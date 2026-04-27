#pragma once
// ---------------------------------------------------------------------------
// xbe_loader.hpp â€” Xbox Executable (.xbe) file loader.
//
// Parses the XBE header, loads sections into guest RAM, decodes the XOR-
// encoded entry point, and resolves the kernel thunk table to HLE stubs.
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
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

// Default base addresses for HLE stubs and kernel data exports.
// These are recalculated by load_xbe() to sit above the XBE image so they
// never overlap with any section.  The defaults are only used by tests that
// don't load a full XBE.
inline uint32_t HLE_STUB_BASE  = 0x00080000u;
inline uint32_t KDATA_BASE     = 0x00081000u;

// Kernel data area layout (offsets from KDATA_BASE)
// The area is 0x1000 bytes (4KB page) to hold all data exports.
static constexpr uint32_t KDATA_SIZE                         = 0x1000;
static constexpr uint32_t KDATA_OFF_KeTickCount              = 0x000; // DWORD
static constexpr uint32_t KDATA_OFF_XboxHardwareInfo         = 0x004; // 8 bytes
static constexpr uint32_t KDATA_OFF_XboxKrnlVersion          = 0x00C; // 8 bytes
static constexpr uint32_t KDATA_OFF_LaunchDataPage           = 0x014; // DWORD ptr
static constexpr uint32_t KDATA_OFF_XeImageFileName          = 0x018; // ANSI_STRING
static constexpr uint32_t KDATA_OFF_XeImageFileNameBuf       = 0x020; // 64 bytes
static constexpr uint32_t KDATA_OFF_HalDiskCachePartCount    = 0x060; // DWORD
static constexpr uint32_t KDATA_OFF_HalDiskModelNumber       = 0x064; // ANSI_STRING
static constexpr uint32_t KDATA_OFF_HalDiskModelNumberBuf    = 0x06C; // 40 bytes
static constexpr uint32_t KDATA_OFF_HalDiskSerialNumber      = 0x094; // ANSI_STRING
static constexpr uint32_t KDATA_OFF_HalDiskSerialNumberBuf   = 0x09C; // 20 bytes
static constexpr uint32_t KDATA_OFF_KdDebuggerEnabled        = 0x0B0; // BOOLEAN
static constexpr uint32_t KDATA_OFF_KdDebuggerNotPresent     = 0x0B1; // BOOLEAN
static constexpr uint32_t KDATA_OFF_KeInterruptTime          = 0x0B4; // 12 bytes
static constexpr uint32_t KDATA_OFF_KeSystemTime             = 0x0C0; // 12 bytes
static constexpr uint32_t KDATA_OFF_KeTimeIncrement          = 0x0CC; // DWORD
static constexpr uint32_t KDATA_OFF_KiBugCheckData           = 0x0D0; // 20 bytes
static constexpr uint32_t KDATA_OFF_HalBootSMCVideoMode      = 0x0E4; // DWORD
static constexpr uint32_t KDATA_OFF_IdexChannelObject        = 0x0E8; // 128 bytes
static constexpr uint32_t KDATA_OFF_MmGlobalData             = 0x168; // 32 bytes
static constexpr uint32_t KDATA_OFF_ExEventObjectType        = 0x188; // 32 bytes
static constexpr uint32_t KDATA_OFF_ExMutantObjectType       = 0x1A8; // 32 bytes
static constexpr uint32_t KDATA_OFF_ExSemaphoreObjectType    = 0x1C8; // 32 bytes
static constexpr uint32_t KDATA_OFF_ExTimerObjectType        = 0x1E8; // 32 bytes
static constexpr uint32_t KDATA_OFF_IoCompletionObjectType   = 0x208; // 32 bytes
static constexpr uint32_t KDATA_OFF_IoDeviceObjectType       = 0x228; // 32 bytes
static constexpr uint32_t KDATA_OFF_IoFileObjectType         = 0x248; // 32 bytes
static constexpr uint32_t KDATA_OFF_ObDirectoryObjectType    = 0x268; // 32 bytes
static constexpr uint32_t KDATA_OFF_ObpObjectHandleTable     = 0x288; // 32 bytes
static constexpr uint32_t KDATA_OFF_ObSymbolicLinkObjectType = 0x2A8; // 32 bytes
static constexpr uint32_t KDATA_OFF_PsThreadObjectType       = 0x2C8; // 32 bytes
static constexpr uint32_t KDATA_OFF_XboxEEPROMKey            = 0x2E8; // 16 bytes
static constexpr uint32_t KDATA_OFF_XboxHDKey                = 0x2F8; // 16 bytes
static constexpr uint32_t KDATA_OFF_XboxSignatureKey         = 0x308; // 16 bytes
static constexpr uint32_t KDATA_OFF_XboxLANKey               = 0x318; // 16 bytes
static constexpr uint32_t KDATA_OFF_XboxAltSigKeys           = 0x328; // 256 bytes
static constexpr uint32_t KDATA_OFF_XePublicKeyData          = 0x428; // 284 bytes

// Convenience accessors (use current KDATA_BASE)
inline uint32_t KDATA_KeTickCount()              { return KDATA_BASE + KDATA_OFF_KeTickCount; }
inline uint32_t KDATA_XboxHardwareInfo()         { return KDATA_BASE + KDATA_OFF_XboxHardwareInfo; }
inline uint32_t KDATA_XboxKrnlVersion()          { return KDATA_BASE + KDATA_OFF_XboxKrnlVersion; }
inline uint32_t KDATA_LaunchDataPage()           { return KDATA_BASE + KDATA_OFF_LaunchDataPage; }
inline uint32_t KDATA_XeImageFileName()          { return KDATA_BASE + KDATA_OFF_XeImageFileName; }
inline uint32_t KDATA_XeImageFileNameBuf()       { return KDATA_BASE + KDATA_OFF_XeImageFileNameBuf; }
inline uint32_t KDATA_HalDiskCachePartCount()    { return KDATA_BASE + KDATA_OFF_HalDiskCachePartCount; }
inline uint32_t KDATA_HalDiskModelNumber()       { return KDATA_BASE + KDATA_OFF_HalDiskModelNumber; }
inline uint32_t KDATA_HalDiskModelNumberBuf()    { return KDATA_BASE + KDATA_OFF_HalDiskModelNumberBuf; }
inline uint32_t KDATA_HalDiskSerialNumber()      { return KDATA_BASE + KDATA_OFF_HalDiskSerialNumber; }
inline uint32_t KDATA_HalDiskSerialNumberBuf()   { return KDATA_BASE + KDATA_OFF_HalDiskSerialNumberBuf; }
inline uint32_t KDATA_KdDebuggerEnabled()        { return KDATA_BASE + KDATA_OFF_KdDebuggerEnabled; }
inline uint32_t KDATA_KdDebuggerNotPresent()     { return KDATA_BASE + KDATA_OFF_KdDebuggerNotPresent; }
inline uint32_t KDATA_KeInterruptTime()          { return KDATA_BASE + KDATA_OFF_KeInterruptTime; }
inline uint32_t KDATA_KeSystemTime()             { return KDATA_BASE + KDATA_OFF_KeSystemTime; }
inline uint32_t KDATA_KeTimeIncrement()          { return KDATA_BASE + KDATA_OFF_KeTimeIncrement; }
inline uint32_t KDATA_KiBugCheckData()           { return KDATA_BASE + KDATA_OFF_KiBugCheckData; }
inline uint32_t KDATA_HalBootSMCVideoMode()      { return KDATA_BASE + KDATA_OFF_HalBootSMCVideoMode; }
inline uint32_t KDATA_IdexChannelObject()        { return KDATA_BASE + KDATA_OFF_IdexChannelObject; }
inline uint32_t KDATA_MmGlobalData()             { return KDATA_BASE + KDATA_OFF_MmGlobalData; }
inline uint32_t KDATA_ExEventObjectType()        { return KDATA_BASE + KDATA_OFF_ExEventObjectType; }
inline uint32_t KDATA_ExMutantObjectType()       { return KDATA_BASE + KDATA_OFF_ExMutantObjectType; }
inline uint32_t KDATA_ExSemaphoreObjectType()    { return KDATA_BASE + KDATA_OFF_ExSemaphoreObjectType; }
inline uint32_t KDATA_ExTimerObjectType()        { return KDATA_BASE + KDATA_OFF_ExTimerObjectType; }
inline uint32_t KDATA_IoCompletionObjectType()   { return KDATA_BASE + KDATA_OFF_IoCompletionObjectType; }
inline uint32_t KDATA_IoDeviceObjectType()       { return KDATA_BASE + KDATA_OFF_IoDeviceObjectType; }
inline uint32_t KDATA_IoFileObjectType()         { return KDATA_BASE + KDATA_OFF_IoFileObjectType; }
inline uint32_t KDATA_ObDirectoryObjectType()    { return KDATA_BASE + KDATA_OFF_ObDirectoryObjectType; }
inline uint32_t KDATA_ObpObjectHandleTable()     { return KDATA_BASE + KDATA_OFF_ObpObjectHandleTable; }
inline uint32_t KDATA_ObSymbolicLinkObjectType() { return KDATA_BASE + KDATA_OFF_ObSymbolicLinkObjectType; }
inline uint32_t KDATA_PsThreadObjectType()       { return KDATA_BASE + KDATA_OFF_PsThreadObjectType; }
inline uint32_t KDATA_XboxEEPROMKey()            { return KDATA_BASE + KDATA_OFF_XboxEEPROMKey; }
inline uint32_t KDATA_XboxHDKey()                { return KDATA_BASE + KDATA_OFF_XboxHDKey; }
inline uint32_t KDATA_XboxSignatureKey()         { return KDATA_BASE + KDATA_OFF_XboxSignatureKey; }
inline uint32_t KDATA_XboxLANKey()               { return KDATA_BASE + KDATA_OFF_XboxLANKey; }
inline uint32_t KDATA_XboxAltSigKeys()           { return KDATA_BASE + KDATA_OFF_XboxAltSigKeys; }
inline uint32_t KDATA_XePublicKeyData()          { return KDATA_BASE + KDATA_OFF_XePublicKeyData; }

// Forward declarations (defined after KernelOrdinal enum below)
inline uint32_t kernel_data_addr(uint32_t ordinal);
inline void init_kernel_data(uint8_t* ram);

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
    uint32_t image_size;        // from header (size_of_image)
    uint32_t stack_size;        // from header
    uint32_t kernel_thunk_va;   // decoded kernel thunk VA
    uint32_t num_sections;
    uint32_t tls_addr;          // VA of TLS directory (0 if none)
    uint32_t hle_stub_base;     // where HLE stubs were placed
    uint32_t kdata_base;        // where kernel data exports were placed
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

    // Place HLE stubs and kernel data above the XBE image to avoid overlap.
    uint32_t stubs_needed = MAX_KERNEL_ORDINALS * HLE_STUB_SIZE; // ~3 KB
    uint32_t hle_base = (image_end + 0xFFFu) & ~0xFFFu;         // page-align
    if (hle_base + stubs_needed + 0x1000 < GUEST_RAM_SIZE) {
        HLE_STUB_BASE = hle_base;
        KDATA_BASE    = (hle_base + stubs_needed + 0xFFFu) & ~0xFFFu;
    }
    info.hle_stub_base = HLE_STUB_BASE;
    info.kdata_base    = KDATA_BASE;

    printf("[xbe] HLE stubs at 0x%08X  KDATA at 0x%08X\n", HLE_STUB_BASE, KDATA_BASE);

    // Write HLE stub table
    write_hle_stubs(ram);

    // Initialize kernel data exports
    init_kernel_data(ram);

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
                // Data exports get the address of the actual data;
                // function exports get the address of the HLE stub.
                uint32_t data_va = kernel_data_addr(ordinal);
                uint32_t resolved_va = data_va ? data_va : hle_stub_addr(ordinal);
                memcpy(ram + off, &resolved_va, 4);
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


} // namespace xbe
