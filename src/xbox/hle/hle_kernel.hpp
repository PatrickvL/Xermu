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
#include <sys/stat.h>

// Forward declaration for NV2A PRAMIN mapping
namespace xbox { struct Nv2aState; }
#include <unordered_map>
#include <algorithm>


// ---------- Minimal SHA-1 for Xbox crypto HLE (XcSHA / XcHMAC) ----------
namespace sha1_detail {
struct SHA1_CTX {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
};

static inline uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(block[i*4])<<24)|(uint32_t(block[i*4+1])<<16)|
               (uint32_t(block[i*4+2])<<8)|block[i*4+3];
    for (int i = 16; i < 80; ++i)
        w[i] = rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
        else if (i<40) { f=b^c^d;           k=0x6ED9EBA1; }
        else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else           { f=b^c^d;           k=0xCA62C1D6; }
        uint32_t t = rol32(a,5)+f+e+k+w[i];
        e=d; d=c; c=rol32(b,30); b=a; a=t;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d; state[4]+=e;
}

static void sha1_init(SHA1_CTX* ctx) {
    ctx->state[0]=0x67452301; ctx->state[1]=0xEFCDAB89;
    ctx->state[2]=0x98BADCFE; ctx->state[3]=0x10325476;
    ctx->state[4]=0xC3D2E1F0; ctx->count=0;
}

static void sha1_update(SHA1_CTX* ctx, const uint8_t* data, size_t len) {
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[idx++] = data[i];
        if (idx == 64) { sha1_transform(ctx->state, ctx->buffer); idx = 0; }
    }
}

static void sha1_final(SHA1_CTX* ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 63) != 56)
        sha1_update(ctx, &pad, 1);
    uint8_t len_be[8];
    for (int i = 7; i >= 0; --i) { len_be[i] = (uint8_t)(bits & 0xFF); bits >>= 8; }
    sha1_update(ctx, len_be, 8);
    for (int i = 0; i < 5; ++i) {
        digest[i*4+0] = (uint8_t)(ctx->state[i]>>24);
        digest[i*4+1] = (uint8_t)(ctx->state[i]>>16);
        digest[i*4+2] = (uint8_t)(ctx->state[i]>>8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

static void hmac_sha1(const uint8_t* key, size_t key_len,
                      const uint8_t* data1, size_t data1_len,
                      const uint8_t* data2, size_t data2_len,
                      uint8_t digest[20]) {
    uint8_t k_pad[64];
    memset(k_pad, 0, 64);
    if (key_len > 64) {
        SHA1_CTX tmp; sha1_init(&tmp);
        sha1_update(&tmp, key, key_len);
        sha1_final(&tmp, k_pad);
    } else {
        memcpy(k_pad, key, key_len);
    }
    // ipad
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k_pad[i] ^ 0x36; opad[i] = k_pad[i] ^ 0x5C; }
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, ipad, 64);
    if (data1 && data1_len) sha1_update(&ctx, data1, data1_len);
    if (data2 && data2_len) sha1_update(&ctx, data2, data2_len);
    uint8_t inner[20];
    sha1_final(&ctx, inner);
    sha1_init(&ctx);
    sha1_update(&ctx, opad, 64);
    sha1_update(&ctx, inner, 20);
    sha1_final(&ctx, digest);
}
} // namespace sha1_detail

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
    ORD_MmPersistContiguousMemory      = 178,
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
    ORD_NtDuplicateObject              = 197,
    ORD_NtFlushBuffersFile             = 198,
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
    ORD_PhyGetLinkState                  = 252,
    ORD_PhyInitialize                   = 253,
    ORD_XcSHAInit                      = 335,
    ORD_XcSHAUpdate                    = 336,
    ORD_XcSHAFinal                     = 337,
    ORD_XcRC4Key                       = 338,
    ORD_XcRC4Crypt                     = 339,
    ORD_XcHMAC                         = 340,
    ORD_ExReadWriteRefurbInfo               = 25,
    ORD_ExReleaseReadWriteLock              = 28,
    ORD_IoBuildSynchronousFsdRequest        = 62,
    ORD_IoInvalidDeviceRequest              = 74,
    ORD_IoStartNextPacket                   = 81,
    ORD_IoStartPacket                       = 83,
    ORD_IoSynchronousDeviceIoControlRequest = 84,
    ORD_IoSynchronousFsdRequest             = 85,
    ORD_IofCallDriver                       = 86,
    ORD_IofCompleteRequest                  = 87,
    ORD_KeDisconnectInterrupt               = 100,
    ORD_KeQueryBasePriorityThread           = 124,
    ORD_KeRemoveQueueDpc                    = 137,
    ORD_KeRestoreFloatingPointState         = 139,
    ORD_KeSaveFloatingPointState            = 142,
    ORD_KeSynchronizeExecution              = 153,
    ORD_MmLockUnlockPhysicalPage            = 176,
    ORD_MmQueryAddressProtect               = 179,
    ORD_MmQueryAllocationSize               = 180,
    ORD_NtDeleteFile                        = 195,
    ORD_NtFsControlFile                     = 200,
    ORD_NtOpenSymbolicLinkObject            = 203,
    ORD_NtQueryDirectoryFile                = 207,
    ORD_NtQuerySymbolicLinkObject           = 215,
    ORD_NtQueryVirtualMemory                = 217,
    ORD_NtReleaseMutant                     = 221,
    ORD_NtReleaseSemaphore                  = 222,
    ORD_NtResumeThread                      = 224,
    ORD_NtSetSystemTime                     = 228,
    ORD_NtSignalAndWaitForSingleObjectEx    = 230,
    ORD_ObReferenceObjectByName             = 247,
    ORD_RtlCompareMemoryUlong               = 269,
    ORD_RtlRaiseException                   = 302,
    ORD_RtlTimeFieldsToTime                 = 304,
    ORD_RtlTimeToTimeFields                 = 305,
    ORD_RtlUnwind                           = 312,
    ORD_RtlUpcaseUnicodeChar                = 313,
    ORD_XcVerifyPKCS1Signature              = 344,
    ORD_XcModExp                            = 345,
    ORD_HalIsResetOrShutdownPending         = 358,
    ORD_IoMarkIrpMustComplete               = 359,
    ORD_HalInitiateShutdown                 = 360,
    ORD_HalEnableSecureTrayEject            = 365,
    ORD_XcDESKeyParity                  = 346,
    ORD_XcKeyTable                      = 347,
    ORD_XcBlockCrypt                    = 348,
    ORD_XcBlockCryptCBC                 = 349,
    ORD_HalBootSMCVideoMode            = 356,
};

// Returns the guest VA for a data export ordinal, or 0 if it's not a data export
inline uint32_t kernel_data_addr(uint32_t ordinal) {
    switch (ordinal) {
    case ORD_KeTickCount:       return KDATA_KeTickCount();
    case ORD_XboxHardwareInfo:  return KDATA_XboxHardwareInfo();
    case ORD_XboxKrnlVersion:   return KDATA_XboxKrnlVersion();
    case ORD_LaunchDataPage:    return KDATA_LaunchDataPage();
    case ORD_XeImageFileName:   return KDATA_XeImageFileName();
    case 40:  return KDATA_HalDiskCachePartCount();  // HalDiskCachePartitionCount
    case 41:  return KDATA_HalDiskModelNumber();     // HalDiskModelNumber
    case 42:  return KDATA_HalDiskSerialNumber();    // HalDiskSerialNumber
    case 88:  return KDATA_KdDebuggerEnabled();      // KdDebuggerEnabled
    case 89:  return KDATA_KdDebuggerNotPresent();   // KdDebuggerNotPresent
    case 120: return KDATA_KeInterruptTime();        // KeInterruptTime
    case 154: return KDATA_KeSystemTime();           // KeSystemTime
    case 157: return KDATA_KeTimeIncrement();        // KeTimeIncrement
    case 162: return KDATA_KiBugCheckData();         // KiBugCheckData
    case 356: return KDATA_HalBootSMCVideoMode();    // HalBootSMCVideoMode
    case 357: return KDATA_IdexChannelObject();      // IdexChannelObject
    case 102: return KDATA_MmGlobalData();           // MmGlobalData
    case 16:  return KDATA_ExEventObjectType();      // ExEventObjectType
    case 22:  return KDATA_ExMutantObjectType();     // ExMutantObjectType
    case 30:  return KDATA_ExSemaphoreObjectType();  // ExSemaphoreObjectType
    case 31:  return KDATA_ExTimerObjectType();      // ExTimerObjectType
    case 64:  return KDATA_IoCompletionObjectType(); // IoCompletionObjectType
    case 70:  return KDATA_IoDeviceObjectType();     // IoDeviceObjectType
    case 71:  return KDATA_IoFileObjectType();       // IoFileObjectType
    case 240: return KDATA_ObDirectoryObjectType();  // ObDirectoryObjectType
    case 245: return KDATA_ObpObjectHandleTable();   // ObpObjectHandleTable
    case 249: return KDATA_ObSymbolicLinkObjectType(); // ObSymbolicLinkObjectType
    case 259: return KDATA_PsThreadObjectType();     // PsThreadObjectType
    case 321: return KDATA_XboxEEPROMKey();          // XboxEEPROMKey
    case 323: return KDATA_XboxHDKey();              // XboxHDKey
    case 325: return KDATA_XboxSignatureKey();       // XboxSignatureKey
    case 353: return KDATA_XboxLANKey();             // XboxLANKey
    case 354: return KDATA_XboxAltSigKeys();         // XboxAlternateSignatureKeys
    case 355: return KDATA_XePublicKeyData();        // XePublicKeyData
    default:  return 0;
    }
}

// Helper: write an ANSI_STRING {Length, MaxLength, Buffer} into guest RAM.
// `str_va` is the VA of the ANSI_STRING struct (8 bytes), `buf_va` is where
// the character data lives.  Mirrors RtlInitAnsiString but runs on the host.
inline void write_guest_ansi_string(uint8_t* ram, uint32_t str_va,
                                    uint32_t buf_va, const char* text) {
    uint16_t len = (uint16_t)strlen(text);
    uint16_t max_len = len + 1;
    memcpy(ram + str_va + 0, &len, 2);
    memcpy(ram + str_va + 2, &max_len, 2);
    memcpy(ram + str_va + 4, &buf_va, 4);
    memcpy(ram + buf_va, text, len + 1);
}

// Initialize kernel data exports in guest RAM.
// Sets up scalar values, time counters, ANSI_STRING data exports, and
// hardware info.  Object type stubs are left zeroed (valid address is enough).
inline void init_kernel_data(uint8_t* ram) {
    // Zero the entire 4KB KDATA area first
    memset(ram + KDATA_BASE, 0, KDATA_SIZE);

    // KeTickCount: start at 1
    uint32_t tick = 1;
    memcpy(ram + KDATA_KeTickCount(), &tick, 4);

    // XboxHardwareInfo: Flags=0x20 (INTERNAL_USB_HUB), GpuRevision=0xA1, McpRevision=0xD4
    uint32_t hw_flags = 0x00000020u;
    memcpy(ram + KDATA_XboxHardwareInfo(), &hw_flags, 4);
    ram[KDATA_XboxHardwareInfo() + 4] = 0xA1; // GPU revision (NV2A A1)
    ram[KDATA_XboxHardwareInfo() + 5] = 0xD4; // MCP revision (X3)

    // XboxKrnlVersion: 1.0.5838.1
    uint16_t ver[4] = { 1, 0, 5838, 1 };
    memcpy(ram + KDATA_XboxKrnlVersion(), ver, 8);

    // LaunchDataPage: NULL (no launch data)
    uint32_t null_ptr = 0;
    memcpy(ram + KDATA_LaunchDataPage(), &null_ptr, 4);

    // XeImageFileName: ANSI_STRING for the running image
    write_guest_ansi_string(ram, KDATA_XeImageFileName(),
        KDATA_XeImageFileNameBuf(),
        "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe");

    // HalDiskCachePartitionCount: 3 (X, Y, Z)
    uint32_t cache_parts = 3;
    memcpy(ram + KDATA_HalDiskCachePartCount(), &cache_parts, 4);

    // HalDiskModelNumber / HalDiskSerialNumber: ANSI_STRINGs
    write_guest_ansi_string(ram, KDATA_HalDiskModelNumber(),
        KDATA_HalDiskModelNumberBuf(), "XXXXXX");
    write_guest_ansi_string(ram, KDATA_HalDiskSerialNumber(),
        KDATA_HalDiskSerialNumberBuf(), "123456");

    // KdDebuggerEnabled: FALSE (0)
    ram[KDATA_KdDebuggerEnabled()] = 0;
    // KdDebuggerNotPresent: TRUE (1)
    ram[KDATA_KdDebuggerNotPresent()] = 1;

    // KeInterruptTime: KSYSTEM_TIME (LowPart, High1Time, High2Time) — start at 0
    // KeSystemTime: KSYSTEM_TIME — start at 0 (init_kernel_data is called once)

    // KeTimeIncrement: 10000 (= 1ms in 100ns units, matching Xbox timer tick)
    uint32_t time_inc = 10000;
    memcpy(ram + KDATA_KeTimeIncrement(), &time_inc, 4);

    // KiBugCheckData: 5 DWORDs, all zero (no bug check)

    // HalBootSMCVideoMode: 1 = NTSC
    uint32_t smc_mode = 1;
    memcpy(ram + KDATA_HalBootSMCVideoMode(), &smc_mode, 4);

    // IdexChannelObject: zeroed is fine (not actively used in HLE)
    // MmGlobalData: zeroed (not actively used)
    // Object type structures: zeroed is OK — they just need valid addresses
    // Encryption keys: all zero (no real key data in HLE mode)
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

    // GPU instance memory (set by MmClaimGpuInstanceMemory)
    uint32_t gpu_instance_base = 0;
    uint32_t gpu_instance_size = 0;
    void*    nv2a_ptr = nullptr;  // pointer to Nv2aState, set during boot

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

    // Set up the XBE path (mounts are configured separately by boot_hle)
    void set_xbe_path(const std::string& xbe_path) {
        size_t last_sep = xbe_path.find_last_of("/\\");
        xbe_directory = (last_sep != std::string::npos) ? xbe_path.substr(0, last_sep) : ".";
    }

    // Add a mount point (both NT object path and bare letter form)
    void mount_drive(const char* nt_prefix, const char* letter,
                     const std::string& host_dir) {
        mounts.push_back({nt_prefix, host_dir});
        // Add bare letter variants (e.g. "c:", "C:")
        char lower[3] = { (char)tolower(letter[0]), ':', 0 };
        char upper[3] = { (char)toupper(letter[0]), ':', 0 };
        mounts.push_back({lower, host_dir});
        mounts.push_back({upper, host_dir});
    }

    // Add a device mount (NT path only, no drive letter)
    void mount_device(const char* device_path, const std::string& host_dir) {
        mounts.push_back({device_path, host_dir});
    }

    // Translate Xbox path to host path
    std::string translate_path(const std::string& xbox_path) const {
        for (auto& m : mounts) {
            if (_strnicmp(xbox_path.c_str(), m.xbox_prefix.c_str(), m.xbox_prefix.size()) == 0) {
                if (xbox_path.size() == m.xbox_prefix.size()) {
                    // Exact match (e.g. "CDROM0:" → host_dir)
                    return m.host_dir;
                }
                char sep = xbox_path[m.xbox_prefix.size()];
                if (sep == '\\' || sep == '/') {
                    std::string rel = xbox_path.substr(m.xbox_prefix.size() + 1);
                    for (auto& c : rel) if (c == '\\') c = '/';
                    return m.host_dir + "/" + rel;
                }
            }
        }
        return ""; // no mount matched
    }

    // Open a host file, return handle or 0 on failure
    // Open a host file, return handle or 0 on failure.
    // Also succeeds for directories (returns handle with fp=nullptr, size=0).
    uint32_t open_host_file(const std::string& host_path) {
        FILE* fp = fopen(host_path.c_str(), "rb");
        if (!fp) {
            // Check if it's a directory — common for partition root opens.
            struct _stat st;
            if (_stat(host_path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR)) {
                uint32_t h = next_handle++;
                open_files[h] = {nullptr, 0, host_path};
                return h;
            }
            return 0;
        }
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
        if (it->second.fp) fclose(it->second.fp);
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
        fprintf(stderr, "[hle] MmAllocateContiguousMemoryEx: size=%08X align=%08X -> addr=%08X\n",
                size, align, addr);
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_ExFreePool:
    case ORD_MmFreeContiguousMemory:
        // Leak: don't track frees in the bump allocator
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_MmPersistContiguousMemory:
        // VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
        // No-op: persistence across quick-reboots is not emulated.
        stdcall_cleanup(exec, 3);
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
        case 0x10: result_val = 0; break;          // Audio flags: stereo
        case 0x11: result_val = 0; break;          // Parental control: no restrictions
        case 0x103: result_val = 0x00000100; break; // Factory AV region: NTSC_M
        case 0x104: result_val = 0x00000001; break; // Factory game region: North America
        default: break;
        }

        if (type_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + type_ptr, &type, 4);
        if (value_ptr + result_size <= GUEST_RAM_SIZE && result_size <= value_len)
            memcpy(exec.ram + value_ptr, &result_val, result_size);
        if (result_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + result_ptr, &result_size, 4);

        fprintf(stderr, "[hle] ExQueryNonVolatileSetting(0x%02X) -> 0x%08X\n",
                value_index, result_val);
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;
    }

    case ORD_RtlNtStatusToDosError: {
        // ULONG RtlNtStatusToDosError(NTSTATUS Status)
        uint32_t status = stack_arg(exec, 0);
        uint32_t dos_error;
        switch (status) {
        case 0x00000000u: dos_error = 0; break;                     // STATUS_SUCCESS
        case 0xC0000034u: dos_error = 2; break;                     // STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND
        case 0xC000003Au: dos_error = 3; break;                     // STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND
        case 0xC0000035u: dos_error = 183; break;                   // STATUS_OBJECT_NAME_COLLISION → ERROR_ALREADY_EXISTS
        case 0xC0000022u: dos_error = 5; break;                     // STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED
        case 0xC0000008u: dos_error = 6; break;                     // STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE
        case 0xC000000Du: dos_error = 87; break;                    // STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER
        case 0xC0000017u: dos_error = 8; break;                     // STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY
        case 0xC0000002u: dos_error = 1; break;                     // STATUS_NOT_IMPLEMENTED → ERROR_INVALID_FUNCTION
        default:          dos_error = (status >> 30) ? 317u : 0; break; // fallback
        }
        exec.ctx.gp[GP_EAX] = dos_error;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_HalReturnToFirmware: {
        // HalReturnToFirmware(FIRMWARE_REENTRY: 0=Halt, 1=Reboot, 2=QuickReboot, 3=KdReboot, 4=Fatal)
        uint32_t reason = stack_arg(exec, 0);
        fprintf(stderr, "[hle] HalReturnToFirmware(%u) at EIP=0x%08X\n", reason, exec.ctx.eip);
        // Dump stack frames for debugging
        uint32_t esp_val = exec.ctx.gp[GP_ESP];
        uint32_t ebp_val = exec.ctx.gp[GP_EBP];
        fprintf(stderr, "[hle]   ESP=0x%08X EBP=0x%08X\n", esp_val, ebp_val);
        // Walk EBP chain for up to 10 frames
        for (int i = 0; i < 10 && ebp_val > 0 && ebp_val + 8 <= GUEST_RAM_SIZE; ++i) {
            uint32_t saved_ebp = 0, ret_addr = 0;
            memcpy(&saved_ebp, exec.ram + ebp_val, 4);
            memcpy(&ret_addr, exec.ram + ebp_val + 4, 4);
            fprintf(stderr, "[hle]   frame %d: [%08X] ret=%08X\n", i, ebp_val, ret_addr);
            if (saved_ebp <= ebp_val) break; // ascending or stuck
            ebp_val = saved_ebp;
        }
        // Also dump raw stack words
        fprintf(stderr, "[hle]   stack:");
        for (int i = 0; i < 16 && esp_val + i*4 + 4 <= GUEST_RAM_SIZE; ++i) {
            uint32_t w = 0;
            memcpy(&w, exec.ram + esp_val + i*4, 4);
            fprintf(stderr, " %08X", w);
        }
        fprintf(stderr, "\n");
        stdcall_cleanup(exec, 1);
        exec.ctx.eip = 0xFFFFFFFF;
        exec.ctx.halted = true;
        return true;
    }

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
        // Current thread exits. The scheduler in run_step will switch to
        // the next alive thread.
        stdcall_cleanup(exec, 1);
        exec.ctx.eip = 0xFFFFFFFF;
        exec.ctx.halted = true;
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

    case ORD_ObReferenceObjectByHandle: {
        // NTSTATUS ObReferenceObjectByHandle(HANDLE, ACCESS_MASK, POBJECT_TYPE,
        //          KPROCESSOR_MODE, PVOID *Object, POBJECT_HANDLE_INFORMATION)
        uint32_t obj_out = stack_arg(exec, 4);
        if (obj_out + 4 <= GUEST_RAM_SIZE) {
            // Return a fake object pointer (use a page from the heap)
            uint32_t fake_obj = heap->alloc(0x100);
            if (fake_obj) memset(exec.ram + fake_obj, 0, 0x100);
            memcpy(exec.ram + obj_out, &fake_obj, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 6);
        return true;
    }

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
        // PhysAddr is LARGE_INTEGER = 8 bytes on stack. Total = 8+4+4 = 16 = 4 dwords.
        // On Xbox, MMIO is identity-mapped. Return PhysAddr.lo as-is.
        uint32_t phys = stack_arg(exec, 0);
        fprintf(stderr, "[hle] MmMapIoSpace: phys=0x%08X → va=0x%08X\n", phys, phys);
        exec.ctx.gp[GP_EAX] = phys;
        stdcall_cleanup(exec, 4);
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

    case ORD_IoCreateDevice: {
        // NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG ExtensionSize,
        //     PSTRING DeviceName, ULONG DeviceType, BOOLEAN Exclusive,
        //     OUT PDEVICE_OBJECT *DeviceObject)
        uint32_t ext_size   = stack_arg(exec, 1);
        uint32_t dev_obj_pp = stack_arg(exec, 5);
        // Allocate a fake DEVICE_OBJECT (>= 0xB8 bytes) + extension
        uint32_t obj_size = 0xB8 + ext_size;
        uint32_t obj_addr = heap->alloc(obj_size);
        if (obj_addr) memset(exec.ram + obj_addr, 0, obj_size);
        if (dev_obj_pp + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + dev_obj_pp, &obj_addr, 4);
        // Write DeviceExtension pointer at offset 0x28 in DEVICE_OBJECT
        if (obj_addr && obj_addr + 0x2C <= GUEST_RAM_SIZE) {
            uint32_t ext_ptr = (ext_size > 0) ? obj_addr + 0xB8 : 0;
            memcpy(exec.ram + obj_addr + 0x28, &ext_ptr, 4);
        }
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 6);
        return true;
    }

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
            // No mount matched — return not-found.
            fprintf(stderr, "[hle] NtOpenFile: '%s' -> not found (no mount)\n",
                    xbox_path.c_str());
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

    case ORD_NtSetInformationFile: {
        // NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID FileInfo, ULONG Length, FILE_INFORMATION_CLASS)
        uint32_t handle     = stack_arg(exec, 0);
        uint32_t info_ptr   = stack_arg(exec, 2);
        uint32_t info_class = stack_arg(exec, 4);

        HostFile* hf = heap->get_host_file(handle);
        if (hf && hf->fp && info_class == 14 && info_ptr + 8 <= GUEST_RAM_SIZE) {
            // FilePositionInformation: set file pointer
            int64_t pos = 0;
            memcpy(&pos, exec.ram + info_ptr, 8);
            _fseeki64(hf->fp, pos, SEEK_SET);
        }

        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;
    }

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
        uint32_t create_disp = stack_arg(exec, 7); // CreateDisposition
        uint32_t create_opts = stack_arg(exec, 8); // CreateOptions

        std::string xbox_path = read_object_name(exec, obj_attrs);
        std::string host_path = heap->translate_path(xbox_path);

        // CreateDisposition: FILE_SUPERSEDE=0, FILE_OPEN=1, FILE_CREATE=2,
        // FILE_OPEN_IF=3, FILE_OVERWRITE=4, FILE_OVERWRITE_IF=5
        constexpr uint32_t FILE_OPEN = 1;
        constexpr uint32_t FILE_OVERWRITE = 4;
        // CreateOptions
        constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
        bool open_only = (create_disp == FILE_OPEN || create_disp == FILE_OVERWRITE);
        bool is_dir    = (create_opts & FILE_DIRECTORY_FILE) != 0;

        uint32_t h = 0;
        uint32_t status = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        bool is_device_open = false;   // true if opening a volume root
        if (!host_path.empty()) {
            // Check if this is a bare device/volume open (path == mount base)
            for (auto& m : heap->mounts) {
                if (_strnicmp(xbox_path.c_str(), m.xbox_prefix.c_str(),
                              m.xbox_prefix.size()) == 0 &&
                    xbox_path.size() == m.xbox_prefix.size()) {
                    is_device_open = true;
                    break;
                }
            }
            h = heap->open_host_file(host_path);
            if (h) {
                status = 0; // STATUS_SUCCESS
                fprintf(stderr, "[hle] NtCreateFile: '%s' -> handle 0x%X\n",
                        xbox_path.c_str(), h);
            } else if (is_device_open || is_dir || !open_only) {
                // Device/volume open, directory operation, or create disposition.
                h = heap->next_handle++;
                status = 0;
                fprintf(stderr, "[hle] NtCreateFile: '%s' -> fake handle 0x%X (%s)\n",
                        xbox_path.c_str(), h,
                        is_device_open ? "device" : "created");
            } else {
                // Mount matched, file doesn't exist, and disposition is
                // FILE_OPEN — return not-found (e.g. xonlinedash.xbe check).
                fprintf(stderr, "[hle] NtCreateFile: '%s' -> not found (%s) disp=%u\n",
                        xbox_path.c_str(), host_path.c_str(), create_disp);
            }
        } else {
            // No mount matched — return not-found.
            fprintf(stderr, "[hle] NtCreateFile: '%s' -> not found (no mount)\n",
                    xbox_path.c_str());
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

    case ORD_MmClaimGpuInstanceMemory: {
        // PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, PSIZE_T NumberOfPaddingBytes)
        uint32_t num_bytes = stack_arg(exec, 0);
        uint32_t pad_ptr   = stack_arg(exec, 1);
        fprintf(stderr, "[hle] MmClaimGpuInstanceMemory(0x%X, 0x%X)\n", num_bytes, pad_ptr);
        uint32_t addr = heap->alloc(num_bytes > 0 ? num_bytes : 0x1000);
        if (addr && num_bytes > 0) memset(exec.ram + addr, 0, num_bytes);
        if (pad_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t pad = 0;
            memcpy(exec.ram + pad_ptr, &pad, 4);
        }
        // Wire PRAMIN to this GPU instance memory in guest RAM
        heap->gpu_instance_base = addr;
        heap->gpu_instance_size = num_bytes > 0 ? num_bytes : 0x1000;
        if (heap->nv2a_ptr) {
            auto* nv = static_cast<xbox::Nv2aState*>(heap->nv2a_ptr);
            nv->pramin_ram      = exec.ram;
            nv->pramin_ram_base = addr;
            nv->pramin_ram_size = num_bytes > 0 ? num_bytes : 0x1000;
        }
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_MmCreateKernelStack: {
        // PVOID MmCreateKernelStack(SIZE_T NumberOfBytes, BOOLEAN DebuggerThread)
        uint32_t size = stack_arg(exec, 0);
        if (size == 0) size = 0x10000;
        uint32_t base = heap->alloc(size);
        exec.ctx.gp[GP_EAX] = base ? (base + size) : 0;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_MmDeleteKernelStack:
        // void MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_MmAllocateSystemMemory: {
        // PVOID MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect)
        uint32_t size = stack_arg(exec, 0);
        uint32_t addr = heap->alloc(size > 0 ? size : 0x1000);
        if (addr && size > 0) memset(exec.ram + addr, 0, size);
        exec.ctx.gp[GP_EAX] = addr;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_MmFreeSystemMemory:
        // ULONG MmFreeSystemMemory(PVOID, SIZE_T)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_MmIsAddressValid:
        // BOOLEAN MmIsAddressValid(PVOID VirtualAddress)
        exec.ctx.gp[GP_EAX] = (stack_arg(exec, 0) < GUEST_RAM_SIZE) ? 1 : 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_MmQueryStatistics: {
        // NTSTATUS MmQueryStatistics(PMM_STATISTICS)
        uint32_t ptr = stack_arg(exec, 0);
        if (ptr + 0x24 <= GUEST_RAM_SIZE) {
            memset(exec.ram + ptr, 0, 0x24);
            uint32_t total = GUEST_RAM_SIZE / 0x1000;
            uint32_t avail = total / 2;
            memcpy(exec.ram + ptr + 0x04, &total, 4);
            memcpy(exec.ram + ptr + 0x0C, &avail, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_MmSetAddressProtect:
        // void MmSetAddressProtect(PVOID, ULONG, ULONG)
        stdcall_cleanup(exec, 3);
        return true;




    case ORD_NtYieldExecution:
        // NTSTATUS NtYieldExecution(void)
        exec.ctx.gp[GP_EAX] = 0;
        exec.ctx.halted = true;
        return true;

    case ORD_NtFlushBuffersFile:
        // NTSTATUS NtFlushBuffersFile(HANDLE, PIO_STATUS_BLOCK)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;


    case ORD_NtDuplicateObject: {
        // NTSTATUS NtDuplicateObject(HANDLE Source, PHANDLE Target, ULONG Options)
        uint32_t source     = stack_arg(exec, 0);
        uint32_t target_ptr = stack_arg(exec, 1);
        if (target_ptr + 4 <= GUEST_RAM_SIZE)
            memcpy(exec.ram + target_ptr, &source, 4);
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        return true;
    }


    case ORD_ObfReferenceObject:
        // void ObfReferenceObject(PVOID)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlAnsiStringToUnicodeString: {
        // NTSTATUS RtlAnsiStringToUnicodeString(PUNICODE_STRING, PCANSI_STRING, BOOLEAN)
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        uint16_t src_len = 0;
        uint32_t src_buf = 0;
        if (src + 8 <= GUEST_RAM_SIZE) {
            memcpy(&src_len, exec.ram + src, 2);
            memcpy(&src_buf, exec.ram + src + 4, 4);
        }
        uint32_t uni_len = (uint32_t)src_len * 2;
        uint32_t uni_buf = heap->alloc(uni_len + 2);
        if (uni_buf && src_buf + src_len <= GUEST_RAM_SIZE) {
            for (uint16_t i = 0; i < src_len; i++) {
                uint16_t ch = exec.ram[src_buf + i];
                memcpy(exec.ram + uni_buf + i * 2, &ch, 2);
            }
            uint16_t zero = 0;
            memcpy(exec.ram + uni_buf + uni_len, &zero, 2);
        }
        if (dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t u_len = (uint16_t)uni_len;
            uint16_t u_max = (uint16_t)(uni_len + 2);
            memcpy(exec.ram + dest + 0, &u_len, 2);
            memcpy(exec.ram + dest + 2, &u_max, 2);
            memcpy(exec.ram + dest + 4, &uni_buf, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_RtlUnicodeStringToAnsiString: {
        // NTSTATUS RtlUnicodeStringToAnsiString(PANSI_STRING, PCUNICODE_STRING, BOOLEAN)
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        uint16_t src_len = 0;
        uint32_t src_buf = 0;
        if (src + 8 <= GUEST_RAM_SIZE) {
            memcpy(&src_len, exec.ram + src, 2);
            memcpy(&src_buf, exec.ram + src + 4, 4);
        }
        uint32_t ansi_len = src_len / 2;
        uint32_t ansi_buf = heap->alloc(ansi_len + 1);
        if (ansi_buf && src_buf + src_len <= GUEST_RAM_SIZE) {
            for (uint32_t i = 0; i < ansi_len; i++) {
                uint16_t ch;
                memcpy(&ch, exec.ram + src_buf + i * 2, 2);
                exec.ram[ansi_buf + i] = (uint8_t)(ch & 0xFF);
            }
            exec.ram[ansi_buf + ansi_len] = 0;
        }
        if (dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t a_len = (uint16_t)ansi_len;
            uint16_t a_max = (uint16_t)(ansi_len + 1);
            memcpy(exec.ram + dest + 0, &a_len, 2);
            memcpy(exec.ram + dest + 2, &a_max, 2);
            memcpy(exec.ram + dest + 4, &ansi_buf, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_RtlFreeAnsiString:
        // void RtlFreeAnsiString(PANSI_STRING)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlFreeUnicodeString:
        // void RtlFreeUnicodeString(PUNICODE_STRING)
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_RtlTryEnterCriticalSection: {
        // BOOLEAN RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION)
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            int32_t lock = 0;
            memcpy(exec.ram + cs + 0x04, &lock, 4);
            int32_t rec = 1;
            memcpy(exec.ram + cs + 0x08, &rec, 4);
        }
        exec.ctx.gp[GP_EAX] = 1;
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_KeLeaveCriticalRegion:
        // void KeLeaveCriticalRegion(void)
        return true;



    case ORD_KeInitializeQueue:
        // void KeInitializeQueue(PKQUEUE, ULONG)
        stdcall_cleanup(exec, 2);
        return true;




    case ORD_ExSaveNonVolatileSetting:
        // NTSTATUS ExSaveNonVolatileSetting(4 args)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_RtlEnterCriticalSectionAndRegion: {
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            int32_t lock = 0;
            memcpy(exec.ram + cs + 0x04, &lock, 4);
            int32_t rec = 1;
            memcpy(exec.ram + cs + 0x08, &rec, 4);
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_RtlLeaveCriticalSectionAndRegion: {
        uint32_t cs = stack_arg(exec, 0);
        if (cs + 0x18 <= GUEST_RAM_SIZE) {
            int32_t lock = -1;
            memcpy(exec.ram + cs + 0x04, &lock, 4);
            int32_t rec = 0;
            memcpy(exec.ram + cs + 0x08, &rec, 4);
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_RtlCopyUnicodeString: {
        // void RtlCopyUnicodeString(PUNICODE_STRING Dest, PCUNICODE_STRING Src)
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        if (src + 8 <= GUEST_RAM_SIZE && dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t src_len = 0, dest_max = 0;
            uint32_t src_buf = 0, dest_buf = 0;
            memcpy(&src_len,  exec.ram + src + 0, 2);
            memcpy(&src_buf,  exec.ram + src + 4, 4);
            memcpy(&dest_max, exec.ram + dest + 2, 2);
            memcpy(&dest_buf, exec.ram + dest + 4, 4);
            uint16_t copy_len = (src_len < dest_max) ? src_len : dest_max;
            if (src_buf + copy_len <= GUEST_RAM_SIZE && dest_buf + copy_len <= GUEST_RAM_SIZE)
                memcpy(exec.ram + dest_buf, exec.ram + src_buf, copy_len);
            memcpy(exec.ram + dest + 0, &copy_len, 2);
        }
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_RtlCreateUnicodeString: {
        // BOOLEAN RtlCreateUnicodeString(PUNICODE_STRING Dest, PCWSTR Src)
        uint32_t dest = stack_arg(exec, 0);
        uint32_t src  = stack_arg(exec, 1);
        uint16_t len = 0;
        if (src && src < GUEST_RAM_SIZE) {
            while (src + len + 1 < GUEST_RAM_SIZE) {
                uint16_t ch;
                memcpy(&ch, exec.ram + src + len, 2);
                if (ch == 0) break;
                len += 2;
            }
        }
        uint32_t buf = heap->alloc(len + 2);
        if (buf && src + len <= GUEST_RAM_SIZE)
            memcpy(exec.ram + buf, exec.ram + src, len + 2);
        if (dest + 8 <= GUEST_RAM_SIZE) {
            uint16_t max_len = len + 2;
            memcpy(exec.ram + dest + 0, &len, 2);
            memcpy(exec.ram + dest + 2, &max_len, 2);
            memcpy(exec.ram + dest + 4, &buf, 4);
        }
        exec.ctx.gp[GP_EAX] = 1;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_InterlockedExchangeAdd: {
        // LONG InterlockedExchangeAdd(LONG volatile *Addend, LONG Value)
        uint32_t ptr = stack_arg(exec, 0);
        int32_t val = (int32_t)stack_arg(exec, 1);
        int32_t old_val = 0;
        if (ptr + 4 <= GUEST_RAM_SIZE) {
            memcpy(&old_val, exec.ram + ptr, 4);
            int32_t new_val = old_val + val;
            memcpy(exec.ram + ptr, &new_val, 4);
        }
        exec.ctx.gp[GP_EAX] = (uint32_t)old_val;
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_NtWaitForSingleObjectEx:
        // NTSTATUS NtWaitForSingleObjectEx(HANDLE, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        exec.ctx.halted = true;
        return true;

    case ORD_XeUnloadSection:
        // NTSTATUS XeUnloadSection(PXBE_SECTION_HEADER)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_IoDismountVolume:
        // NTSTATUS IoDismountVolume(PDEVICE_OBJECT)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

        case ORD_AvSendTVEncoderOption:
        // void AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result)
        stdcall_cleanup(exec, 4);
        return true;

    case ORD_DbgBreakPoint:
        // void DbgBreakPoint(void)
        fprintf(stderr, "[hle] DbgBreakPoint at EIP=0x%08X\n", exec.ctx.eip);
        return true;

    case ORD_ExAcquireReadWriteLockExclusive:
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_ExAcquireReadWriteLockShared:
        stdcall_cleanup(exec, 1);
        return true;


    case ORD_HalReadWritePCISpace: {
        // void HalReadWritePCISpace(ULONG BusNumber, ULONG SlotNumber,
        //     ULONG RegisterNumber, PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace)
        uint32_t bus      = stack_arg(exec, 0);
        uint32_t slot     = stack_arg(exec, 1);
        uint32_t reg      = stack_arg(exec, 2);
        uint32_t buf_ptr  = stack_arg(exec, 3);
        uint32_t length   = stack_arg(exec, 4);
        uint32_t is_write = stack_arg(exec, 5);

        // Build PCI config address: enable bit (31) | bus | dev | fn=0 | reg
        uint32_t cfg_addr = 0x80000000u | ((bus & 0xFF) << 16)
                          | ((slot & 0x1F) << 11) | (reg & 0xFC);
        exec.io_write(0x0CF8, cfg_addr, 4);

        if (is_write) {
            // Write from guest buffer to PCI config space
            if (buf_ptr + length <= GUEST_RAM_SIZE) {
                for (uint32_t off = 0; off < length; off += 4) {
                    uint32_t val = 0;
                    uint32_t chunk = (length - off >= 4) ? 4 : (length - off);
                    memcpy(&val, exec.ram + buf_ptr + off, chunk);
                    exec.io_write(0x0CFC + (reg & 3), val, chunk);
                    if (off + 4 < length) {
                        cfg_addr += 4;
                        exec.io_write(0x0CF8, cfg_addr, 4);
                    }
                }
            }
        } else {
            // Read from PCI config space to guest buffer
            if (buf_ptr + length <= GUEST_RAM_SIZE) {
                for (uint32_t off = 0; off < length; off += 4) {
                    uint32_t chunk = (length - off >= 4) ? 4 : (length - off);
                    uint32_t val = exec.io_read(0x0CFC + (reg & 3), chunk);
                    memcpy(exec.ram + buf_ptr + off, &val, chunk);
                    if (off + 4 < length) {
                        cfg_addr += 4;
                        exec.io_write(0x0CF8, cfg_addr, 4);
                    }
                }
            }
        }
        stdcall_cleanup(exec, 6);
        return true;
    }

    case ORD_KeInitializeApc:
        // void KeInitializeApc(7 args)
        stdcall_cleanup(exec, 7);
        return true;

    case ORD_KeInsertQueueDpc: {
        // BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
        uint32_t dpc_va = stack_arg(exec, 0);
        if (dpc_va + 0x1C <= GUEST_RAM_SIZE) {
            uint32_t routine = 0, context = 0;
            memcpy(&routine, exec.ram + dpc_va + 0x0C, 4);
            memcpy(&context, exec.ram + dpc_va + 0x10, 4);
            if (routine)
                heap->pending_dpcs.push_back({routine, context, dpc_va, 0});
        }
        exec.ctx.gp[GP_EAX] = 1; // TRUE (inserted)
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_KeSetBasePriorityThread:
        // LONG KeSetBasePriorityThread(PKTHREAD, LONG Increment)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;

    case ORD_KfRaiseIrql:
        // KIRQL KfRaiseIrql(KIRQL) - fastcall, arg in CL
        exec.ctx.gp[GP_EAX] = 0; // old IRQL = PASSIVE_LEVEL
        return true;

    case ORD_KiUnlockDispatcherDatabase:
        // void KiUnlockDispatcherDatabase(KIRQL) - fastcall
        return true;

    case ORD_NtWriteFile: {
        // NTSTATUS NtWriteFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
        //     PIO_STATUS_BLOCK, PVOID, ULONG Length, PLARGE_INTEGER)
        uint32_t iosb_ptr = stack_arg(exec, 4);
        uint32_t length   = stack_arg(exec, 6);
        if (iosb_ptr + 8 <= GUEST_RAM_SIZE) {
            uint32_t status = 0;
            memcpy(exec.ram + iosb_ptr, &status, 4);
            memcpy(exec.ram + iosb_ptr + 4, &length, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 8);
        return true;
    }


    // ---- I/O manager stubs ----
    case ORD_IoBuildSynchronousFsdRequest:
        exec.ctx.gp[GP_EAX] = 0; // NULL IRP
        stdcall_cleanup(exec, 6);
        return true;
    case ORD_IoInvalidDeviceRequest:
        exec.ctx.gp[GP_EAX] = 0xC0000010u; // STATUS_INVALID_DEVICE_REQUEST
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_IoStartNextPacket:
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_IoStartPacket:
        stdcall_cleanup(exec, 4);
        return true;
    case ORD_IoSynchronousDeviceIoControlRequest:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 7);
        return true;
    case ORD_IoSynchronousFsdRequest:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 4);
        return true;
    case ORD_IofCallDriver: {
        // NTSTATUS fastcall IofCallDriver(PDEVICE_OBJECT Device, PIRP Irp)
        // Args: ECX=Device, EDX=Irp
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        return true;
    }
    case ORD_IofCompleteRequest:
        // VOID fastcall IofCompleteRequest(PIRP Irp, CCHAR Boost)
        return true;
    case ORD_IoMarkIrpMustComplete:
        // BOOLEAN IoMarkIrpMustComplete(PIRP Irp)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Kernel scheduler/DPC stubs ----
    case ORD_KeDisconnectInterrupt:
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_KeQueryBasePriorityThread:
        exec.ctx.gp[GP_EAX] = 8; // THREAD_PRIORITY_NORMAL
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_KeRemoveQueueDpc:
        exec.ctx.gp[GP_EAX] = 0; // FALSE (wasn't queued)
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_KeRestoreFloatingPointState:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_KeSaveFloatingPointState:
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_KeSynchronizeExecution:
        // BOOLEAN KeSynchronizeExecution(PKINTERRUPT, PKSYNC_ROUTINE, PVOID)
        // Just call the routine -- stub: return TRUE
        exec.ctx.gp[GP_EAX] = 1;
        stdcall_cleanup(exec, 3);
        return true;

    // ---- Memory manager stubs ----
    case ORD_MmLockUnlockPhysicalPage:
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_MmQueryAddressProtect:
        exec.ctx.gp[GP_EAX] = 0x04; // PAGE_READWRITE
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_MmQueryAllocationSize:
        exec.ctx.gp[GP_EAX] = 0x1000; // 4KB default
        stdcall_cleanup(exec, 1);
        return true;

    // ---- Nt object manager stubs ----
    case ORD_NtDeleteFile: {
        // NTSTATUS NtDeleteFile(POBJECT_ATTRIBUTES)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS (pretend)
        stdcall_cleanup(exec, 1);
        return true;
    }
    case ORD_NtFsControlFile:
        exec.ctx.gp[GP_EAX] = 0xC0000002u; // STATUS_NOT_IMPLEMENTED
        stdcall_cleanup(exec, 10);
        return true;
    case ORD_NtOpenSymbolicLinkObject:
        // NTSTATUS NtOpenSymbolicLinkObject(PHANDLE, POBJECT_ATTRIBUTES)
        exec.ctx.gp[GP_EAX] = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_NtQueryDirectoryFile: {
        // NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE,
        //   PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PUNICODE_STRING, BOOLEAN)
        // Return no more files
        uint32_t iosb = stack_arg(exec, 4);
        if (iosb + 8 <= GUEST_RAM_SIZE) {
            uint32_t status = 0x80000006u; // STATUS_NO_MORE_FILES
            uint32_t info = 0;
            memcpy(exec.ram + iosb, &status, 4);
            memcpy(exec.ram + iosb + 4, &info, 4);
        }
        exec.ctx.gp[GP_EAX] = 0x80000006u; // STATUS_NO_MORE_FILES
        stdcall_cleanup(exec, 9);
        return true;
    }
    case ORD_NtQuerySymbolicLinkObject:
        exec.ctx.gp[GP_EAX] = 0xC0000008u; // STATUS_INVALID_HANDLE
        stdcall_cleanup(exec, 3);
        return true;
    case ORD_NtQueryVirtualMemory: {
        // NTSTATUS NtQueryVirtualMemory(PVOID Base, PMEMORY_BASIC_INFORMATION, ULONG Len, PULONG RetLen)
        uint32_t info_ptr = stack_arg(exec, 1);
        uint32_t info_len = stack_arg(exec, 2);
        if (info_ptr + info_len <= GUEST_RAM_SIZE && info_len >= 28) {
            memset(exec.ram + info_ptr, 0, info_len);
            // Fill in basic info: AllocationBase, RegionSize, State=MEM_COMMIT, Protect=RW
            uint32_t base = stack_arg(exec, 0) & ~0xFFF;
            uint32_t region = 0x1000;
            uint32_t state = 0x1000; // MEM_COMMIT
            uint32_t protect = 0x04; // PAGE_READWRITE
            memcpy(exec.ram + info_ptr + 0, &base, 4);    // BaseAddress
            memcpy(exec.ram + info_ptr + 4, &base, 4);    // AllocationBase
            memcpy(exec.ram + info_ptr + 8, &protect, 4); // AllocationProtect
            memcpy(exec.ram + info_ptr + 12, &region, 4); // RegionSize
            memcpy(exec.ram + info_ptr + 16, &state, 4);  // State
            memcpy(exec.ram + info_ptr + 20, &protect, 4);// Protect
            // Type=MEM_PRIVATE
            uint32_t type = 0x20000;
            memcpy(exec.ram + info_ptr + 24, &type, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 4);
        return true;
    }
    case ORD_NtReleaseMutant:
        // NTSTATUS NtReleaseMutant(HANDLE, PLONG PreviousCount)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_NtReleaseSemaphore: {
        // NTSTATUS NtReleaseSemaphore(HANDLE, ULONG ReleaseCount, PULONG PreviousCount)
        uint32_t prev_ptr = stack_arg(exec, 2);
        if (prev_ptr && prev_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t prev = 0;
            memcpy(exec.ram + prev_ptr, &prev, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        return true;
    }
    case ORD_NtResumeThread: {
        // NTSTATUS NtResumeThread(HANDLE, PULONG PreviousSuspendCount)
        uint32_t prev_ptr = stack_arg(exec, 1);
        if (prev_ptr && prev_ptr + 4 <= GUEST_RAM_SIZE) {
            uint32_t prev = 0;
            memcpy(exec.ram + prev_ptr, &prev, 4);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;
    }
    case ORD_NtSetSystemTime:
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 2);
        return true;
    case ORD_NtSignalAndWaitForSingleObjectEx:
        // NTSTATUS(HANDLE SignalObj, HANDLE WaitObj, ULONG WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
        exec.ctx.gp[GP_EAX] = 0; // STATUS_SUCCESS
        stdcall_cleanup(exec, 5);
        return true;

    // ---- Object manager stubs ----
    case ORD_ObReferenceObjectByName:
        // NTSTATUS(POBJECT_STRING, ULONG, PACCESS_STATE, ACCESS_MASK, POBJECT_TYPE, KPROCESSOR_MODE, PVOID, PVOID*)
        exec.ctx.gp[GP_EAX] = 0xC0000034u; // STATUS_OBJECT_NAME_NOT_FOUND
        stdcall_cleanup(exec, 8);
        return true;

    // ---- Ex/HAL stubs ----
    case ORD_ExReadWriteRefurbInfo:
        // NTSTATUS ExReadWriteRefurbInfo(PXBOX_REFURB_INFO, ULONG, BOOLEAN)
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 3);
        return true;
    case ORD_ExReleaseReadWriteLock:
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_HalIsResetOrShutdownPending:
        exec.ctx.gp[GP_EAX] = 0; // FALSE
        return true;
    case ORD_HalInitiateShutdown:
        fprintf(stderr, "[hle] HalInitiateShutdown called\n");
        exec.ctx.halted = true;
        return true;
    case ORD_HalEnableSecureTrayEject:
        stdcall_cleanup(exec, 0);
        return true;

    // ---- RTL stubs ----
    case ORD_RtlCompareMemoryUlong: {
        // SIZE_T RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern)
        uint32_t src_ptr = stack_arg(exec, 0);
        uint32_t length  = stack_arg(exec, 1);
        uint32_t pattern = stack_arg(exec, 2);
        uint32_t matched = 0;
        if (src_ptr + length <= GUEST_RAM_SIZE) {
            for (uint32_t i = 0; i + 4 <= length; i += 4) {
                uint32_t val;
                memcpy(&val, exec.ram + src_ptr + i, 4);
                if (val != pattern) break;
                matched = i + 4;
            }
        }
        exec.ctx.gp[GP_EAX] = matched;
        stdcall_cleanup(exec, 3);
        return true;
    }
    case ORD_RtlRaiseException:
        // Just log and halt -- this is a fatal exception
        fprintf(stderr, "[hle] RtlRaiseException at EIP=0x%08X\n", exec.ctx.eip);
        exec.ctx.halted = true;
        stdcall_cleanup(exec, 1);
        return true;
    case ORD_RtlTimeFieldsToTime: {
        // BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS, PLARGE_INTEGER)
        // Stub: write epoch time
        uint32_t time_ptr = stack_arg(exec, 1);
        if (time_ptr + 8 <= GUEST_RAM_SIZE) {
            // Windows epoch: January 1, 1601 in 100ns intervals
            // For Xbox, just use a reasonable time: 2004-01-01 = 127488000000000000
            int64_t t = 127488000000000000LL;
            memcpy(exec.ram + time_ptr, &t, 8);
        }
        exec.ctx.gp[GP_EAX] = 1; // TRUE
        stdcall_cleanup(exec, 2);
        return true;
    }
    case ORD_RtlTimeToTimeFields: {
        // VOID RtlTimeToTimeFields(PLARGE_INTEGER, PTIME_FIELDS)
        // TIME_FIELDS: Year(2), Month(2), Day(2), Hour(2), Minute(2), Second(2), Milliseconds(2), Weekday(2)
        uint32_t tf_ptr = stack_arg(exec, 1);
        if (tf_ptr + 16 <= GUEST_RAM_SIZE) {
            memset(exec.ram + tf_ptr, 0, 16);
            uint16_t year = 2004, month = 1, day = 1;
            memcpy(exec.ram + tf_ptr + 0, &year, 2);
            memcpy(exec.ram + tf_ptr + 2, &month, 2);
            memcpy(exec.ram + tf_ptr + 4, &day, 2);
        }
        stdcall_cleanup(exec, 2);
        return true;
    }
    case ORD_RtlUnwind:
        // VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD, PVOID ReturnValue)
        // This is complex SEH unwinding -- for now just return
        stdcall_cleanup(exec, 4);
        return true;
    case ORD_RtlUpcaseUnicodeChar: {
        // WCHAR RtlUpcaseUnicodeChar(WCHAR SourceChar)
        uint32_t ch = stack_arg(exec, 0);
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        exec.ctx.gp[GP_EAX] = ch;
        stdcall_cleanup(exec, 1);
        return true;
    }

    // ---- Crypto stubs ----
    case ORD_XcVerifyPKCS1Signature:
        // BOOLEAN XcVerifyPKCS1Signature(PUCHAR Hash, PUCHAR PubKey, PUCHAR Sig)
        exec.ctx.gp[GP_EAX] = 1; // TRUE -- pretend signature is valid
        stdcall_cleanup(exec, 3);
        return true;
    case ORD_XcModExp:
        // ULONG XcModExp(PULONG Result, PULONG Base, PULONG Exp, PULONG Mod, ULONG Len)
        // Stub: set result to 0
        {
            uint32_t result_ptr = stack_arg(exec, 0);
            uint32_t len = stack_arg(exec, 4);
            if (result_ptr + len <= GUEST_RAM_SIZE && len > 0)
                memset(exec.ram + result_ptr, 0, len);
        }
        exec.ctx.gp[GP_EAX] = 0;
        stdcall_cleanup(exec, 5);
        return true;

    case ORD_XcDESKeyParity: {
        // VOID XcDESKeyParity(PUCHAR Key, ULONG KeyLen)
        // Sets odd parity on each byte of a DES key
        uint32_t key_ptr = stack_arg(exec, 0);
        uint32_t key_len = stack_arg(exec, 1);
        if (key_ptr + key_len <= GUEST_RAM_SIZE) {
            for (uint32_t i = 0; i < key_len; ++i) {
                uint8_t b = exec.ram[key_ptr + i] & 0xFE;
                // Count bits, set parity bit for odd parity
                uint8_t p = b;
                p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
                exec.ram[key_ptr + i] = b | (~p & 1);
            }
        }
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_XcKeyTable:
        // VOID XcKeyTable(ULONG Encrypt, PUCHAR KeyTable, PUCHAR Key)
        // DES key schedule generation -- stub: zero the 128-byte key table
        if (stack_arg(exec, 1) + 128 <= GUEST_RAM_SIZE)
            memset(exec.ram + stack_arg(exec, 1), 0, 128);
        stdcall_cleanup(exec, 3);
        return true;

    case ORD_XcBlockCrypt:
        // VOID XcBlockCrypt(ULONG Encrypt, PUCHAR Output, PUCHAR Input, PUCHAR KeyTable, ULONG Operation)
        // DES single block -- stub: copy input to output
        {
            uint32_t out_ptr = stack_arg(exec, 1);
            uint32_t in_ptr  = stack_arg(exec, 2);
            if (out_ptr + 8 <= GUEST_RAM_SIZE && in_ptr + 8 <= GUEST_RAM_SIZE)
                memcpy(exec.ram + out_ptr, exec.ram + in_ptr, 8);
        }
        stdcall_cleanup(exec, 5);
        return true;

    case ORD_XcBlockCryptCBC:
        // VOID XcBlockCryptCBC(ULONG Encrypt, ULONG InputLen, PUCHAR Output, PUCHAR Input,
        //                      PUCHAR KeyTable, ULONG Operation, PUCHAR Feedback)
        // DES CBC -- stub: copy input to output
        {
            uint32_t in_len  = stack_arg(exec, 1);
            uint32_t out_ptr = stack_arg(exec, 2);
            uint32_t in_ptr  = stack_arg(exec, 3);
            if (out_ptr + in_len <= GUEST_RAM_SIZE && in_ptr + in_len <= GUEST_RAM_SIZE && in_len > 0)
                memcpy(exec.ram + out_ptr, exec.ram + in_ptr, in_len);
        }
        stdcall_cleanup(exec, 7);
        return true;

    case ORD_PhyGetLinkState:
        // BOOLEAN PhyGetLinkState(BOOLEAN param) -- return FALSE (cable not connected)
        exec.ctx.gp[GP_EAX] = 0; // FALSE
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_PhyInitialize:
        // BOOLEAN PhyInitialize(BOOLEAN param) -- Ethernet PHY; return FALSE (no network)
        exec.ctx.gp[GP_EAX] = 0; // FALSE
        stdcall_cleanup(exec, 1);
        return true;

    case ORD_XcSHAInit: {
        // VOID XcSHAInit(PUCHAR SHAContext)  -- 116-byte context
        uint32_t ctx_ptr = stack_arg(exec, 0);
        if (ctx_ptr + 116 <= GUEST_RAM_SIZE) {
            sha1_detail::SHA1_CTX ctx;
            sha1_detail::sha1_init(&ctx);
            memset(exec.ram + ctx_ptr, 0, 116);
            memcpy(exec.ram + ctx_ptr, ctx.state, 20);
        }
        stdcall_cleanup(exec, 1);
        return true;
    }

    case ORD_XcSHAUpdate: {
        // VOID XcSHAUpdate(PUCHAR SHAContext, PUCHAR Data, ULONG DataLen)
        uint32_t ctx_ptr  = stack_arg(exec, 0);
        uint32_t data_ptr = stack_arg(exec, 1);
        uint32_t data_len = stack_arg(exec, 2);
        if (ctx_ptr + 116 <= GUEST_RAM_SIZE) {
            sha1_detail::SHA1_CTX ctx;
            memcpy(ctx.state, exec.ram + ctx_ptr, 20);
            memcpy(&ctx.count, exec.ram + ctx_ptr + 20, 8);
            memcpy(ctx.buffer, exec.ram + ctx_ptr + 28, 64);
            if (data_ptr + data_len <= GUEST_RAM_SIZE && data_len > 0)
                sha1_detail::sha1_update(&ctx, exec.ram + data_ptr, data_len);
            memcpy(exec.ram + ctx_ptr, ctx.state, 20);
            memcpy(exec.ram + ctx_ptr + 20, &ctx.count, 8);
            memcpy(exec.ram + ctx_ptr + 28, ctx.buffer, 64);
        }
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_XcSHAFinal: {
        // VOID XcSHAFinal(PUCHAR SHAContext, PUCHAR Digest)
        uint32_t ctx_ptr    = stack_arg(exec, 0);
        uint32_t digest_ptr = stack_arg(exec, 1);
        if (ctx_ptr + 116 <= GUEST_RAM_SIZE && digest_ptr + 20 <= GUEST_RAM_SIZE) {
            sha1_detail::SHA1_CTX ctx;
            memcpy(ctx.state, exec.ram + ctx_ptr, 20);
            memcpy(&ctx.count, exec.ram + ctx_ptr + 20, 8);
            memcpy(ctx.buffer, exec.ram + ctx_ptr + 28, 64);
            uint8_t digest[20];
            sha1_detail::sha1_final(&ctx, digest);
            memcpy(exec.ram + digest_ptr, digest, 20);
        }
        stdcall_cleanup(exec, 2);
        return true;
    }

    case ORD_XcRC4Key: {
        // VOID XcRC4Key(PUCHAR RC4Context, ULONG KeyLen, PUCHAR Key)
        uint32_t ctx_ptr = stack_arg(exec, 0);
        uint32_t key_len = stack_arg(exec, 1);
        uint32_t key_ptr = stack_arg(exec, 2);
        if (ctx_ptr + 258 <= GUEST_RAM_SIZE) {
            uint8_t* S = exec.ram + ctx_ptr;
            for (int i = 0; i < 256; ++i) S[i] = (uint8_t)i;
            uint8_t j = 0;
            if (key_ptr + key_len <= GUEST_RAM_SIZE && key_len > 0) {
                for (int i = 0; i < 256; ++i) {
                    j = j + S[i] + exec.ram[key_ptr + (i % key_len)];
                    uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
                }
            }
            exec.ram[ctx_ptr + 256] = 0;
            exec.ram[ctx_ptr + 257] = 0;
        }
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_XcRC4Crypt: {
        // VOID XcRC4Crypt(PUCHAR RC4Context, ULONG DataLen, PUCHAR Data)
        uint32_t ctx_ptr  = stack_arg(exec, 0);
        uint32_t data_len = stack_arg(exec, 1);
        uint32_t data_ptr = stack_arg(exec, 2);
        if (ctx_ptr + 258 <= GUEST_RAM_SIZE && data_ptr + data_len <= GUEST_RAM_SIZE) {
            uint8_t* S = exec.ram + ctx_ptr;
            uint8_t si = exec.ram[ctx_ptr + 256];
            uint8_t sj = exec.ram[ctx_ptr + 257];
            for (uint32_t k = 0; k < data_len; ++k) {
                si++; sj += S[si];
                uint8_t tmp = S[si]; S[si] = S[sj]; S[sj] = tmp;
                exec.ram[data_ptr + k] ^= S[(uint8_t)(S[si] + S[sj])];
            }
            exec.ram[ctx_ptr + 256] = si;
            exec.ram[ctx_ptr + 257] = sj;
        }
        stdcall_cleanup(exec, 3);
        return true;
    }

    case ORD_XcHMAC: {
        // VOID XcHMAC(PBYTE Key, ULONG KeyLen, PBYTE Data, ULONG DataLen,
        //             PBYTE Data2, ULONG Data2Len, PBYTE Digest)
        uint32_t key_ptr   = stack_arg(exec, 0);
        uint32_t key_len   = stack_arg(exec, 1);
        uint32_t d1_ptr    = stack_arg(exec, 2);
        uint32_t d1_len    = stack_arg(exec, 3);
        uint32_t d2_ptr    = stack_arg(exec, 4);
        uint32_t d2_len    = stack_arg(exec, 5);
        uint32_t dig_ptr   = stack_arg(exec, 6);

        const uint8_t* key  = (key_ptr + key_len <= GUEST_RAM_SIZE) ? exec.ram + key_ptr : nullptr;
        const uint8_t* dat1 = (d1_ptr + d1_len <= GUEST_RAM_SIZE && d1_len) ? exec.ram + d1_ptr : nullptr;
        const uint8_t* dat2 = (d2_ptr + d2_len <= GUEST_RAM_SIZE && d2_len) ? exec.ram + d2_ptr : nullptr;

        if (key && dig_ptr + 20 <= GUEST_RAM_SIZE) {
            uint8_t digest[20];
            sha1_detail::hmac_sha1(key, key_len,
                                   dat1, dat1 ? d1_len : 0,
                                   dat2, dat2 ? d2_len : 0,
                                   digest);
            memcpy(exec.ram + dig_ptr, digest, 20);
        } else if (dig_ptr + 20 <= GUEST_RAM_SIZE) {
            memset(exec.ram + dig_ptr, 0, 20);
        }
        stdcall_cleanup(exec, 7);
        return true;
    }

        default:
        // Unhandled: log and halt (cannot continue safely because
        // we don't know the argument count for stdcall cleanup).
        fprintf(stderr, "[hle] FATAL: unhandled kernel ordinal %u at EIP=0x%08X\n",
                ordinal, exec.ctx.eip);
        exec.ctx.gp[GP_EAX] = 0xC0000002u; // STATUS_NOT_IMPLEMENTED
        exec.ctx.halted = true;
        exec.ctx.stop_reason = STOP_INVALID_OPCODE;
        return true;
    }
}

} // namespace xbe
