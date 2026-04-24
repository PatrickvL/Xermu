#pragma once
#include "emitter.hpp"
#include "context.hpp"
#include <Zydis/Zydis.h>
#include <cstdint>
#include <initializer_list>

// ---------------------------------------------------------------------------
// Target CPU feature level.
// Xbox original = Pentium III (Coppermine): SSE1 + MMX.  No SSE2.
// Set XBOX_TARGET_SSE to 2 to enable SSE2+ dispatch for other targets.
// ---------------------------------------------------------------------------
#ifndef XBOX_TARGET_SSE
#define XBOX_TARGET_SSE 1
#endif

// ---------------------------------------------------------------------------
// Forward declarations for handler context
// ---------------------------------------------------------------------------

struct Emitter;
struct GuestContext;

// ---------------------------------------------------------------------------
// Instruction class — properties and emit handler for a mnemonic group
// ---------------------------------------------------------------------------

// Handler callback: emits host code for one guest instruction.
// Returns true on success, false if operand form is unsupported.
using EmitHandler = bool (*)(Emitter&                         e,
                              const ZydisDecodedInstruction&   insn,
                              const ZydisDecodedOperand*       ops,
                              const uint8_t*                   raw,
                              GuestContext*                    ctx,
                              bool                             save_flags);

enum InsnClassFlags : uint8_t {
    ICF_NONE         = 0,
    ICF_HAS_DISPATCH = 1 << 0,  // has fastmem CMP — may clobber EFLAGS
    ICF_CLEAN_COPY   = 1 << 1,  // verbatim-copy (or re-encode), no mem access
    ICF_TERMINATOR   = 1 << 2,  // trace terminator (branch/call/ret)
    ICF_PRIVILEGED   = 1 << 3,  // privileged instruction — trap/UD2
};

struct InsnClass {
    EmitHandler    handler;
    InsnClassFlags flags;
};

// ---------------------------------------------------------------------------
// Emit handlers — declared here, defined in trace_builder.cpp
// ---------------------------------------------------------------------------

bool emit_handler_clean(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, const uint8_t* raw,
                        GuestContext* ctx, bool save_flags);

bool emit_handler_lea(Emitter& e, const ZydisDecodedInstruction& insn,
                      const ZydisDecodedOperand* ops, const uint8_t* raw,
                      GuestContext* ctx, bool save_flags);

bool emit_handler_mov_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* ctx, bool save_flags);

bool emit_handler_alu_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* ctx, bool save_flags);

bool emit_handler_push(Emitter& e, const ZydisDecodedInstruction& insn,
                       const ZydisDecodedOperand* ops, const uint8_t* raw,
                       GuestContext* ctx, bool save_flags);

bool emit_handler_pop(Emitter& e, const ZydisDecodedInstruction& insn,
                      const ZydisDecodedOperand* ops, const uint8_t* raw,
                      GuestContext* ctx, bool save_flags);

bool emit_handler_push_imm(Emitter& e, const ZydisDecodedInstruction& insn,
                           const ZydisDecodedOperand* ops, const uint8_t* raw,
                           GuestContext* ctx, bool save_flags);

bool emit_handler_leave(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, const uint8_t* raw,
                        GuestContext* ctx, bool save_flags);

bool emit_handler_movzx_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                            const ZydisDecodedOperand* ops, const uint8_t* raw,
                            GuestContext* ctx, bool save_flags);

bool emit_handler_movsx_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                            const ZydisDecodedOperand* ops, const uint8_t* raw,
                            GuestContext* ctx, bool save_flags);

bool emit_handler_test_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                           const ZydisDecodedOperand* ops, const uint8_t* raw,
                           GuestContext* ctx, bool save_flags);

bool emit_handler_fpu_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* ctx, bool save_flags);

bool emit_handler_pushfd(Emitter& e, const ZydisDecodedInstruction& insn,
                         const ZydisDecodedOperand* ops, const uint8_t* raw,
                         GuestContext* ctx, bool save_flags);

bool emit_handler_popfd(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, const uint8_t* raw,
                        GuestContext* ctx, bool save_flags);

// ---------------------------------------------------------------------------
// Dispatch table — Level 1 maps mnemonic → compact index, Level 2 has class
// ---------------------------------------------------------------------------

// Compact class IDs (0 = no entry → fall through to generic path)
enum InsnClassId : uint8_t {
    IC_NONE = 0,
    IC_CLEAN,       // INC/DEC r32 re-encode, others verbatim
    IC_LEA,         // LEA — no memory access, re-encode
    IC_MOV_MEM,     // MOV r,[m] / MOV [m],r / MOV [m],imm
    IC_ALU_MEM,     // ADD/SUB/AND/OR/XOR/CMP/ADC/SBB r,[m] / [m],r / [m],imm
    IC_TEST_MEM,    // TEST r,[m] / [m],r / [m],imm
    IC_PUSH,        // PUSH r32
    IC_POP,         // POP r32
    IC_PUSH_IMM,    // PUSH imm8/imm32
    IC_LEAVE,       // LEAVE
    IC_MOVZX_MEM,   // MOVZX r32, [m]
    IC_MOVSX_MEM,   // MOVSX r32, [m]
    IC_FPU_MEM,     // x87 instructions with memory operand forms
    IC_SSE_MEM,     // SSE/MMX instructions with memory operand forms
    IC_PUSHFD,      // PUSHFD — push EFLAGS onto guest stack
    IC_POPFD,       // POPFD — pop EFLAGS from guest stack
    IC_TERMINATOR,  // JMP/CALL/RET/Jcc/LOOP — trace exit
    IC_PRIVILEGED,  // HLT/LGDT/CLI/... — trap
    IC_MAX
};

// Level 2: one entry per InsnClassId
inline constexpr InsnClass INSN_CLASS_TABLE[] = {
    /* IC_NONE       */ { nullptr,                ICF_NONE },
    /* IC_CLEAN      */ { emit_handler_clean,     ICF_CLEAN_COPY },
    /* IC_LEA        */ { emit_handler_lea,       ICF_CLEAN_COPY },
    /* IC_MOV_MEM    */ { emit_handler_mov_mem,   ICF_HAS_DISPATCH },
    /* IC_ALU_MEM    */ { emit_handler_alu_mem,   ICF_HAS_DISPATCH },
    /* IC_TEST_MEM   */ { emit_handler_test_mem,  ICF_HAS_DISPATCH },
    /* IC_PUSH       */ { emit_handler_push,      ICF_HAS_DISPATCH },
    /* IC_POP        */ { emit_handler_pop,       ICF_HAS_DISPATCH },
    /* IC_PUSH_IMM   */ { emit_handler_push_imm,  ICF_HAS_DISPATCH },
    /* IC_LEAVE      */ { emit_handler_leave,     ICF_HAS_DISPATCH },
    /* IC_MOVZX_MEM  */ { emit_handler_movzx_mem, ICF_HAS_DISPATCH },
    /* IC_MOVSX_MEM  */ { emit_handler_movsx_mem, ICF_HAS_DISPATCH },
    /* IC_FPU_MEM    */ { emit_handler_fpu_mem,   ICF_HAS_DISPATCH },
    /* IC_SSE_MEM    */ { emit_handler_fpu_mem,   ICF_HAS_DISPATCH }, // same logic
    /* IC_PUSHFD     */ { emit_handler_pushfd,    ICF_HAS_DISPATCH },
    /* IC_POPFD      */ { emit_handler_popfd,     ICF_HAS_DISPATCH },
    /* IC_TERMINATOR */ { nullptr,                ICF_TERMINATOR },
    /* IC_PRIVILEGED */ { nullptr,                ICF_PRIVILEGED },
};
static_assert(sizeof(INSN_CLASS_TABLE)/sizeof(INSN_CLASS_TABLE[0]) == IC_MAX);

// Level 1: mnemonic → InsnClassId
// Initialized at program startup by init_mnemonic_table().
// Size is ~1.8 KB (one byte per mnemonic enum value).
inline uint8_t MNEMONIC_CLASS[ZYDIS_MNEMONIC_MAX_VALUE + 1] = {};

inline void init_mnemonic_table() {
    // ---- Terminators ----
    for (auto m : {
        ZYDIS_MNEMONIC_JMP,  ZYDIS_MNEMONIC_CALL, ZYDIS_MNEMONIC_RET,
        ZYDIS_MNEMONIC_IRETD,
        ZYDIS_MNEMONIC_JB,   ZYDIS_MNEMONIC_JBE,  ZYDIS_MNEMONIC_JCXZ,
        ZYDIS_MNEMONIC_JECXZ,ZYDIS_MNEMONIC_JL,   ZYDIS_MNEMONIC_JLE,
        ZYDIS_MNEMONIC_JNB,  ZYDIS_MNEMONIC_JNBE, ZYDIS_MNEMONIC_JNL,
        ZYDIS_MNEMONIC_JNLE, ZYDIS_MNEMONIC_JNO,  ZYDIS_MNEMONIC_JNP,
        ZYDIS_MNEMONIC_JNS,  ZYDIS_MNEMONIC_JNZ,  ZYDIS_MNEMONIC_JO,
        ZYDIS_MNEMONIC_JP,   ZYDIS_MNEMONIC_JS,   ZYDIS_MNEMONIC_JZ,
        ZYDIS_MNEMONIC_LOOP, ZYDIS_MNEMONIC_LOOPE,ZYDIS_MNEMONIC_LOOPNE,
    }) MNEMONIC_CLASS[m] = IC_TERMINATOR;

    // ---- Privileged ----
    for (auto m : {
        ZYDIS_MNEMONIC_HLT,    ZYDIS_MNEMONIC_LGDT,   ZYDIS_MNEMONIC_LIDT,
        ZYDIS_MNEMONIC_LLDT,   ZYDIS_MNEMONIC_LTR,    ZYDIS_MNEMONIC_CLTS,
        ZYDIS_MNEMONIC_INVD,   ZYDIS_MNEMONIC_WBINVD, ZYDIS_MNEMONIC_INVLPG,
        ZYDIS_MNEMONIC_RDMSR,  ZYDIS_MNEMONIC_WRMSR,  ZYDIS_MNEMONIC_RDTSC,
        ZYDIS_MNEMONIC_RDPMC,  ZYDIS_MNEMONIC_IN,     ZYDIS_MNEMONIC_OUT,
        ZYDIS_MNEMONIC_INSB,   ZYDIS_MNEMONIC_INSD,   ZYDIS_MNEMONIC_INSW,
        ZYDIS_MNEMONIC_OUTSB,  ZYDIS_MNEMONIC_OUTSD,  ZYDIS_MNEMONIC_OUTSW,
        ZYDIS_MNEMONIC_CLI,    ZYDIS_MNEMONIC_STI,    ZYDIS_MNEMONIC_LMSW,
    }) MNEMONIC_CLASS[m] = IC_PRIVILEGED;

    // ---- LEA — no memory access ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_LEA]    = IC_LEA;

    // ---- MOV (reg/mem forms — handler inspects operands) ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_MOV]    = IC_MOV_MEM;

    // ---- ALU with memory operand forms ----
    for (auto m : {
        ZYDIS_MNEMONIC_ADD, ZYDIS_MNEMONIC_SUB, ZYDIS_MNEMONIC_AND,
        ZYDIS_MNEMONIC_OR,  ZYDIS_MNEMONIC_XOR, ZYDIS_MNEMONIC_CMP,
        ZYDIS_MNEMONIC_ADC, ZYDIS_MNEMONIC_SBB,
        ZYDIS_MNEMONIC_INC, ZYDIS_MNEMONIC_DEC,
        ZYDIS_MNEMONIC_NEG, ZYDIS_MNEMONIC_NOT,
        ZYDIS_MNEMONIC_SHL, ZYDIS_MNEMONIC_SHR, ZYDIS_MNEMONIC_SAR,
        ZYDIS_MNEMONIC_ROL, ZYDIS_MNEMONIC_ROR,
        ZYDIS_MNEMONIC_RCL, ZYDIS_MNEMONIC_RCR,
    }) MNEMONIC_CLASS[m] = IC_ALU_MEM;

    // ---- TEST — read-only, separate handler ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_TEST]   = IC_TEST_MEM;

    // ---- Stack-implicit ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_PUSH]   = IC_PUSH;
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_POP]    = IC_POP;
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_LEAVE]  = IC_LEAVE;

    // ---- Zero/sign extending loads ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_MOVZX]  = IC_MOVZX_MEM;
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_MOVSX]  = IC_MOVSX_MEM;

    // ---- PUSHFD / POPFD (guest stack, not host RSP) ----
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_PUSHFD]  = IC_PUSHFD;
    MNEMONIC_CLASS[ZYDIS_MNEMONIC_POPFD]   = IC_POPFD;

    // ---- x87 FPU (mnemonics that CAN have memory operand forms) ----
    // Register-only forms are handled by the fallback in the handler
    // (clean copy). Memory forms use the generic rewriter.
    for (auto m : {
        ZYDIS_MNEMONIC_FLD,    ZYDIS_MNEMONIC_FST,    ZYDIS_MNEMONIC_FSTP,
        ZYDIS_MNEMONIC_FILD,   ZYDIS_MNEMONIC_FIST,   ZYDIS_MNEMONIC_FISTP,
        ZYDIS_MNEMONIC_FBLD,   ZYDIS_MNEMONIC_FBSTP,
        ZYDIS_MNEMONIC_FADD,   ZYDIS_MNEMONIC_FSUB,   ZYDIS_MNEMONIC_FSUBR,
        ZYDIS_MNEMONIC_FMUL,   ZYDIS_MNEMONIC_FDIV,   ZYDIS_MNEMONIC_FDIVR,
        ZYDIS_MNEMONIC_FCOM,   ZYDIS_MNEMONIC_FCOMP,
        ZYDIS_MNEMONIC_FIADD,  ZYDIS_MNEMONIC_FISUB,  ZYDIS_MNEMONIC_FISUBR,
        ZYDIS_MNEMONIC_FIMUL,  ZYDIS_MNEMONIC_FIDIV,  ZYDIS_MNEMONIC_FIDIVR,
        ZYDIS_MNEMONIC_FICOM,  ZYDIS_MNEMONIC_FICOMP,
        ZYDIS_MNEMONIC_FLDCW,  ZYDIS_MNEMONIC_FNSTCW,
        ZYDIS_MNEMONIC_FLDENV, ZYDIS_MNEMONIC_FNSTENV,
        ZYDIS_MNEMONIC_FNSTSW,
    }) MNEMONIC_CLASS[m] = IC_FPU_MEM;

    // ---- SSE1 (Pentium III / Xbox CPU) ----
    // Register-only forms are verbatim-copied; memory forms use the
    // generic rewriter.  Only single-precision (PS/SS) variants exist.
    for (auto m : {
        // Data movement
        ZYDIS_MNEMONIC_MOVAPS,  ZYDIS_MNEMONIC_MOVUPS,
        ZYDIS_MNEMONIC_MOVSS,
        ZYDIS_MNEMONIC_MOVLPS,  ZYDIS_MNEMONIC_MOVHPS,
        ZYDIS_MNEMONIC_MOVNTPS,
        // Arithmetic (packed single / scalar single)
        ZYDIS_MNEMONIC_ADDPS,   ZYDIS_MNEMONIC_ADDSS,
        ZYDIS_MNEMONIC_SUBPS,   ZYDIS_MNEMONIC_SUBSS,
        ZYDIS_MNEMONIC_MULPS,   ZYDIS_MNEMONIC_MULSS,
        ZYDIS_MNEMONIC_DIVPS,   ZYDIS_MNEMONIC_DIVSS,
        ZYDIS_MNEMONIC_SQRTPS,  ZYDIS_MNEMONIC_SQRTSS,
        ZYDIS_MNEMONIC_RCPPS,   ZYDIS_MNEMONIC_RCPSS,
        ZYDIS_MNEMONIC_RSQRTPS, ZYDIS_MNEMONIC_RSQRTSS,
        ZYDIS_MNEMONIC_MAXPS,   ZYDIS_MNEMONIC_MAXSS,
        ZYDIS_MNEMONIC_MINPS,   ZYDIS_MNEMONIC_MINSS,
        // Logical
        ZYDIS_MNEMONIC_ANDPS,   ZYDIS_MNEMONIC_ANDNPS,
        ZYDIS_MNEMONIC_ORPS,    ZYDIS_MNEMONIC_XORPS,
        // Compare
        ZYDIS_MNEMONIC_CMPPS,   ZYDIS_MNEMONIC_CMPSS,
        ZYDIS_MNEMONIC_COMISS,  ZYDIS_MNEMONIC_UCOMISS,
        // Conversion (integer ↔ single)
        ZYDIS_MNEMONIC_CVTSI2SS,  ZYDIS_MNEMONIC_CVTSS2SI,
        ZYDIS_MNEMONIC_CVTTSS2SI,
        ZYDIS_MNEMONIC_CVTPI2PS,  ZYDIS_MNEMONIC_CVTPS2PI,
        ZYDIS_MNEMONIC_CVTTPS2PI,
        // Shuffle / unpack
        ZYDIS_MNEMONIC_SHUFPS,
        ZYDIS_MNEMONIC_UNPCKLPS, ZYDIS_MNEMONIC_UNPCKHPS,
        // MXCSR
        ZYDIS_MNEMONIC_LDMXCSR, ZYDIS_MNEMONIC_STMXCSR,
    }) MNEMONIC_CLASS[m] = IC_SSE_MEM;

    // ---- MMX / SSE1 integer SIMD (Pentium MMX/III) ----
    // These mnemonics are shared with SSE2 XMM forms (same Zydis enum);
    // the handler works for both register widths.
    for (auto m : {
        ZYDIS_MNEMONIC_MOVD,    ZYDIS_MNEMONIC_MOVQ,
        // Arithmetic
        ZYDIS_MNEMONIC_PADDB,   ZYDIS_MNEMONIC_PADDW,
        ZYDIS_MNEMONIC_PADDD,
        ZYDIS_MNEMONIC_PSUBB,   ZYDIS_MNEMONIC_PSUBW,
        ZYDIS_MNEMONIC_PSUBD,
        ZYDIS_MNEMONIC_PMULLW,  ZYDIS_MNEMONIC_PMULHW,
        // Logical
        ZYDIS_MNEMONIC_PAND,    ZYDIS_MNEMONIC_PANDN,
        ZYDIS_MNEMONIC_POR,     ZYDIS_MNEMONIC_PXOR,
        // Compare
        ZYDIS_MNEMONIC_PCMPEQB, ZYDIS_MNEMONIC_PCMPEQW,
        ZYDIS_MNEMONIC_PCMPEQD, ZYDIS_MNEMONIC_PCMPGTB,
        ZYDIS_MNEMONIC_PCMPGTW, ZYDIS_MNEMONIC_PCMPGTD,
        // Shift
        ZYDIS_MNEMONIC_PSLLW,   ZYDIS_MNEMONIC_PSLLD,
        ZYDIS_MNEMONIC_PSLLQ,   ZYDIS_MNEMONIC_PSRLW,
        ZYDIS_MNEMONIC_PSRLD,   ZYDIS_MNEMONIC_PSRLQ,
        ZYDIS_MNEMONIC_PSRAW,   ZYDIS_MNEMONIC_PSRAD,
        // Pack / unpack
        ZYDIS_MNEMONIC_PACKSSWB, ZYDIS_MNEMONIC_PACKSSDW,
        ZYDIS_MNEMONIC_PACKUSWB,
        ZYDIS_MNEMONIC_PUNPCKLBW, ZYDIS_MNEMONIC_PUNPCKLWD,
        ZYDIS_MNEMONIC_PUNPCKLDQ,
        ZYDIS_MNEMONIC_PUNPCKHBW, ZYDIS_MNEMONIC_PUNPCKHWD,
        ZYDIS_MNEMONIC_PUNPCKHDQ,
        // SSE1 extensions to MMX (Pentium III)
        ZYDIS_MNEMONIC_PMULHUW,
        ZYDIS_MNEMONIC_PSHUFW,
        ZYDIS_MNEMONIC_PMINUB,  ZYDIS_MNEMONIC_PMAXUB,
        ZYDIS_MNEMONIC_PMINSW,  ZYDIS_MNEMONIC_PMAXSW,
        ZYDIS_MNEMONIC_PAVGB,   ZYDIS_MNEMONIC_PAVGW,
        ZYDIS_MNEMONIC_PSADBW,
        ZYDIS_MNEMONIC_MASKMOVQ,
        ZYDIS_MNEMONIC_MOVNTQ,
    }) MNEMONIC_CLASS[m] = IC_SSE_MEM;

#if XBOX_TARGET_SSE >= 2
    // ---- SSE2 (Pentium 4) — NOT present on Xbox Pentium III ----
    for (auto m : {
        // Double-precision float data movement
        ZYDIS_MNEMONIC_MOVAPD,  ZYDIS_MNEMONIC_MOVUPD,
        ZYDIS_MNEMONIC_MOVSD,
        ZYDIS_MNEMONIC_MOVLPD,  ZYDIS_MNEMONIC_MOVHPD,
        ZYDIS_MNEMONIC_MOVNTPD, ZYDIS_MNEMONIC_MOVNTDQ,
        ZYDIS_MNEMONIC_MOVDQA,  ZYDIS_MNEMONIC_MOVDQU,
        // Double-precision arithmetic
        ZYDIS_MNEMONIC_ADDPD,   ZYDIS_MNEMONIC_ADDSD,
        ZYDIS_MNEMONIC_SUBPD,   ZYDIS_MNEMONIC_SUBSD,
        ZYDIS_MNEMONIC_MULPD,   ZYDIS_MNEMONIC_MULSD,
        ZYDIS_MNEMONIC_DIVPD,   ZYDIS_MNEMONIC_DIVSD,
        ZYDIS_MNEMONIC_SQRTPD,  ZYDIS_MNEMONIC_SQRTSD,
        ZYDIS_MNEMONIC_MAXPD,   ZYDIS_MNEMONIC_MAXSD,
        ZYDIS_MNEMONIC_MINPD,   ZYDIS_MNEMONIC_MINSD,
        // Double-precision logical
        ZYDIS_MNEMONIC_ANDPD,   ZYDIS_MNEMONIC_ANDNPD,
        ZYDIS_MNEMONIC_ORPD,    ZYDIS_MNEMONIC_XORPD,
        // Double-precision compare
        ZYDIS_MNEMONIC_CMPPD,   ZYDIS_MNEMONIC_CMPSD,
        ZYDIS_MNEMONIC_COMISD,  ZYDIS_MNEMONIC_UCOMISD,
        // Conversion (double / packed-integer)
        ZYDIS_MNEMONIC_CVTPS2PD,  ZYDIS_MNEMONIC_CVTPD2PS,
        ZYDIS_MNEMONIC_CVTSS2SD,  ZYDIS_MNEMONIC_CVTSD2SS,
        ZYDIS_MNEMONIC_CVTPS2DQ,  ZYDIS_MNEMONIC_CVTDQ2PS,
        ZYDIS_MNEMONIC_CVTTPS2DQ, ZYDIS_MNEMONIC_CVTTPD2DQ,
        ZYDIS_MNEMONIC_CVTPD2DQ,  ZYDIS_MNEMONIC_CVTDQ2PD,
        ZYDIS_MNEMONIC_CVTSI2SD,  ZYDIS_MNEMONIC_CVTSD2SI,
        ZYDIS_MNEMONIC_CVTTSD2SI,
        // Double-precision shuffle / unpack
        ZYDIS_MNEMONIC_SHUFPD,
        ZYDIS_MNEMONIC_UNPCKLPD,  ZYDIS_MNEMONIC_UNPCKHPD,
        // SSE2-only integer SIMD (XMM register forms)
        ZYDIS_MNEMONIC_PADDQ,   ZYDIS_MNEMONIC_PSUBQ,
        ZYDIS_MNEMONIC_PMULUDQ,
        ZYDIS_MNEMONIC_PUNPCKLQDQ, ZYDIS_MNEMONIC_PUNPCKHQDQ,
        ZYDIS_MNEMONIC_PSHUFD,  ZYDIS_MNEMONIC_PSHUFHW,
        ZYDIS_MNEMONIC_PSHUFLW,
        ZYDIS_MNEMONIC_MASKMOVDQU,
    }) MNEMONIC_CLASS[m] = IC_SSE_MEM;
#endif // XBOX_TARGET_SSE >= 2
}

// O(1) lookup: returns the InsnClassFlags for a mnemonic.
inline InsnClassFlags lookup_flags(ZydisMnemonic m) {
    uint8_t id = MNEMONIC_CLASS[m];
    return INSN_CLASS_TABLE[id].flags;
}

// O(1) lookup: returns the InsnClass for a decoded instruction.
// Returns nullptr if the mnemonic has no special handler (generic path).
inline const InsnClass* lookup_insn_class(ZydisMnemonic m) {
    uint8_t id = MNEMONIC_CLASS[m];
    return id ? &INSN_CLASS_TABLE[id] : nullptr;
}
