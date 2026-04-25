#pragma once
#include <cstdint>
#include <cstddef>
#include "mmio.hpp"

// Stop-reason codes: written by JIT into ctx->stop_reason, read by run loop.
enum StopReason : uint32_t {
    STOP_NONE       = 0,  // normal trace exit
    STOP_PRIVILEGED = 1,  // trace hit a privileged instruction (HLT/IN/OUT/...)
    STOP_PAGE_FAULT = 2,  // VA→PA translation failed; CR2 set, run loop delivers #PF
};

// Host reserved registers during trace execution:
//   R12 = fastmem_base   (uint8_t* to guest PA 0)
//   R13 = GuestContext*
//   R14 = EA scratch     (clobbered freely; not callee-saved within a trace)
//   R15 = unused          (callee-saved, push/pop for ABI compliance only)
//
// Guest general-purpose registers are live in host EAX–EDI throughout the trace.
// Guest ESP is NOT mapped to host RSP; it lives in ctx->gp[GP_ESP] and is
// accessed inline by emitted PUSH/POP/CALL/RET sequences.

// IMPORTANT: field offsets are referenced from asm trampolines.
// Do not reorder without updating executor.cpp and emitter.hpp.
struct alignas(16) GuestContext {
    // GP registers [offsets 0..28], in x86 ModRM encoding order.
    uint32_t  gp[8];         // [0]  EAX ECX EDX EBX ESP EBP ESI EDI

    uint32_t  eip;           // [32]
    uint32_t  eflags;        // [36]
    uint32_t  next_eip;      // [40]  written by every trace exit stub
    uint32_t  stop_reason;   // [44]  StopReason — set by JIT for privileged stops

    uint64_t  fastmem_base;  // [48]  host pointer to guest PA 0
    uint32_t  _reserved_56;   // [56]  (was ram_size — now unused, kept for layout)
    uint32_t  _pad1;         // [60]

    MmioMap*  mmio;          // [64]  MMIO dispatch table

    // Emulated privileged state (never live in host registers)
    uint32_t  cr0, cr2, cr3, cr4;
    uint32_t  gdtr_base; uint16_t gdtr_limit; uint16_t _p1;
    uint32_t  idtr_base; uint16_t idtr_limit; uint16_t _p2;
    bool      virtual_if;
    bool      halted;
    uint8_t   _pad_vif[2];   // alignment padding

    // Segment base addresses (only FS/GS are typically non-zero in protected mode)
    uint32_t  fs_base;        // [108] FS segment base (kernel KPCR)
    uint32_t  gs_base;        // [112] GS segment base

    // Reserved — was SMC page-version pointer (now handled by VEH).
    uint32_t  _pad_pv;        // [116] alignment padding
    uint64_t  _reserved_120;  // [120] (was page_versions — now unused, kept for layout)

    // FPU/SSE state in FXSAVE format (512 bytes, 16-byte aligned).
    // Saved/restored by the dispatch_trace trampoline on trace entry/exit.
    alignas(16) uint8_t guest_fpu[512];  // [128] guest x87/MMX/SSE state
    alignas(16) uint8_t host_fpu[512];   // [640] saved host state

    // SYSENTER/SYSEXIT MSRs (IA32_SYSENTER_CS/EIP/ESP)
    // Placed after FPU state to avoid shifting hardcoded offsets.
    uint32_t  sysenter_cs  = 0;   // MSR 0x174
    uint32_t  sysenter_esp = 0;   // MSR 0x175
    uint32_t  sysenter_eip = 0;   // MSR 0x176

    // MTRR MSRs — Memory Type Range Registers
    uint64_t  mtrr_physbase[8] = {};  // MSR 0x200, 0x202, ..., 0x20E
    uint64_t  mtrr_physmask[8] = {};  // MSR 0x201, 0x203, ..., 0x20F
    uint64_t  mtrr_fix64k      = 0;   // MSR 0x250
    uint64_t  mtrr_fix16k[2]   = {};  // MSR 0x258, 0x259
    uint64_t  mtrr_fix4k[8]    = {};  // MSR 0x268–0x26F
    uint64_t  mtrr_def_type    = 0x00000006; // MSR 0xFE (default = write-back)

    // Task register and LDT selector (set by LTR / LLDT, read by STR / SLDT)
    uint16_t  tr_sel   = 0;  // Task Register selector
    uint16_t  ldtr_sel = 0;  // LDT Register selector
};

static_assert(offsetof(GuestContext, gp)           ==  0);
static_assert(offsetof(GuestContext, eip)          == 32);
static_assert(offsetof(GuestContext, next_eip)     == 40);
static_assert(offsetof(GuestContext, fastmem_base) == 48);
static_assert(offsetof(GuestContext, _reserved_56)  == 56);
static_assert(offsetof(GuestContext, mmio)         == 64);
static_assert(offsetof(GuestContext, fs_base)      == 108);
static_assert(offsetof(GuestContext, gs_base)      == 112);
static_assert(offsetof(GuestContext, _reserved_120) == 120);
static_assert(offsetof(GuestContext, guest_fpu)    == 128);
static_assert(offsetof(GuestContext, host_fpu)     == 640);

// GP register indices (match x86 ModRM/SIB encoding order)
enum : int {
    GP_EAX = 0, GP_ECX = 1, GP_EDX = 2, GP_EBX = 3,
    GP_ESP = 4, GP_EBP = 5, GP_ESI = 6, GP_EDI = 7
};

// Byte offset of gp[i] within GuestContext.
inline constexpr uint8_t gp_offset(int i) {
    return static_cast<uint8_t>(offsetof(GuestContext, gp) + i * sizeof(uint32_t));
}

// Named offsets for scalar GuestContext fields accessed from JIT code / asm.
inline constexpr int CTX_GP            = (int)offsetof(GuestContext, gp);
inline constexpr int CTX_EIP           = (int)offsetof(GuestContext, eip);
inline constexpr int CTX_EFLAGS        = (int)offsetof(GuestContext, eflags);
inline constexpr int CTX_NEXT_EIP      = (int)offsetof(GuestContext, next_eip);
inline constexpr int CTX_STOP_REASON   = (int)offsetof(GuestContext, stop_reason);
inline constexpr int CTX_FASTMEM_BASE  = (int)offsetof(GuestContext, fastmem_base);
inline constexpr int CTX_MMIO          = (int)offsetof(GuestContext, mmio);
inline constexpr int CTX_FS_BASE       = (int)offsetof(GuestContext, fs_base);
inline constexpr int CTX_GS_BASE       = (int)offsetof(GuestContext, gs_base);
inline constexpr int CTX_GUEST_FPU     = (int)offsetof(GuestContext, guest_fpu);
inline constexpr int CTX_HOST_FPU      = (int)offsetof(GuestContext, host_fpu);

// Stringification helpers for GCC naked inline asm (cannot use C++ constexpr).
// Values are verified against offsetof by static_assert above.
#define CTX_XSTR_(x) #x
#define CTX_STR_(x)  CTX_XSTR_(x)

#define CTX_ASM_GP_EAX       0
#define CTX_ASM_GP_ECX       4
#define CTX_ASM_GP_EDX       8
#define CTX_ASM_GP_EBX      12
#define CTX_ASM_GP_ESP      16
#define CTX_ASM_GP_EBP      20
#define CTX_ASM_GP_ESI      24
#define CTX_ASM_GP_EDI      28
#define CTX_ASM_EFLAGS      36
#define CTX_ASM_FASTMEM_BASE 48
#define CTX_ASM_GUEST_FPU  128
#define CTX_ASM_HOST_FPU   640

static_assert(CTX_ASM_GP_EAX  == (int)offsetof(GuestContext, gp[0]));
static_assert(CTX_ASM_GP_ECX  == (int)offsetof(GuestContext, gp[1]));
static_assert(CTX_ASM_GP_EDX  == (int)offsetof(GuestContext, gp[2]));
static_assert(CTX_ASM_GP_EBX  == (int)offsetof(GuestContext, gp[3]));
static_assert(CTX_ASM_GP_ESP  == (int)offsetof(GuestContext, gp[4]));
static_assert(CTX_ASM_GP_EBP  == (int)offsetof(GuestContext, gp[5]));
static_assert(CTX_ASM_GP_ESI  == (int)offsetof(GuestContext, gp[6]));
static_assert(CTX_ASM_GP_EDI  == (int)offsetof(GuestContext, gp[7]));
static_assert(CTX_ASM_EFLAGS       == (int)offsetof(GuestContext, eflags));
static_assert(CTX_ASM_FASTMEM_BASE == (int)offsetof(GuestContext, fastmem_base));
static_assert(CTX_ASM_GUEST_FPU    == (int)offsetof(GuestContext, guest_fpu));
static_assert(CTX_ASM_HOST_FPU     == (int)offsetof(GuestContext, host_fpu));
