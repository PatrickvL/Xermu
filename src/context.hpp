#pragma once
#include <cstdint>
#include <cstddef>
#include "mmio.hpp"

// Stop-reason codes: written by JIT into ctx->stop_reason, read by run loop.
enum StopReason : uint32_t {
    STOP_NONE       = 0,  // normal trace exit
    STOP_PRIVILEGED = 1,  // trace hit a privileged instruction (HLT/IN/OUT/...)
};

// Host reserved registers during trace execution:
//   R12 = fastmem_base   (uint8_t* to guest PA 0)
//   R13 = GuestContext*
//   R14 = EA scratch     (clobbered freely; not callee-saved within a trace)
//   R15 = ram_size       (32-bit comparison threshold for fastmem check)
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
    uint32_t  ram_size;      // [56]  bytes of directly-accessible RAM
    uint32_t  _pad1;         // [60]

    MmioMap*  mmio;          // [64]  MMIO dispatch table

    // Emulated privileged state (never live in host registers)
    uint32_t  cr0, cr2, cr3, cr4;
    uint32_t  gdtr_base; uint16_t gdtr_limit; uint16_t _p1;
    uint32_t  idtr_base; uint16_t idtr_limit; uint16_t _p2;
    bool      virtual_if;

    // FPU/SSE state in FXSAVE format (512 bytes, 16-byte aligned).
    // Saved/restored by the dispatch_trace trampoline on trace entry/exit.
    alignas(16) uint8_t guest_fpu[512];  // [112] guest x87/MMX/SSE state
    alignas(16) uint8_t host_fpu[512];   // [624] saved host state
};

static_assert(offsetof(GuestContext, gp)           ==  0);
static_assert(offsetof(GuestContext, eip)          == 32);
static_assert(offsetof(GuestContext, next_eip)     == 40);
static_assert(offsetof(GuestContext, fastmem_base) == 48);
static_assert(offsetof(GuestContext, ram_size)     == 56);
static_assert(offsetof(GuestContext, mmio)         == 64);
static_assert(offsetof(GuestContext, guest_fpu)    == 112);
static_assert(offsetof(GuestContext, host_fpu)     == 624);

// Offsets used by asm trampolines (MASM + inline asm)
inline constexpr int CTX_GUEST_FPU = 112;
inline constexpr int CTX_HOST_FPU  = 624;

// GP register indices (match x86 ModRM/SIB encoding order)
enum : int {
    GP_EAX = 0, GP_ECX = 1, GP_EDX = 2, GP_EBX = 3,
    GP_ESP = 4, GP_EBP = 5, GP_ESI = 6, GP_EDI = 7
};

// Byte offset of gp[i] within GuestContext
inline constexpr uint8_t gp_offset(int i) {
    return static_cast<uint8_t>(i * 4);
}
