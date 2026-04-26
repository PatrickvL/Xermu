#pragma once
// ---------------------------------------------------------------------------
// hle_kernel.hpp — Xbox kernel HLE (High-Level Emulation) stubs.
//
// Implements the default INT 0x20 handler for kernel thunk ordinals,
// including memory allocation, synchronisation primitives, I/O manager,
// Rtl string/memory helpers, interlocked operations, and system threads.
// ---------------------------------------------------------------------------

#include "xbe_loader.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

namespace xbe {

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
// Reference: https://xboxdevwiki.net/Kernel#Kernel_exports
enum KernelOrdinal : uint32_t {
    ORD_AvGetSavedDataAddress          = 1,
    ORD_AvSendTVEncoderOption          = 2,
    ORD_AvSetDisplayMode               = 3,
    ORD_AvSetSavedDataAddress          = 4,
    ORD_DbgBreakPoint                  = 5,
    ORD_DbgPrint                       = 8,
    ORD_HalReadSMCTrayState            = 9,
    ORD_ExAcquireReadWriteLockExclusive= 12,
    ORD_ExAcquireReadWriteLockShared   = 13,
    ORD_ExAllocatePool                 = 14,
    ORD_ExAllocatePoolWithTag          = 15,
    ORD_ExFreePool                     = 17,
    ORD_ExInitializeReadWriteLock      = 18,
    ORD_ExQueryPoolBlockSize           = 23,
    ORD_ExQueryNonVolatileSetting      = 24,
    ORD_ExSaveNonVolatileSetting       = 29,
    ORD_FscSetCacheSize                 = 37,
    ORD_HalGetInterruptVector          = 44,
    ORD_HalReadSMBusValue              = 45,
    ORD_HalReadWritePCISpace           = 46,
    ORD_HalRegisterShutdownNotification= 47,
    ORD_HalReturnToFirmware            = 49,
    ORD_HalWriteSMBusValue             = 50,
    ORD_InterlockedCompareExchange     = 51,
    ORD_InterlockedDecrement           = 52,
    ORD_InterlockedIncrement           = 53,
    ORD_InterlockedExchange            = 54,
    ORD_InterlockedExchangeAdd         = 55,
    ORD_IoCreateDevice                 = 65,
    ORD_IoCreateFile                   = 66,
    ORD_IoCreateSymbolicLink           = 67,
    ORD_IoDeleteDevice                 = 68,
    ORD_IoDeleteSymbolicLink           = 69,
    ORD_IoDismountVolume               = 90,
    ORD_IoDismountVolumeByName         = 91,
    ORD_KeBugCheck                     = 95,
    ORD_KeBugCheckEx                   = 96,
    ORD_KeCancelTimer                  = 97,
    ORD_KeConnectInterrupt             = 98,
    ORD_KeDelayExecutionThread         = 99,
    ORD_KeEnterCriticalRegion          = 101,
    ORD_KeGetCurrentIrql               = 103,
    ORD_KeGetCurrentThread             = 104,
    ORD_KeInitializeApc                = 105,
    ORD_KeInitializeDpc                = 107,
    ORD_KeInitializeEvent              = 108,
    ORD_KeInitializeInterrupt          = 109,
    ORD_KeInitializeMutant             = 110,
    ORD_KeInitializeQueue              = 111,
    ORD_KeInitializeSemaphore          = 112,
    ORD_KeInitializeTimerEx            = 113,
    ORD_KeInsertQueueDpc               = 119,
    ORD_KeLeaveCriticalRegion          = 122,
    ORD_KeQueryPerformanceCounter      = 126,
    ORD_KeQueryPerformanceFrequency    = 127,
    ORD_KeQuerySystemTime              = 128,
    ORD_KeRaiseIrqlToDpcLevel          = 129,
    ORD_KeReleaseMutant                = 131,
    ORD_KeReleaseSemaphore             = 132,
    ORD_KeResetEvent                   = 138,
    ORD_KeSetBasePriorityThread        = 143,
    ORD_KeSetEvent                     = 145,
    ORD_KeSetTimer                     = 149,
    ORD_KeSetTimerEx                   = 150,
    ORD_KeStallExecutionProcessor      = 151,
    ORD_KeTickCount                    = 156,
    ORD_KeWaitForMultipleObjects       = 158,
    ORD_KeWaitForSingleObject          = 159,
    ORD_KfRaiseIrql                    = 160,
    ORD_KfLowerIrql                    = 161,
    ORD_KiUnlockDispatcherDatabase     = 163,
    ORD_LaunchDataPage                 = 164,
    ORD_MmAllocateContiguousMemory     = 165,
    ORD_MmAllocateContiguousMemoryEx   = 166,
    ORD_MmAllocateSystemMemory         = 167,
    ORD_MmClaimGpuInstanceMemory       = 168,
    ORD_MmCreateKernelStack            = 169,
    ORD_MmDeleteKernelStack            = 170,
    ORD_MmFreeContiguousMemory         = 171,
    ORD_MmFreeSystemMemory             = 172,
    ORD_MmGetPhysicalAddress           = 173,
    ORD_MmIsAddressValid               = 174,
    ORD_MmLockUnlockBufferPages        = 175,
    ORD_MmMapIoSpace                   = 177,
    ORD_MmQueryStatistics              = 181,
    ORD_MmSetAddressProtect            = 182,
    ORD_MmUnmapIoSpace                 = 183,
    ORD_NtAllocateVirtualMemory        = 184,
    ORD_NtClearEvent                   = 186,
    ORD_NtClose                        = 187,
    ORD_NtCreateEvent                  = 189,
    ORD_NtCreateFile                   = 190,
    ORD_NtCreateMutant                 = 192,
    ORD_NtCreateSemaphore              = 193,
    ORD_NtDeviceIoControlFile           = 196,
    ORD_NtFreeVirtualMemory            = 199,
    ORD_NtOpenFile                     = 202,
    ORD_NtQueryInformationFile         = 211,
    ORD_NtQueryFullAttributesFile      = 210,
    ORD_NtQueryVolumeInformationFile   = 218,
    ORD_NtReadFile                     = 219,
    ORD_NtSetEvent                     = 225,
    ORD_NtSetInformationFile           = 226,
    ORD_NtWaitForSingleObject          = 233,
    ORD_NtWaitForSingleObjectEx        = 234,
    ORD_NtWaitForMultipleObjectsEx     = 235,
    ORD_NtWriteFile                    = 236,
    ORD_NtYieldExecution               = 238,
    ORD_ObReferenceObjectByHandle      = 246,
    ORD_ObfDereferenceObject           = 250,
    ORD_ObfReferenceObject             = 251,
    ORD_PsCreateSystemThread           = 254,
    ORD_PsCreateSystemThreadEx         = 255,
    ORD_PsTerminateSystemThread        = 258,
    ORD_RtlAnsiStringToUnicodeString   = 260,
    ORD_RtlCompareMemory               = 268,
    ORD_RtlCopyUnicodeString           = 273,
    ORD_RtlCreateUnicodeString         = 274,
    ORD_RtlEnterCriticalSection        = 277,
    ORD_RtlEnterCriticalSectionAndRegion=278,
    ORD_RtlEqualString                 = 279,
    ORD_RtlFillMemory                  = 284,
    ORD_RtlFreeAnsiString             = 286,
    ORD_RtlFreeUnicodeString          = 287,
    ORD_RtlInitAnsiString             = 289,
    ORD_RtlInitUnicodeString           = 290,
    ORD_RtlInitializeCriticalSection   = 291,
    ORD_RtlLeaveCriticalSection        = 294,
    ORD_RtlLeaveCriticalSectionAndRegion=295,
    ORD_RtlMoveMemory                  = 298,
    ORD_RtlNtStatusToDosError          = 301,
    ORD_RtlTryEnterCriticalSection     = 306,
    ORD_RtlUnicodeStringToAnsiString   = 308,
    ORD_RtlZeroMemory                  = 320,
    ORD_XboxHardwareInfo               = 322,
    ORD_XboxKrnlVersion                = 324,
    ORD_XeImageFileName                = 326,
    ORD_XeLoadSection                  = 327,
    ORD_XeUnloadSection                = 328,
    ORD_HalBootSMCVideoMode            = 356,
};

// Returns the guest VA for a data export ordinal, or 0 if it's not a data export
inline uint32_t kernel_data_addr(uint32_t ordinal) {
    switch (ordinal) {
    case ORD_KeTickCount:       return KDATA_KeTickCount;
    case ORD_XboxHardwareInfo:  return KDATA_XboxHardwareInfo;
    case ORD_XboxKrnlVersion:   return KDATA_XboxKrnlVersion;
    case ORD_LaunchDataPage:    return KDATA_LaunchDataPage;
    case ORD_XeImageFileName:   return KDATA_XeImageFileName;
    default:                    return 0;
    }
}

// Initialize kernel data exports in guest RAM
inline void init_kernel_data(uint8_t* ram) {
    // Zero the area first
    memset(ram + KDATA_BASE, 0, 0x120);

    // KeTickCount: start at 1
    uint32_t tick = 1;
    memcpy(ram + KDATA_KeTickCount, &tick, 4);

    // XboxHardwareInfo: Flags=0x20 (INTERNAL_USB_HUB), GpuRevision=0xA1, McpRevision=0xD4
    uint32_t hw_flags = 0x00000020u;
    memcpy(ram + KDATA_XboxHardwareInfo, &hw_flags, 4);
    ram[KDATA_XboxHardwareInfo + 4] = 0xA1; // GPU revision (NV2A A1)
    ram[KDATA_XboxHardwareInfo + 5] = 0xD4; // MCP revision (X3)

    // XboxKrnlVersion: 1.0.5838.1
    uint16_t ver[4] = { 1, 0, 5838, 1 };
    memcpy(ram + KDATA_XboxKrnlVersion, ver, 8);

    // LaunchDataPage: NULL (no launch data)
    uint32_t null_ptr = 0;
    memcpy(ram + KDATA_LaunchDataPage, &null_ptr, 4);

    // XeImageFileName: "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe"
    const char* img_name = "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe";
    uint16_t name_len = (uint16_t)strlen(img_name);
    uint16_t max_len = name_len + 1;
    memcpy(ram + KDATA_XeImageFileName, &name_len, 2);
    memcpy(ram + KDATA_XeImageFileName + 2, &max_len, 2);
    uint32_t buf_addr = KDATA_XeImageFileNameBuf;
    memcpy(ram + KDATA_XeImageFileName + 4, &buf_addr, 4);
    memcpy(ram + KDATA_XeImageFileNameBuf, img_name, name_len + 1);
}

// Simple bump allocator for guest heap (contiguous memory requests)
struct PendingThread {
    uint32_t start_routine;
    uint32_t start_context;
};

// Pending DPC (Deferred Procedure Call) from KeSetTimer/KeSetTimerEx
struct PendingDpc {
    uint32_t routine;     // PKDEFERRED_ROUTINE
    uint32_t context;     // DeferredContext
    uint32_t dpc_va;      // VA of the KDPC object (Arg1 to routine)
    uint32_t timer_va;    // VA of the KTIMER object (SystemArgument1)
};

// Host-backed file handle for guest I/O
struct HostFile {
    FILE* fp;
    uint64_t size;
    std::string host_path;
};

struct XbeHeap {
    uint32_t next_alloc;  // next free PA
    uint32_t limit;       // end of allocatable region
    uint32_t next_handle; // fake handle counter

    // Pending threads created by PsCreateSystemThread(Ex)
    std::vector<PendingThread> pending_threads;

    // Pending DPCs queued by KeSetTimer/KeSetTimerEx
    std::vector<PendingDpc> pending_dpcs;

    // Host-backed file system
    std::string xbe_directory;  // host directory containing the XBE
    std::unordered_map<uint32_t, HostFile> open_files;

    // Mount points: Xbox device path prefix → host directory
    struct MountPoint {
        std::string xbox_prefix;
        std::string host_dir;
    };
    std::vector<MountPoint> mounts;

    XbeHeap() : next_alloc(0x01000000u), limit(GUEST_RAM_SIZE), next_handle(0x100) {}

    uint32_t alloc(uint32_t size, uint32_t align = 0x1000) {
        uint32_t base = (next_alloc + align - 1) & ~(align - 1);
        if (base + size > limit) return 0;
        next_alloc = base + size;
        return base;
    }

    // Set up default mount points based on the XBE file path
    void set_xbe_path(const std::string& xbe_path) {
        // Extract directory from XBE path
        size_t last_sep = xbe_path.find_last_of("/\\");
        xbe_directory = (last_sep != std::string::npos) ? xbe_path.substr(0, last_sep) : ".";

        // Default mounts: Xbox partition 2 = dashboard directory
        mounts.push_back({"\\Device\\Harddisk0\\Partition2", xbe_directory});
        mounts.push_back({"\\??\\C:", xbe_directory});
        mounts.push_back({"\\??\\D:", xbe_directory}); // D: also commonly used
    }

    // Translate Xbox path to host path
    std::string translate_path(const std::string& xbox_path) const {
        for (auto& m : mounts) {
            if (xbox_path.size() > m.xbox_prefix.size() &&
                _strnicmp(xbox_path.c_str(), m.xbox_prefix.c_str(), m.xbox_prefix.size()) == 0 &&
                (xbox_path[m.xbox_prefix.size()] == '\\' || xbox_path[m.xbox_prefix.size()] == '/')) {
                std::string rel = xbox_path.substr(m.xbox_prefix.size() + 1);
                // Convert backslashes to forward slashes
                for (auto& c : rel) if (c == '\\') c = '/';
                return m.host_dir + "/" + rel;
            }
        }
        return ""; // no mount matched
    }

    // Open a host file, return handle or 0 on failure
    uint32_t open_host_file(const std::string& host_path) {
        FILE* fp = fopen(host_path.c_str(), "rb");
        if (!fp) return 0;
        fseek(fp, 0, SEEK_END);
        uint64_t sz = (uint64_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint32_t h = next_handle++;
        open_files[h] = {fp, sz, host_path};
        return h;
    }

    // Close a host file by handle, returns true if it was a real file
    bool close_host_file(uint32_t handle) {
        auto it = open_files.find(handle);
        if (it == open_files.end()) return false;
        fclose(it->second.fp);
        open_files.erase(it);
        return true;
    }

    // Look up a host file by handle
    HostFile* get_host_file(uint32_t handle) {
        auto it = open_files.find(handle);
        return (it != open_files.end()) ? &it->second : nullptr;
    }

    ~XbeHeap() {
        for (auto& [h, f] : open_files)
            if (f.fp) fclose(f.fp);
    }

    // Non-copyable (owns FILE* handles)
    XbeHeap(const XbeHeap&) = delete;
    XbeHeap& operator=(const XbeHeap&) = delete;
    XbeHeap(XbeHeap&&) = default;
    XbeHeap& operator=(XbeHeap&&) = default;

    void reset() {
        for (auto& [h, f] : open_files)
            if (f.fp) fclose(f.fp);
        open_files.clear();
        pending_threads.clear();
        mounts.clear();
        xbe_directory.clear();
        next_alloc = 0x01000000u;
        limit = GUEST_RAM_SIZE;
        next_handle = 0x100;
    }
};

// Helper: read an ANSI string path from guest OBJECT_ATTRIBUTES structure.
// Xbox OBJECT_ATTRIBUTES: { HANDLE RootDirectory (0), PANSI_STRING ObjectName (4),
//                            ULONG Attributes (8) } — 12 bytes.
// ANSI_STRING: { USHORT Length (0), USHORT MaxLength (2), PCHAR Buffer (4) }
inline std::string read_object_name(Executor& exec, uint32_t obj_attrs_ptr) {
    if (obj_attrs_ptr + 12 > GUEST_RAM_SIZE) return "";
    uint32_t name_ptr = 0;
    memcpy(&name_ptr, exec.ram + obj_attrs_ptr + 4, 4);
    if (name_ptr == 0 || name_ptr + 8 > GUEST_RAM_SIZE) return "";
    uint16_t len = 0;
    memcpy(&len, exec.ram + name_ptr, 2);
    uint32_t buf_ptr = 0;
    memcpy(&buf_ptr, exec.ram + name_ptr + 4, 4);
    if (buf_ptr == 0 || buf_ptr + len > GUEST_RAM_SIZE) return "";
    return std::string(reinterpret_cast<char*>(exec.ram + buf_ptr), len);
}

inline bool default_hle_handler(Executor& exec, uint32_t ordinal, void* user) {
    auto* heap = static_cast<XbeHeap*>(user);

    // Trace every HLE call â€” include return address from stack
    uint32_t ret_addr = 0;
    uint32_t hle_esp = exec.ctx.gp[GP_ESP];
    if (hle_esp + 4 <= GUEST_RAM_SIZE)
        memcpy(&ret_addr, exec.ram + hle_esp, 4);

    // Suppress repeated log lines for high-frequency ordinals (wait/delay/critsec)
    static uint32_t last_ordinal = 0;
    static uint32_t repeat_count = 0;
    static uint32_t spin_count = 0;  // counts rapid Enter/Leave CS pairs
    if (ordinal == last_ordinal &&
        (ordinal == ORD_RtlEnterCriticalSection || ordinal == ORD_RtlLeaveCriticalSection ||
         ordinal == ORD_KeDelayExecutionThread   || ordinal == ORD_KeWaitForSingleObject ||
         ordinal == ORD_KeWaitForMultipleObjects  || ordinal == ORD_NtWaitForSingleObject ||
         ordinal == ORD_NtWaitForMultipleObjectsEx || ordinal == ORD_RtlInitializeCriticalSection)) {
        repeat_count++;
    } else {
        if (repeat_count > 0)
            fprintf(stderr, "[hle]   ... repeated %u times\n", repeat_count);
        repeat_count = 0;
        fprintf(stderr, "[hle] ordinal %u  EIP=0x%08X  ESP=0x%08X  EBP=0x%08X  ret=0x%08X\n",
                ordinal, exec.ctx.eip, hle_esp, exec.ctx.gp[GP_EBP], ret_addr);
    }
    last_ordinal = ordinal;

    // Detect spin-wait: rapid repeated Enter/Leave CS pattern.
    // After enough spins, yield to let DPCs/timers fire.
    if (ordinal == ORD_RtlEnterCriticalSection || ordinal == ORD_RtlLeaveCriticalSection) {
        spin_count++;
        if (spin_count > 200) {
            spin_count = 0;
            exec.ctx.halted = true;  // yield — run_step will fire DPCs
        }
    } else {
        spin_count = 0;
    }

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
        // Close handle — if it's a real file handle, close the FILE*
        heap->close_host_file(stack_arg(exec, 0));
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Hal / Ex / Rtl stubs needed by dashboard init ----

    case ORD_HalReadSMBusValue: {
        // NTSTATUS HalReadSMBusValue(UCHAR Addr, UCHAR Cmd, BOOLEAN WordFlag, ULONG *DataValue)
        uint32_t data_ptr = stack_arg(exec, 3);
        if (data_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t val = 0;
            memcpy(exec.ram + data_ptr, &val, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 4);
        return true;
    }

    case ORD_HalWriteSMBusValue:
        // NTSTATUS HalWriteSMBusValue(UCHAR Addr, UCHAR Cmd, BOOLEAN WordFlag, ULONG DataValue)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_HalRegisterShutdownNotification:
        // void HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN Register)
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_ExQueryNonVolatileSetting: {
        // NTSTATUS ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD *Type,
        //          PVOID Value, DWORD ValueLength, DWORD *ResultLength)
        uint32_t value_index = stack_arg(exec, 0);
        uint32_t type_ptr    = stack_arg(exec, 1);
        uint32_t value_ptr   = stack_arg(exec, 2);
        uint32_t value_len   = stack_arg(exec, 3);
        uint32_t result_ptr  = stack_arg(exec, 4);

        // Default: return a DWORD of 0
        uint32_t result_val = 0;
        uint32_t result_size = 4;
        uint32_t type = 4; // REG_DWORD

        // Provide specific values for known settings
        switch (value_index) {
        case 0x04: result_val = 0x00000100; break; // AVRegion: NTSC-M
        case 0x07: result_val = 1; break;          // Language: English
        case 0x08: result_val = 0; break;          // DvdRegion: region free
        case 0x09: result_val = 0; break;          // TimeZone bias: UTC
        case 0x0A: result_val = 0; break;          // TimeZone std name (empty)
        default: break;
        }

        if (type_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + type_ptr, &type, 4);
        if (value_ptr + result_size <= GUEST_RAM_SIZE && result_size <= value_len)
            memcpy(exec.ram + value_ptr, &result_val, result_size);
        if (result_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + result_ptr, &result_size, 4);

        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_RtlNtStatusToDosError: {
        // ULONG RtlNtStatusToDosError(NTSTATUS Status)
        uint32_t status = stack_arg(exec, 0);
        uint32_t dos_error = (status == 0) ? 0 : 317; // ERROR_MR_MID_NOT_FOUND
        exec.ctx.gp[GP_EAX] = dos_error;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_HalReturnToFirmware:
        // If there are pending threads, run the next one instead of halting.
        stdcall_cleanup(exec, 1);
        if (!heap->pending_threads.empty()) {
            PendingThread t = heap->pending_threads.front();
            heap->pending_threads.erase(heap->pending_threads.begin());
            fprintf(stderr, "[hle] HalReturnToFirmware: running pending thread routine=0x%08X\n",
                    t.start_routine);
            uint32_t esp = exec.ctx.gp[GP_ESP];
            uint32_t halt_addr = hle_stub_addr(ORD_HalReturnToFirmware);
            // Push start context as argument
            esp -= 4;
            if (esp + 4 <= GUEST_RAM_SIZE) memcpy(exec.ram + esp, &t.start_context, 4);
            // Push return address
            esp -= 4;
            if (esp + 4 <= GUEST_RAM_SIZE) memcpy(exec.ram + esp, &halt_addr, 4);
            exec.ctx.gp[GP_ESP] = esp;
            exec.ctx.eip = t.start_routine;
        } else {
            exec.ctx.halted = true;
        }
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

    case ORD_KeInitializeDpc: {
        // void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
        // KDPC layout: Type(0x00,1B), Inserted(0x02,1B), Padding(0x03,1B),
        //   DpcListEntry(0x04,8B), DeferredRoutine(0x0C,4B),
        //   DeferredContext(0x10,4B), SystemArgument1(0x14,4B), SystemArgument2(0x18,4B)
        uint32_t dpc_va  = stack_arg(exec, 0);
        uint32_t routine = stack_arg(exec, 1);
        uint32_t context = stack_arg(exec, 2);
        if (dpc_va + 0x1C <= GUEST_RAM_SIZE) {
            memset(exec.ram + dpc_va, 0, 0x1C);
            exec.ram[dpc_va] = 19; // Type = DpcObject
            memcpy(exec.ram + dpc_va + 0x0C, &routine, 4);
            memcpy(exec.ram + dpc_va + 0x10, &context, 4);
        }
        stdcall_cleanup(exec, 3);
        return true;
    }

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
        // Xbox Pentium III runs at 733.33 MHz â†’ report that as the frequency.
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

    case ORD_KeSetTimer: {
        // BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
        uint32_t timer_va = stack_arg(exec, 0);
        uint32_t dpc_va   = stack_arg(exec, 3); // after 8-byte DueTime
        if (dpc_va && dpc_va + 0x1C <= GUEST_RAM_SIZE) {
            uint32_t routine = 0, context = 0;
            memcpy(&routine, exec.ram + dpc_va + 0x0C, 4);
            memcpy(&context, exec.ram + dpc_va + 0x10, 4);
            if (routine)
                heap->pending_dpcs.push_back({routine, context, dpc_va, timer_va});
        }
        exec.ctx.gp[GP_EAX] = 0; // was not already in queue
        stdcall_cleanup(exec, 4);
        return true;
    }

    case ORD_KeSetTimerEx: {
        // BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc)
        uint32_t timer_va = stack_arg(exec, 0);
        uint32_t dpc_va   = stack_arg(exec, 4); // after 8-byte DueTime + Period
        if (dpc_va && dpc_va + 0x1C <= GUEST_RAM_SIZE) {
            uint32_t routine = 0, context = 0;
            memcpy(&routine, exec.ram + dpc_va + 0x0C, 4);
            memcpy(&context, exec.ram + dpc_va + 0x10, 4);
            if (routine)
                heap->pending_dpcs.push_back({routine, context, dpc_va, timer_va});
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_PsCreateSystemThread: {
        // PsCreateSystemThread(OUT PHANDLE, ..., IN PKSTART_ROUTINE StartRoutine,
        //                      IN PVOID StartContext)
        // stdcall: 7 args. Args[0]=PHANDLE, Args[5]=StartRoutine, Args[6]=StartContext
        uint32_t handle_ptr    = stack_arg(exec, 0);
        uint32_t start_routine = stack_arg(exec, 5);
        uint32_t start_context = stack_arg(exec, 6);
        uint32_t handle = heap->next_handle++;
        if (handle_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + handle_ptr, &handle, 4);
        heap->pending_threads.push_back({start_routine, start_context});
        fprintf(stderr, "[hle] PsCreateSystemThread: routine=0x%08X ctx=0x%08X handle=0x%X\n",
                start_routine, start_context, handle);
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 7);
        return true;
    }

    case ORD_PsCreateSystemThreadEx: {
        // PsCreateSystemThreadEx(OUT PHANDLE ThreadHandle, SIZE_T ThreadExtensionSize,
        //   SIZE_T KernelStackSize, SIZE_T TlsDataSize, OUT PHANDLE ThreadId,
        //   PKSTART_ROUTINE StartRoutine, PVOID StartContext,
        //   BOOLEAN CreateSuspended, BOOLEAN DebuggerThread, PKSYSTEM_ROUTINE SystemRoutine)
        // stdcall: 10 args.
        uint32_t handle_ptr    = stack_arg(exec, 0);
        uint32_t tid_ptr       = stack_arg(exec, 4);
        uint32_t start_routine = stack_arg(exec, 5);
        uint32_t start_context = stack_arg(exec, 6);
        uint32_t handle = heap->next_handle++;
        if (handle_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + handle_ptr, &handle, 4);
        if (tid_ptr && tid_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t tid = handle;
            memcpy(exec.ram + tid_ptr, &tid, 4);
        }
        heap->pending_threads.push_back({start_routine, start_context});
        fprintf(stderr, "[hle] PsCreateSystemThreadEx: routine=0x%08X ctx=0x%08X handle=0x%X\n",
                start_routine, start_context, handle);
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 10);
        return true;
    }

    case ORD_PsTerminateSystemThread:
        // Current thread exits. Run next pending thread if any.
        stdcall_cleanup(exec, 1);
        if (!heap->pending_threads.empty()) {
            PendingThread t = heap->pending_threads.front();
            heap->pending_threads.erase(heap->pending_threads.begin());
            fprintf(stderr, "[hle] PsTerminateSystemThread: switching to thread routine=0x%08X ctx=0x%08X\n",
                    t.start_routine, t.start_context);
            // Set up a call frame: push context as arg, set EIP to routine
            uint32_t esp = exec.ctx.gp[GP_ESP];
            // Push a return address that will halt (use HalReturnToFirmware stub)
            uint32_t halt_addr = hle_stub_addr(ORD_HalReturnToFirmware);
            esp -= 4;
            if (esp + 4 <= GUEST_RAM_SIZE) memcpy(exec.ram + esp, &halt_addr, 4);
            // Push start context as argument
            esp -= 4;
            if (esp + 4 <= GUEST_RAM_SIZE) memcpy(exec.ram + esp, &t.start_context, 4);
            // Push the return address (HalReturnToFirmware, so thread exit halts)
            esp -= 4;
            if (esp + 4 <= GUEST_RAM_SIZE) memcpy(exec.ram + esp, &halt_addr, 4);
            exec.ctx.gp[GP_ESP] = esp;
            exec.ctx.eip = t.start_routine;
        } else {
            exec.ctx.halted = true;
        }
        exec.ctx.gp[GP_EAX] = 0;
        return true;

    case ORD_RtlEnterCriticalSection: {
        // void RtlEnterCriticalSection(PRTL_CRITICAL_SECTION)
        // Single-threaded stub: always succeeds immediately.
        // RTL_CRITICAL_SECTION: {DebugInfo(0), LockCount(4), RecursionCount(8),
        //                        OwningThread(0xC), LockSemaphore(0x10), SpinCount(0x14)}
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            int32_t lock = 0; // locked
            memcpy(exec.ram + cs + 0x04, &lock, 4);
            int32_t rec = 1;
            memcpy(exec.ram + cs + 0x08, &rec, 4);
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_RtlLeaveCriticalSection: {
        // void RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION)
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            int32_t lock = -1; // unlocked
            memcpy(exec.ram + cs + 0x04, &lock, 4);
            int32_t rec = 0;
            memcpy(exec.ram + cs + 0x08, &rec, 4);
            uint32_t zero = 0;
            memcpy(exec.ram + cs + 0x0C, &zero, 4); // OwningThread = 0
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_RtlInitializeCriticalSection: {
        // void RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION)
        // Initialize all fields: DebugInfo=0, LockCount=-1, RecursionCount=0,
        // OwningThread=0, LockSemaphore=0, SpinCount=0
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            memset(exec.ram + cs, 0, 0x18);
            int32_t neg1 = -1;
            memcpy(exec.ram + cs + 0x04, &neg1, 4); // LockCount = -1 (unlocked)
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

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

    // ---- ExAllocatePoolWithTag / ExQueryPoolBlockSize ----

    case ORD_ExAllocatePoolWithTag: {
        // PVOID ExAllocatePoolWithTag(ULONG size, ULONG tag)
        uint32_t size = stack_arg(exec, 0);
        uint32_t addr = heap->alloc(size);
        if (addr) memset(exec.ram + addr, 0, size);
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_ExQueryPoolBlockSize:
        // ULONG ExQueryPoolBlockSize(PVOID)
        exec.ctx.gp[GP_EAX] = 0x1000; // return page-sized block
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Synchronisation primitives (single-threaded stubs) ----

    case ORD_KeInitializeEvent:
        // void KeInitializeEvent(PKEVENT, EVENT_TYPE, BOOLEAN InitialState)
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeInitializeMutant:
        // void KeInitializeMutex(PKMUTEX, ULONG Level)
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_KeInitializeSemaphore:
        // void KeInitializeSemaphore(PKSEMAPHORE, LONG Count, LONG Limit)
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeInitializeTimerEx:
        // void KeInitializeTimerEx(PKTIMER, TIMER_TYPE)
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_KeSetEvent:
        // LONG KeSetEvent(PKEVENT, KPRIORITY Increment, BOOLEAN Wait)
        exec.ctx.gp[GP_EAX] = 0; // previous state = not-signaled
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeResetEvent:
        // LONG KeResetEvent(PKEVENT)
        exec.ctx.gp[GP_EAX] = 0; // previous state
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_KeReleaseMutant:
        // LONG KeReleaseMutex(PKMUTEX, BOOLEAN Wait)
        exec.ctx.gp[GP_EAX] = 0; // previous count
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_KeReleaseSemaphore:
        // LONG KeReleaseSemaphore(PKSEMAPHORE, KPRIORITY, LONG Adjustment, BOOLEAN Wait)
        exec.ctx.gp[GP_EAX] = 0; // previous count
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_KeWaitForSingleObject:
        // NTSTATUS KeWaitForSingleObject(PVOID Object, KWAIT_REASON, KPROCESSOR_MODE,
        //                                BOOLEAN Alertable, PLARGE_INTEGER Timeout)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS (object signaled immediately)
        stdcall_cleanup(exec, 5);
        exec.ctx.halted = true; // yield to frame loop
        return true;

    case ORD_KeWaitForMultipleObjects:
        // NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID *Objects,
        //          WAIT_TYPE, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN Alertable,
        //          PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_WAIT_0
        stdcall_cleanup(exec, 8);
        exec.ctx.halted = true; // yield to frame loop
        return true;

    case ORD_NtCreateEvent: {
        // NTSTATUS NtCreateEvent(PHANDLE Handle, ...)
        // Write a fake handle to *Handle
        uint32_t handle_ptr = stack_arg(exec, 0);
        if (handle_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t fake_handle = 0x100;
            memcpy(exec.ram + handle_ptr, &fake_handle, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_NtCreateMutant: {
        uint32_t handle_ptr = stack_arg(exec, 0);
        if (handle_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t fake_handle = 0x104;
            memcpy(exec.ram + handle_ptr, &fake_handle, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        return true;
    }

    case ORD_NtCreateSemaphore: {
        uint32_t handle_ptr = stack_arg(exec, 0);
        if (handle_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t fake_handle = 0x108;
            memcpy(exec.ram + handle_ptr, &fake_handle, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_NtSetEvent:
        // NTSTATUS NtSetEvent(HANDLE, PLONG PreviousState)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_NtClearEvent:
        // NTSTATUS NtClearEvent(HANDLE)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_NtWaitForSingleObject:
        // NTSTATUS NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        exec.ctx.halted = true; // yield to frame loop
        return true;

    case ORD_NtWaitForMultipleObjectsEx:
        // NTSTATUS NtWaitForMultipleObjects(ULONG Count, PHANDLE Handles,
        //          WAIT_TYPE, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 5);
        exec.ctx.halted = true; // yield to frame loop
        return true;

    // ---- Object manager stubs ----

    case ORD_ObReferenceObjectByHandle:
        // NTSTATUS ObReferenceObjectByHandle(HANDLE, ACCESS_MASK, POBJECT_TYPE,
        //          KPROCESSOR_MODE, PVOID *Object, POBJECT_HANDLE_INFORMATION)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 6);
        return true;

    case ORD_ObfDereferenceObject:
        // void ObDereferenceObject(PVOID Object)
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Interlocked operations ----

    case ORD_InterlockedCompareExchange: {
        // LONG InterlockedCompareExchange(LONG volatile *Dest, LONG Exchange, LONG Comparand)
        // fastcall: Dest=ECX, Exchange=EDX, Comparand=stack_arg(0)
        // Actually stdcall on Xbox: 3 args on stack
        uint32_t dest_ptr  = stack_arg(exec, 0);
        uint32_t exchange  = stack_arg(exec, 1);
        uint32_t comparand = stack_arg(exec, 2);
        uint32_t current = 0;
        if (dest_ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(&current, exec.ram + dest_ptr, 4);
            if (current == comparand)
                memcpy(exec.ram + dest_ptr, &exchange, 4);
        }
        exec.ctx.gp[GP_EAX] = current; // returns old value
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_InterlockedDecrement: {
        // LONG InterlockedDecrement(LONG volatile *Addend)
        uint32_t ptr = stack_arg(exec, 0);
        int32_t val = 0;
        if (ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(&val, exec.ram + ptr, 4);
            val--;
            memcpy(exec.ram + ptr, &val, 4);
        }
        exec.ctx.gp[GP_EAX] = (uint32_t)val;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_InterlockedIncrement: {
        // LONG InterlockedIncrement(LONG volatile *Addend)
        uint32_t ptr = stack_arg(exec, 0);
        int32_t val = 0;
        if (ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(&val, exec.ram + ptr, 4);
            val++;
            memcpy(exec.ram + ptr, &val, 4);
        }
        exec.ctx.gp[GP_EAX] = (uint32_t)val;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_InterlockedExchange: {
        // LONG InterlockedExchange(LONG volatile *Target, LONG Value)
        uint32_t ptr = stack_arg(exec, 0);
        uint32_t new_val = stack_arg(exec, 1);
        uint32_t old_val = 0;
        if (ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(&old_val, exec.ram + ptr, 4);
            memcpy(exec.ram + ptr, &new_val, 4);
        }
        exec.ctx.gp[GP_EAX] = old_val;
        stdcall_cleanup(exec, 2);
        return true;
    }

    // ---- Memory mapping ----

    case ORD_MmMapIoSpace: {
        // PVOID MmMapIoSpace(PHYSICAL_ADDRESS PhysAddr, SIZE_T Size, ULONG CacheType)
        // On Xbox, MMIO is identity-mapped. Return PhysAddr as-is.
        uint32_t phys = stack_arg(exec, 0);
        exec.ctx.gp[GP_EAX] = phys;
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_MmUnmapIoSpace:
        // void MmUnmapIoSpace(PVOID BaseAddress, SIZE_T Size)
        stdcall_cleanup(exec, 2);
        return true;

    // ---- Rtl memory/string utilities ----

    case ORD_RtlInitUnicodeString: {
        // void RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Src)
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        uint16_t len = 0;
        if (src && src < GUEST_RAM_SIZE) {
            // Count bytes until double-null (wchar_t = 2 bytes)
            while (src + len + 1 < GUEST_RAM_SIZE) {
                uint16_t ch;
                memcpy(&ch, exec.ram + src + len, 2);
                if (ch == 0) break;
                len += 2;
            }
        }
        if (dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t max_len = len + 2;
            memcpy(exec.ram + dest + 0, &len, 2);
            memcpy(exec.ram + dest + 2, &max_len, 2);
            memcpy(exec.ram + dest + 4, &src, 4);
        }
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_RtlCompareMemory: {
        // SIZE_T RtlCompareMemory(const VOID *Src1, const VOID *Src2, SIZE_T Length)
        // Returns number of bytes that match (from the beginning).
        uint32_t src1 = stack_arg(exec, 0);
        uint32_t src2 = stack_arg(exec, 1);
        uint32_t len  = stack_arg(exec, 2);
        uint32_t matched = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (src1 + i >= GUEST_RAM_SIZE || src2 + i >= GUEST_RAM_SIZE) break;
            if (exec.ram[src1 + i] != exec.ram[src2 + i]) break;
            matched++;
        }
        exec.ctx.gp[GP_EAX] = matched;
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_RtlMoveMemory: {
        // void RtlCopyMemory(VOID *Dest, const VOID *Src, SIZE_T Length)
        // RtlMoveMemory handles overlaps (memmove).
        uint32_t dst = stack_arg(exec, 0);
        uint32_t src = stack_arg(exec, 1);
        uint32_t len = stack_arg(exec, 2);
        if (dst + len <= GUEST_RAM_SIZE && src + len <= GUEST_RAM_SIZE && len > 0)
            memmove(exec.ram + dst, exec.ram + src, len);
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_RtlFillMemory: {
        // void RtlFillMemory(VOID *Dest, SIZE_T Length, UCHAR Fill)
        uint32_t dst  = stack_arg(exec, 0);
        uint32_t len  = stack_arg(exec, 1);
        uint32_t fill = stack_arg(exec, 2);
        if (dst + len <= GUEST_RAM_SIZE && len > 0)
            memset(exec.ram + dst, (int)(fill & 0xFF), len);
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_RtlZeroMemory: {
        // void RtlZeroMemory(VOID *Dest, SIZE_T Length)
        uint32_t dst = stack_arg(exec, 0);
        uint32_t len = stack_arg(exec, 1);
        if (dst + len <= GUEST_RAM_SIZE && len > 0)
            memset(exec.ram + dst, 0, len);
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_RtlEqualString: {
        // BOOLEAN RtlEqualString(PSTRING Str1, PSTRING Str2, BOOLEAN CaseInsensitive)
        // ANSI_STRING: { USHORT Length, USHORT MaxLength, PCHAR Buffer }
        uint32_t s1_ptr = stack_arg(exec, 0);
        uint32_t s2_ptr = stack_arg(exec, 1);
        uint16_t len1 = 0, len2 = 0;
        uint32_t buf1 = 0, buf2 = 0;
        if (s1_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(&len1, exec.ram + s1_ptr, 2);
            memcpy(&buf1, exec.ram + s1_ptr + 4, 4);
        }
        if (s2_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(&len2, exec.ram + s2_ptr, 2);
            memcpy(&buf2, exec.ram + s2_ptr + 4, 4);
        }
        bool eq = (len1 == len2);
        if (eq && len1 > 0 && buf1 + len1 <= GUEST_RAM_SIZE && buf2 + len2 <= GUEST_RAM_SIZE)
            eq = (memcmp(exec.ram + buf1, exec.ram + buf2, len1) == 0);
        exec.ctx.gp[GP_EAX] = eq ? 1u : 0u;
        stdcall_cleanup(exec, 3);
        return true;
    }

    // ---- I/O Manager stubs ----

    case ORD_IoCreateDevice:
        // NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PSTRING, ULONG,
        //                         BOOLEAN, PDEVICE_OBJECT*)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 6);
        return true;

    case ORD_IoCreateFile: {
        // NTSTATUS IoCreateFile(OUT PHANDLE, ACCESS, OBJ_ATTRS, IO_STATUS,
        //     PLARGE_INT, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG)
        // Write a fake handle
        uint32_t handle_ptr = stack_arg(exec, 0);
        uint32_t h = heap->next_handle++;
        if (handle_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + handle_ptr, &h, 4);
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 14);
        return true;
    }

    case ORD_IoCreateSymbolicLink:
        // NTSTATUS IoCreateSymbolicLink(PSTRING SymbolicLinkName, PSTRING DeviceName)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_IoDeleteDevice:
        // void IoDeleteDevice(PDEVICE_OBJECT)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_IoDeleteSymbolicLink:
        // NTSTATUS IoDeleteSymbolicLink(PSTRING SymbolicLinkName)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Additional Ke stubs ----

    case ORD_KeGetCurrentIrql:
        // KIRQL KeGetCurrentIrql(void)
        exec.ctx.gp[GP_EAX] = 0; // PASSIVE_LEVEL
        return true; // no args to clean

    case ORD_KeDelayExecutionThread:
        // NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER)
        // Yield to host: halt the executor so the frame loop advances time.
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 3);
        exec.ctx.halted = true;
        return true;

    case ORD_KeEnterCriticalRegion:
        // void KeEnterCriticalRegion(void) â€” no args
        return true;

    case ORD_HalGetInterruptVector: {
        // ULONG HalGetInterruptVector(ULONG BusInterruptLevel, OUT PKIRQL Irql)
        uint32_t irql_ptr = stack_arg(exec, 1);
        if (irql_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t irql = 0; // PASSIVE_LEVEL
            memcpy(exec.ram + irql_ptr, &irql, 4);
        }
        exec.ctx.gp[GP_EAX] = stack_arg(exec, 0) + 0x30; // fake vector
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_KeConnectInterrupt:
        // BOOLEAN KeConnectInterrupt(PKINTERRUPT)
        exec.ctx.gp[GP_EAX] = 1; // TRUE
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_KeInitializeInterrupt:
        // void KeInitializeInterrupt(PKINTERRUPT, PKSERVICE_ROUTINE, PVOID, ULONG,
        //     KIRQL, KINTERRUPT_MODE, BOOLEAN ShareVector)
        stdcall_cleanup(exec, 7);
        return true;

    case ORD_KeStallExecutionProcessor:
        // void KeStallExecutionProcessor(ULONG MicroSeconds)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_NtOpenFile: {
        // NTSTATUS NtOpenFile(OUT PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        //     PIO_STATUS_BLOCK, ULONG ShareAccess, ULONG OpenOptions)
        uint32_t handle_ptr = stack_arg(exec, 0);
        uint32_t obj_attrs  = stack_arg(exec, 2);
        uint32_t iosb_ptr   = stack_arg(exec, 3);

        std::string xbox_path = read_object_name(exec, obj_attrs);
        std::string host_path = heap->translate_path(xbox_path);

        uint32_t h = 0;
        uint32_t status = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        if (!host_path.empty()) {
            h = heap->open_host_file(host_path);
            if (h) {
                status = 0;
                fprintf(stderr, "[hle] NtOpenFile: '%s' -> handle 0x%X\n",
                        xbox_path.c_str(), h);
            } else {
                fprintf(stderr, "[hle] NtOpenFile: '%s' -> not found (%s)\n",
                        xbox_path.c_str(), host_path.c_str());
            }
        } else {
            // No mount matched — return a fake handle
            h = heap->next_handle++;
            status = 0;
            fprintf(stderr, "[hle] NtOpenFile: '%s' -> fake handle 0x%X\n",
                    xbox_path.c_str(), h);
        }

        if (handle_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + handle_ptr, &h, 4);
        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + iosb_ptr, &status, 4);
            uint32_t info = (status == 0) ? 1u : 0u;
            memcpy(exec.ram + iosb_ptr + 4, &info, 4);
        }
        exec.ctx.gp[GP_EAX] = status;
        stdcall_cleanup(exec, 6);
        return true;
    }

    case ORD_NtReadFile: {
        // NTSTATUS NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID,
        //     PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset)
        uint32_t handle   = stack_arg(exec, 0);
        uint32_t iosb_ptr = stack_arg(exec, 4);
        uint32_t buf_ptr  = stack_arg(exec, 5);
        uint32_t length   = stack_arg(exec, 6);
        uint32_t off_ptr  = stack_arg(exec, 7);

        HostFile* hf = heap->get_host_file(handle);
        uint32_t bytes_read = 0;
        uint32_t status = 0xC0000008u; // STATUS_INVALID_HANDLE

        if (hf && hf->fp) {
            // Seek to offset if provided
            if (off_ptr && off_ptr + 8 <= GUEST_RAM_SIZE) {
                int64_t offset = 0;
                memcpy(&offset, exec.ram + off_ptr, 8);
                if (offset >= 0) _fseeki64(hf->fp, offset, SEEK_SET);
            }
            // Clamp to guest RAM
            if (buf_ptr + length > GUEST_RAM_SIZE)
                length = (buf_ptr < GUEST_RAM_SIZE) ? GUEST_RAM_SIZE - buf_ptr : 0;
            if (length > 0)
                bytes_read = (uint32_t)fread(exec.ram + buf_ptr, 1, length, hf->fp);
            status = (bytes_read > 0) ? 0 : 0xC0000011u; // STATUS_END_OF_FILE
        } else if (!hf) {
            // Fake handle (device, etc.) — return 0 bytes
            status = 0;
        }

        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + iosb_ptr, &status, 4);
            memcpy(exec.ram + iosb_ptr + 4, &bytes_read, 4);
        }
        exec.ctx.gp[GP_EAX] = status;
        stdcall_cleanup(exec, 8);
        return true;
    }

    case ORD_NtSetInformationFile:
        // NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;

    case ORD_NtQueryInformationFile: {
        // NTSTATUS NtQueryInformationFile(HANDLE FileHandle,
        //     PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
        //     ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
        uint32_t handle   = stack_arg(exec, 0);
        uint32_t iosb_ptr = stack_arg(exec, 1);
        uint32_t info_ptr = stack_arg(exec, 2);
        uint32_t info_len = stack_arg(exec, 3);
        uint32_t info_class = stack_arg(exec, 4);

        HostFile* hf = heap->get_host_file(handle);
        uint32_t status = 0;

        if (hf) {
            switch (info_class) {
            case 5: // FileStandardInformation (size, nlinks, delete pending, directory)
                if (info_ptr + 24 <= GUEST_RAM_SIZE && info_len >= 24) {
                    memset(exec.ram + info_ptr, 0, 24);
                    uint64_t alloc_sz = (hf->size + 0xFFF) & ~0xFFFULL;
                    memcpy(exec.ram + info_ptr + 0, &alloc_sz, 8); // AllocationSize
                    memcpy(exec.ram + info_ptr + 8, &hf->size, 8); // EndOfFile
                    uint32_t nlinks = 1;
                    memcpy(exec.ram + info_ptr + 16, &nlinks, 4); // NumberOfLinks
                    // DeletePending=0, Directory=0
                }
                break;
            case 14: // FilePositionInformation
                if (info_ptr + 8 <= GUEST_RAM_SIZE && info_len >= 8) {
                    int64_t pos = hf->fp ? _ftelli64(hf->fp) : 0;
                    memcpy(exec.ram + info_ptr, &pos, 8);
                }
                break;
            default:
                if (info_ptr + info_len <= GUEST_RAM_SIZE && info_len > 0)
                    memset(exec.ram + info_ptr, 0, info_len);
                break;
            }
        }

        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + iosb_ptr, &status, 4);
            uint32_t info = 0;
            memcpy(exec.ram + iosb_ptr + 4, &info, 4);
        }
        exec.ctx.gp[GP_EAX] = status;
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_NtQueryVolumeInformationFile: {
        // NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK,
        //     PVOID FsInformation, ULONG Length, FS_INFORMATION_CLASS)
        uint32_t iosb_ptr = stack_arg(exec, 1);
        uint32_t info_ptr = stack_arg(exec, 2);
        uint32_t info_len = stack_arg(exec, 3);
        uint32_t info_class = stack_arg(exec, 4);

        if (info_ptr + info_len <= GUEST_RAM_SIZE && info_len > 0)
            memset(exec.ram + info_ptr, 0, info_len);

        // FileFsSizeInformation (class 3): report 8 GB free
        if (info_class == 3 && info_len >= 24 && info_ptr + 24 <= GUEST_RAM_SIZE) {
            uint64_t total_units = 8ULL * 1024 * 1024 * 1024 / (512 * 8); // ~2M clusters
            uint64_t avail_units = total_units / 2;
            memcpy(exec.ram + info_ptr + 0, &total_units, 8);
            memcpy(exec.ram + info_ptr + 8, &avail_units, 8);
            uint32_t spc = 8;  // sectors per cluster
            memcpy(exec.ram + info_ptr + 16, &spc, 4);
            uint32_t bps = 512; // bytes per sector
            memcpy(exec.ram + info_ptr + 20, &bps, 4);
        }

        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            uint32_t status = 0;
            memcpy(exec.ram + iosb_ptr, &status, 4);
            uint32_t info = 0;
            memcpy(exec.ram + iosb_ptr + 4, &info, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_KeRaiseIrqlToDpcLevel:
        // KIRQL KeRaiseIrqlToDpcLevel(void)
        exec.ctx.gp[GP_EAX] = 0; // return old IRQL (PASSIVE_LEVEL)
        return true;

    case ORD_KfLowerIrql:
        // void KfLowerIrql(KIRQL NewIrql) â€” fastcall, arg in CL
        return true;

    case ORD_MmLockUnlockBufferPages:
        // void MmLockUnlockBufferPages(PVOID, ULONG, BOOLEAN)
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_KeBugCheck:
        fprintf(stderr, "[hle] KeBugCheck(0x%08X)\n", exec.ctx.gp[GP_EAX]);
        exec.ctx.halted = true;
        return true;

    case ORD_KeBugCheckEx:
        fprintf(stderr, "[hle] KeBugCheckEx(0x%08X, ...)\n", stack_arg(exec, 0));
        exec.ctx.halted = true;
        stdcall_cleanup(exec, 5);
        return true;

    case ORD_HalReadSMCTrayState: {
        // NTSTATUS HalReadSMCTrayState(PULONG TrayState, PULONG TrayStateChangeCount)
        // TrayState: 0=no media, 1=tray open, etc.  Return "media present"
        uint32_t state_ptr = stack_arg(exec, 0);
        uint32_t count_ptr = stack_arg(exec, 1);
        if (state_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t state = 0x10; // TRAY_CLOSED_MEDIA_PRESENT
            memcpy(exec.ram + state_ptr, &state, 4);
        }
        if (count_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t count = 0;
            memcpy(exec.ram + count_ptr, &count, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_IoDismountVolumeByName:
        // NTSTATUS IoDismountVolumeByName(PSTRING VolumeName)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_NtCreateFile: {
        // NTSTATUS NtCreateFile(OUT PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
        //     POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
        //     PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
        //     ULONG CreateDisposition, ULONG CreateOptions)
        uint32_t handle_ptr  = stack_arg(exec, 0);
        uint32_t obj_attrs   = stack_arg(exec, 2);
        uint32_t iosb_ptr    = stack_arg(exec, 3);

        std::string xbox_path = read_object_name(exec, obj_attrs);
        std::string host_path = heap->translate_path(xbox_path);

        uint32_t h = 0;
        uint32_t status = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        if (!host_path.empty()) {
            h = heap->open_host_file(host_path);
            if (h) {
                status = 0; // STATUS_SUCCESS
                fprintf(stderr, "[hle] NtCreateFile: '%s' -> handle 0x%X\n",
                        xbox_path.c_str(), h);
            } else {
                fprintf(stderr, "[hle] NtCreateFile: '%s' -> not found (%s)\n",
                        xbox_path.c_str(), host_path.c_str());
            }
        } else {
            // No mount matched — return a fake handle for device paths etc.
            h = heap->next_handle++;
            status = 0;
            fprintf(stderr, "[hle] NtCreateFile: '%s' -> fake handle 0x%X\n",
                    xbox_path.c_str(), h);
        }

        if (handle_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + handle_ptr, &h, 4);
        // Write IO_STATUS_BLOCK: {Status, Information}
        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            memcpy(exec.ram + iosb_ptr, &status, 4);
            uint32_t info = (status == 0) ? 1u : 0u; // FILE_OPENED
            memcpy(exec.ram + iosb_ptr + 4, &info, 4);
        }
        exec.ctx.gp[GP_EAX] = status;
        stdcall_cleanup(exec, 9);
        return true;
    }

    case ORD_ExInitializeReadWriteLock:
        // void ExInitializeReadWriteLock(PERWLOCK ReadWriteLock)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_FscSetCacheSize:
        // NTSTATUS FscSetCacheSize(ULONG NumberOfPages)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_NtDeviceIoControlFile: {
        // NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
        //     PIO_STATUS_BLOCK, ULONG IoControlCode, PVOID InputBuffer,
        //     ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 10);
        return true;
    }

    case ORD_NtQueryFullAttributesFile: {
        // NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes,
        //     PFILE_NETWORK_OPEN_INFORMATION FileInformation)
        uint32_t obj_attrs = stack_arg(exec, 0);
        uint32_t info_ptr  = stack_arg(exec, 1);

        std::string xbox_path = read_object_name(exec, obj_attrs);
        std::string host_path = heap->translate_path(xbox_path);

        uint32_t status = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        if (!host_path.empty()) {
            FILE* fp = fopen(host_path.c_str(), "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                uint64_t sz = (uint64_t)ftell(fp);
                fclose(fp);

                // FILE_NETWORK_OPEN_INFORMATION: 56 bytes
                // {CreationTime(8), LastAccessTime(8), LastWriteTime(8),
                //  ChangeTime(8), AllocationSize(8), EndOfFile(8),
                //  FileAttributes(4), padding(4)}
                if (info_ptr + 56 <= GUEST_RAM_SIZE) {
                    memset(exec.ram + info_ptr, 0, 56);
                    uint64_t alloc_sz = (sz + 0xFFF) & ~0xFFFULL;
                    memcpy(exec.ram + info_ptr + 32, &alloc_sz, 8);
                    memcpy(exec.ram + info_ptr + 40, &sz, 8);
                    uint32_t attrs = 0x80; // FILE_ATTRIBUTE_NORMAL
                    memcpy(exec.ram + info_ptr + 48, &attrs, 4);
                }
                status = 0;
                fprintf(stderr, "[hle] NtQueryFullAttributesFile: '%s' -> size=%llu\n",
                        xbox_path.c_str(), (unsigned long long)sz);
            } else {
                fprintf(stderr, "[hle] NtQueryFullAttributesFile: '%s' -> not found\n",
                        xbox_path.c_str());
            }
        }
        exec.ctx.gp[GP_EAX] = status;
        stdcall_cleanup(exec, 2);
        return true;
    }

    default:
        // Unhandled: log and return STATUS_NOT_IMPLEMENTED
        fprintf(stderr, "[hle] unhandled kernel ordinal %u at EIP=0x%08X\n",
                ordinal, exec.ctx.eip);
        exec.ctx.gp[GP_EAX] = 0xC0000002u; // STATUS_NOT_IMPLEMENTED
        return true;
    }
}

} // namespace xbe
