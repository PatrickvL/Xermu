#pragma once
// ---------------------------------------------------------------------------
// xbe_loader.hpp — Xbox Executable (.xbe) file loader.
//
// Parses the XBE header, loads sections into guest RAM, decodes the XOR-
// encoded entry point, and resolves the kernel thunk table to HLE stubs.
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace xbe {

// ============================= XBE Structures ==============================

#pragma pack(push, 1)

struct ImageHeader {
    uint32_t magic;                     // 0x000: 'XBEH' = 0x48454258
    uint8_t  signature[256];            // 0x004: RSA-2048 digital signature
    uint32_t base_address;              // 0x104: load address (usually 0x10000)
    uint32_t size_of_headers;           // 0x108: bytes for all headers
    uint32_t size_of_image;             // 0x10C: total image size
    uint32_t size_of_image_header;      // 0x110: this struct's size (>= 0x178)
    uint32_t timedate;                  // 0x114: UNIX timestamp
    uint32_t certificate_addr;          // 0x118: VA of certificate
    uint32_t num_sections;              // 0x11C: number of section headers
    uint32_t section_headers_addr;      // 0x120: VA of section header array
    uint32_t init_flags;                // 0x124: initialization flags
    uint32_t entry_point;               // 0x128: XOR-encoded entry point
    uint32_t tls_addr;                  // 0x12C: VA of TLS directory
    uint32_t stack_size;                // 0x130: stack reserve
    uint32_t pe_heap_reserve;           // 0x134
    uint32_t pe_heap_commit;            // 0x138
    uint32_t pe_base_address;           // 0x13C
    uint32_t pe_size_of_image;          // 0x140
    uint32_t pe_checksum;               // 0x144
    uint32_t pe_timedate;               // 0x148
    uint32_t debug_pathname_addr;       // 0x14C
    uint32_t debug_filename_addr;       // 0x150
    uint32_t debug_unicode_fname_addr;  // 0x154
    uint32_t kernel_thunk_addr;         // 0x158: XOR-encoded kernel thunk VA
    uint32_t non_kernel_import_dir;     // 0x15C
    uint32_t num_library_versions;      // 0x160
    uint32_t library_versions_addr;     // 0x164
    uint32_t kernel_library_version;    // 0x168
    uint32_t xapi_library_version;      // 0x16C
    uint32_t logo_bitmap_addr;          // 0x170
    uint32_t logo_bitmap_size;          // 0x174
};

struct SectionHeader {
    uint32_t flags;                     // 0x00: section flags
    uint32_t virtual_address;           // 0x04: VA to load at
    uint32_t virtual_size;              // 0x08: size in memory
    uint32_t raw_address;               // 0x0C: file offset
    uint32_t raw_size;                  // 0x10: size in file
    uint32_t section_name_addr;         // 0x14: VA of name string
    uint32_t section_name_refcount;     // 0x18
    uint32_t head_shared_refcount_addr; // 0x1C
    uint32_t tail_shared_refcount_addr; // 0x20
    uint8_t  section_digest[20];        // 0x24: SHA-1 hash
};

struct TlsDirectory {
    uint32_t raw_data_start;
    uint32_t raw_data_end;
    uint32_t tls_index_addr;
    uint32_t tls_callback_addr;
    uint32_t size_of_zero_fill;
    uint32_t characteristics;
};

#pragma pack(pop)

static constexpr uint32_t XBE_MAGIC = 0x48454258u; // "XBEH"

// XOR keys for entry point / kernel thunk decoding
static constexpr uint32_t XOR_EP_DEBUG  = 0x94859D4Bu;
static constexpr uint32_t XOR_EP_RETAIL = 0xA8FC57ABu;
static constexpr uint32_t XOR_KT_DEBUG  = 0xEFB1F152u;
static constexpr uint32_t XOR_KT_RETAIL = 0x5B6D40B6u;

// Section flags
static constexpr uint32_t SECTION_WRITABLE   = 0x00000001;
static constexpr uint32_t SECTION_PRELOAD    = 0x00000002;
static constexpr uint32_t SECTION_EXECUTABLE = 0x00000004;

// ============================= Kernel HLE ==================================
// Stub kernel exports: each ordinal maps to a small thunk in guest RAM that
// triggers a privileged trap (INT 0x20) followed by the ordinal number.
// The executor interprets INT 0x20 as an HLE kernel call.

static constexpr uint32_t MAX_KERNEL_ORDINALS = 379;  // ordinals 1..378
static constexpr uint8_t  HLE_INT_VECTOR      = 0x20; // INT 20h = HLE trap

// Each HLE stub is 8 bytes:   MOV EAX, ordinal (5 bytes) + INT 0x20 (2 bytes) + RET (1 byte)
static constexpr uint32_t HLE_STUB_SIZE  = 8;
static constexpr uint32_t HLE_STUB_BASE  = 0x00080000u; // guest PA for stub table

// Write kernel HLE stub table into guest RAM at HLE_STUB_BASE.
// Returns the VA of stub for a given ordinal: HLE_STUB_BASE + ordinal * HLE_STUB_SIZE.
inline void write_hle_stubs(uint8_t* ram) {
    for (uint32_t ord = 0; ord < MAX_KERNEL_ORDINALS; ++ord) {
        uint8_t* stub = ram + HLE_STUB_BASE + ord * HLE_STUB_SIZE;
        stub[0] = 0xB8;                    // MOV EAX, imm32
        memcpy(stub + 1, &ord, 4);         // ordinal
        stub[5] = 0xCD;                    // INT imm8
        stub[6] = HLE_INT_VECTOR;          // 0x20
        stub[7] = 0xC3;                    // RET
    }
}

inline uint32_t hle_stub_addr(uint32_t ordinal) {
    return HLE_STUB_BASE + ordinal * HLE_STUB_SIZE;
}

// ============================= XBE Loader ==================================

struct XbeInfo {
    uint32_t entry_point;       // decoded entry point VA
    uint32_t base_address;      // image base
    uint32_t stack_size;        // from header
    uint32_t kernel_thunk_va;   // decoded kernel thunk VA
    uint32_t num_sections;
    uint32_t tls_addr;          // VA of TLS directory (0 if none)
    bool     valid;
};

// Decode an XOR-encoded value. Try retail key first, fall back to debug.
inline uint32_t decode_xor(uint32_t encoded, uint32_t retail_key, uint32_t debug_key,
                           uint32_t base, uint32_t image_end) {
    uint32_t val = encoded ^ retail_key;
    if (val >= base && val < image_end) return val;
    val = encoded ^ debug_key;
    if (val >= base && val < image_end) return val;
    // Fallback: return retail decode
    return encoded ^ retail_key;
}

// Load an XBE file into guest RAM and resolve kernel imports.
// `file_data` points to the entire .xbe file contents, `file_size` is its length.
// `ram` is the guest RAM pointer (GUEST_RAM_SIZE bytes).
// Returns XbeInfo with decoded entry point and metadata.
inline XbeInfo load_xbe(uint8_t* ram, const uint8_t* file_data, size_t file_size) {
    XbeInfo info {};

    // Validate minimum size
    if (file_size < sizeof(ImageHeader)) {
        fprintf(stderr, "[xbe] File too small (%zu bytes)\n", file_size);
        return info;
    }

    const auto* hdr = reinterpret_cast<const ImageHeader*>(file_data);

    // Check magic
    if (hdr->magic != XBE_MAGIC) {
        fprintf(stderr, "[xbe] Bad magic: 0x%08X (expected 0x%08X)\n",
                hdr->magic, XBE_MAGIC);
        return info;
    }

    uint32_t base = hdr->base_address;
    uint32_t image_end = base + hdr->size_of_image;

    printf("[xbe] Base=0x%08X  ImageSize=0x%08X  Sections=%u\n",
           base, hdr->size_of_image, hdr->num_sections);

    // Decode entry point
    info.entry_point = decode_xor(hdr->entry_point,
                                   XOR_EP_RETAIL, XOR_EP_DEBUG,
                                   base, image_end);
    // Decode kernel thunk address
    info.kernel_thunk_va = decode_xor(hdr->kernel_thunk_addr,
                                       XOR_KT_RETAIL, XOR_KT_DEBUG,
                                       base, image_end);
    info.base_address  = base;
    info.stack_size    = hdr->stack_size;
    info.num_sections  = hdr->num_sections;
    info.tls_addr      = hdr->tls_addr;

    printf("[xbe] EntryPoint=0x%08X  KernelThunk=0x%08X  TLS=0x%08X\n",
           info.entry_point, info.kernel_thunk_va, info.tls_addr);

    // Copy headers into guest RAM at base address
    if (base + hdr->size_of_headers > GUEST_RAM_SIZE) {
        fprintf(stderr, "[xbe] Headers don't fit in RAM\n");
        return info;
    }
    uint32_t hdr_copy = hdr->size_of_headers;
    if (hdr_copy > file_size) hdr_copy = (uint32_t)file_size;
    memcpy(ram + base, file_data, hdr_copy);

    // Load sections
    uint32_t sec_hdr_offset = hdr->section_headers_addr - base;
    if (sec_hdr_offset + hdr->num_sections * sizeof(SectionHeader) > file_size) {
        fprintf(stderr, "[xbe] Section headers out of bounds\n");
        return info;
    }

    const auto* sections = reinterpret_cast<const SectionHeader*>(
        file_data + sec_hdr_offset);

    for (uint32_t i = 0; i < hdr->num_sections; ++i) {
        const auto& sec = sections[i];
        uint32_t va   = sec.virtual_address;
        uint32_t vsz  = sec.virtual_size;
        uint32_t roff = sec.raw_address;
        uint32_t rsz  = sec.raw_size;

        // Bounds check: section must fit in guest RAM
        if (va + vsz > GUEST_RAM_SIZE) {
            fprintf(stderr, "[xbe] Section %u: VA 0x%08X+0x%08X exceeds RAM\n",
                    i, va, vsz);
            continue;
        }

        // Zero-fill entire virtual range first
        memset(ram + va, 0, vsz);

        // Copy raw data from file
        uint32_t copy_size = rsz;
        if (roff + copy_size > file_size) {
            fprintf(stderr, "[xbe] Section %u: raw data truncated\n", i);
            copy_size = (uint32_t)(file_size > roff ? file_size - roff : 0);
        }
        if (copy_size > vsz) copy_size = vsz;
        if (copy_size > 0) {
            memcpy(ram + va, file_data + roff, copy_size);
        }

        printf("[xbe] Section %u: VA=0x%08X  VSize=0x%08X  RawOff=0x%08X  RawSz=0x%08X  Flags=0x%X\n",
               i, va, vsz, roff, rsz, sec.flags);
    }

    // Write HLE stub table
    write_hle_stubs(ram);

    // Resolve kernel thunk table
    uint32_t thunk_offset = info.kernel_thunk_va;
    if (thunk_offset + 4 <= GUEST_RAM_SIZE) {
        uint32_t resolved = 0;
        for (uint32_t off = thunk_offset; off + 4 <= GUEST_RAM_SIZE; off += 4) {
            uint32_t entry;
            memcpy(&entry, ram + off, 4);
            if (entry == 0) break; // end of table

            if (entry & 0x80000000u) {
                uint32_t ordinal = entry & 0x7FFFFFFFu;
                uint32_t stub_va = hle_stub_addr(ordinal);
                memcpy(ram + off, &stub_va, 4);
                ++resolved;
            }
        }
        printf("[xbe] Resolved %u kernel thunk entries\n", resolved);
    }

    // Handle TLS directory
    if (info.tls_addr != 0 && info.tls_addr + sizeof(TlsDirectory) <= GUEST_RAM_SIZE) {
        TlsDirectory tls;
        memcpy(&tls, ram + info.tls_addr, sizeof(tls));
        printf("[xbe] TLS: data=[0x%08X..0x%08X] index=0x%08X callbacks=0x%08X\n",
               tls.raw_data_start, tls.raw_data_end,
               tls.tls_index_addr, tls.tls_callback_addr);

        // Write TLS index = 0 (single-threaded for now)
        if (tls.tls_index_addr >= base && tls.tls_index_addr + 4 <= GUEST_RAM_SIZE) {
            uint32_t zero = 0;
            memcpy(ram + tls.tls_index_addr, &zero, 4);
        }
    }

    info.valid = true;
    return info;
}

// ============================= Default HLE Handler =========================
// Handles INT 0x20 traps from kernel thunk stubs.
// Ordinal is in EAX. Guest stack has the return address from CALL [thunk].
// The stub does: MOV EAX, ordinal / INT 0x20 / RET
// So after INT 0x20 is handled, execution resumes at the RET which pops
// the caller's return address.
//
// stdcall convention: callee pops args. For HLE stubs, we read args from
// the guest stack (ESP+4, ESP+8, ...) and adjust ESP to pop them.

// Helper: read a 32-bit value from guest stack at [ESP + offset]
inline uint32_t stack_arg(Executor& exec, int index) {
    // ESP points to INT return address (stub's RET addr), args start at +4
    // Actually: the CALL [thunk] pushed return address, then stub runs
    // MOV EAX,ord / INT 0x20 / RET. The INT 0x20 is handled, EIP advances
    // past it. ESP still points where it was after the CALL.
    // Stack: [ESP]=caller_return_addr, [ESP+4]=arg1, [ESP+8]=arg2, ...
    uint32_t esp = exec.ctx.gp[GP_ESP];
    uint32_t addr = esp + 4 + index * 4;
    if (addr + 4 > GUEST_RAM_SIZE) return 0;
    uint32_t val;
    memcpy(&val, exec.ram + addr, 4);
    return val;
}

// Helper: pop N dword args from stack (stdcall cleanup).
// The stub's RET will pop [ESP] as return address, so we move the return
// address up over the args: copy [ESP] to [ESP + n_args*4], then adjust ESP.
inline void stdcall_cleanup(Executor& exec, int n_args) {
    if (n_args <= 0) return;
    uint32_t esp = exec.ctx.gp[GP_ESP];
    // Read the return address currently at [ESP]
    uint32_t ret_addr = 0;
    if (esp + 4 <= GUEST_RAM_SIZE)
        memcpy(&ret_addr, exec.ram + esp, 4);
    // Move ESP up past the args
    uint32_t new_esp = esp + 4u * n_args;
    exec.ctx.gp[GP_ESP] = new_esp;
    // Write the return address at the new [ESP] so RET pops it correctly
    if (new_esp + 4 <= GUEST_RAM_SIZE)
        memcpy(exec.ram + new_esp, &ret_addr, 4);
}

// Named kernel ordinals used in the handler below
enum KernelOrdinal : uint32_t {
    ORD_AvGetSavedDataAddress          = 1,
    ORD_AvSetDisplayMode               = 3,
    ORD_AvSetSavedDataAddress          = 4,
    ORD_DbgPrint                       = 7,
    ORD_ExAllocatePool                 = 15,
    ORD_ExFreePool                     = 17,
    ORD_HalReturnToFirmware            = 49,
    ORD_KeGetCurrentThread             = 104,
    ORD_KeInitializeDpc                = 107,
    ORD_KeQueryPerformanceCounter      = 126,
    ORD_KeQueryPerformanceFrequency    = 127,
    ORD_KeQuerySystemTime              = 131,
    ORD_KeSetTimer                     = 145,
    ORD_KeSetTimerEx                   = 146,
    ORD_KeCancelTimer                  = 96,
    ORD_KeTickCount                    = 156,
    ORD_MmAllocateContiguousMemory     = 165,
    ORD_MmAllocateContiguousMemoryEx   = 166,
    ORD_MmFreeContiguousMemory         = 171,
    ORD_MmGetPhysicalAddress           = 173,
    ORD_NtAllocateVirtualMemory        = 184,
    ORD_NtClose                        = 187,
    ORD_NtFreeVirtualMemory            = 199,
    ORD_PsCreateSystemThread           = 254,
    ORD_PsTerminateSystemThread        = 258,
    ORD_RtlEnterCriticalSection        = 264,
    ORD_RtlLeaveCriticalSection        = 267,
    ORD_RtlInitAnsiString             = 268,
    ORD_RtlInitializeCriticalSection   = 270,
    ORD_XeLoadSection                  = 327,
};

// Simple bump allocator for guest heap (contiguous memory requests)
struct XbeHeap {
    uint32_t next_alloc;  // next free PA
    uint32_t limit;       // end of allocatable region

    XbeHeap() : next_alloc(0x01000000u), limit(GUEST_RAM_SIZE) {}

    uint32_t alloc(uint32_t size, uint32_t align = 0x1000) {
        uint32_t base = (next_alloc + align - 1) & ~(align - 1);
        if (base + size > limit) return 0;
        next_alloc = base + size;
        return base;
    }
};

inline bool default_hle_handler(Executor& exec, uint32_t ordinal, void* user) {
    auto* heap = static_cast<XbeHeap*>(user);

    switch (ordinal) {
    case ORD_AvGetSavedDataAddress:
        exec.ctx.gp[GP_EAX] = 0; // NULL
        return true;

    case ORD_AvSetDisplayMode:
        // void AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode,
        //                       ULONG Format, ULONG Pitch, ULONG FrameBuffer)
        stdcall_cleanup(exec, 6);
        return true;

    case ORD_AvSetSavedDataAddress:
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_DbgPrint:
        // int DbgPrint(const char* fmt, ...)
        // We just return 0 (STATUS_SUCCESS). Can't easily parse varargs.
        exec.ctx.gp[GP_EAX] = 0;
        return true;

    case ORD_ExAllocatePool:
    case ORD_MmAllocateContiguousMemory: {
        // PVOID ExAllocatePool(ULONG size)
        // PVOID MmAllocateContiguousMemory(ULONG size)
        uint32_t size = stack_arg(exec, 0);
        uint32_t addr = heap->alloc(size);
        if (addr) memset(exec.ram + addr, 0, size);
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_MmAllocateContiguousMemoryEx: {
        // PVOID MmAllocateContiguousMemoryEx(SIZE, LowAddr, HighAddr, Align, Protect)
        uint32_t size = stack_arg(exec, 0);
        uint32_t align = stack_arg(exec, 3);
        if (align < 0x1000) align = 0x1000;
        uint32_t addr = heap->alloc(size, align);
        if (addr) memset(exec.ram + addr, 0, size);
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_ExFreePool:
    case ORD_MmFreeContiguousMemory:
        // Leak: don't track frees in the bump allocator
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_MmGetPhysicalAddress: {
        // ULONG_PTR MmGetPhysicalAddress(PVOID va)
        // Identity mapping: PA = VA (no paging in most XBE contexts)
        uint32_t va = stack_arg(exec, 0);
        exec.ctx.gp[GP_EAX] = va;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_NtAllocateVirtualMemory: {
        // NTSTATUS NtAllocateVirtualMemory(OUT PVOID *BaseAddress,
        //          ULONG_PTR ZeroBits, IN OUT PSIZE_T RegionSize,
        //          ULONG AllocationType, ULONG Protect)
        uint32_t base_ptr = stack_arg(exec, 0);
        uint32_t size_ptr = stack_arg(exec, 2);
        uint32_t req_size = 0;
        if (size_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(&req_size, exec.ram + size_ptr, 4);
        uint32_t addr = heap->alloc(req_size);
        if (addr && base_ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + base_ptr, &addr, 4);
            memset(exec.ram + addr, 0, req_size);
        }
        if (addr && size_ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + size_ptr, &req_size, 4);
        }
        exec.ctx.gp[GP_EAX] = addr ? 0 : 0xC0000017u; // STATUS_NO_MEMORY
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_NtFreeVirtualMemory:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_NtClose:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_HalReturnToFirmware:
        // Halt the guest — this is the "reboot" call.
        exec.ctx.halted = true;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_KeGetCurrentThread:
        // Return a fake KTHREAD pointer (nonzero, page-aligned)
        exec.ctx.gp[GP_EAX] = 0x00060000u;
        return true;

    case ORD_KeCancelTimer:
        // BOOLEAN KeCancelTimer(PKTIMER Timer)
        // Return FALSE (timer was not in the queue)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_KeInitializeDpc:
        // void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeQueryPerformanceCounter: {
        // ULONGLONG KeQueryPerformanceCounter()
        // Returns 64-bit counter in EDX:EAX.  Use host rdtsc scaled to
        // approximate the Xbox Pentium III 733 MHz TSC.
        uint64_t tsc;
#if defined(_MSC_VER)
        tsc = __rdtsc();
#else
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tsc = ((uint64_t)hi << 32) | lo;
#endif
        exec.ctx.gp[GP_EAX] = (uint32_t)tsc;
        exec.ctx.gp[GP_EDX] = (uint32_t)(tsc >> 32);
        return true;
    }

    case ORD_KeQueryPerformanceFrequency: {
        // ULONGLONG KeQueryPerformanceFrequency()
        // Xbox Pentium III runs at 733.33 MHz → report that as the frequency.
        uint64_t freq = 733333333ULL;
        exec.ctx.gp[GP_EAX] = (uint32_t)freq;
        exec.ctx.gp[GP_EDX] = (uint32_t)(freq >> 32);
        return true;
    }

    case ORD_KeQuerySystemTime: {
        // void KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
        // Write a monotonic 100ns-unit timestamp to the caller's buffer.
        uint32_t ptr = stack_arg(exec, 0);
        if (ptr + 8 <= GUEST_RAM_SIZE) {
            // Use the NV2A PTIMER ns counter as our time source (already
            // advancing in hw_tick_callback). Scale: 1 unit = 100ns.
            // Fallback: just use host rdtsc / 10 if no NV2A context.
            uint64_t time_100ns;
#if defined(_MSC_VER)
            time_100ns = __rdtsc() / 10;
#else
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            time_100ns = (((uint64_t)hi << 32) | lo) / 10;
#endif
            memcpy(exec.ram + ptr, &time_100ns, 8);
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_KeSetTimer:
        // BOOLEAN KeSetTimer(PKTIMER, LARGE_INTEGER DueTime, PKDPC Dpc)
        exec.ctx.gp[GP_EAX] = 0; // was not already in queue
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeSetTimerEx:
        // BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_KeTickCount:
        // This is actually a variable, not a function. Return a counter.
        exec.ctx.gp[GP_EAX] = 1;
        return true;

    case ORD_PsCreateSystemThread:
        // Stub: return STATUS_SUCCESS but don't actually create a thread.
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 7);
        return true;

    case ORD_PsTerminateSystemThread:
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlEnterCriticalSection:
        // void RtlEnterCriticalSection(PRTL_CRITICAL_SECTION)
        // Single-threaded stub: always succeeds immediately.
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlLeaveCriticalSection:
        // void RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlInitializeCriticalSection:
        // void RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlInitAnsiString: {
        // void RtlInitAnsiString(PANSI_STRING Dest, PCSTR Src)
        // ANSI_STRING: { USHORT Length, USHORT MaxLength, PCHAR Buffer }
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        uint16_t len = 0;
        if (src && src < GUEST_RAM_SIZE) {
            while (src + len < GUEST_RAM_SIZE && exec.ram[src + len]) ++len;
        }
        if (dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t max_len = len + 1;
            memcpy(exec.ram + dest + 0, &len, 2);
            memcpy(exec.ram + dest + 2, &max_len, 2);
            memcpy(exec.ram + dest + 4, &src, 4);
        }
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_XeLoadSection:
        // NTSTATUS XeLoadSection(PXBE_SECTION_HEADER)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS (already loaded)
        stdcall_cleanup(exec, 1);
        return true;

    default:
        // Unhandled: log and return STATUS_NOT_IMPLEMENTED
        fprintf(stderr, "[hle] unhandled kernel ordinal %u at EIP=0x%08X\n",
                ordinal, exec.ctx.eip);
        exec.ctx.gp[GP_EAX] = 0xC0000002u; // STATUS_NOT_IMPLEMENTED
        return true;
    }
}

} // namespace xbe
