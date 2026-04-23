#pragma once
#include "emitter.hpp"
#include "context.hpp"
#include <Zydis/Zydis.h>
#include <cstdint>
#include <initializer_list>

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
