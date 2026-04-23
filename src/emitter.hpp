#pragma once
#include "context.hpp"
#include <cstdint>
#include <cstring>
#include <cassert>
#include <Zydis/Zydis.h>

// ---------------------------------------------------------------------------
// Raw byte emitter
// ---------------------------------------------------------------------------

struct Emitter {
    uint8_t* buf;
    size_t   pos;
    size_t   cap;

    Emitter(uint8_t* b, size_t c) : buf(b), pos(0), cap(c) {}

    void emit8 (uint8_t  v) { assert(pos < cap); buf[pos++] = v; }
    void emit32(uint32_t v) {
        assert(pos + 4 <= cap);
        memcpy(buf + pos, &v, 4); pos += 4;
    }
    void emit64(uint64_t v) {
        assert(pos + 8 <= cap);
        memcpy(buf + pos, &v, 8); pos += 8;
    }
    void copy(const uint8_t* src, size_t n) {
        assert(pos + n <= cap);
        memcpy(buf + pos, src, n); pos += n;
    }

    uint8_t* cur() const { return buf + pos; }

    // Reserve 4 bytes for a rel32 patch site and return its address.
    uint8_t* reserve_rel32() {
        uint8_t* p = cur(); emit32(0); return p;
    }

    // Backpatch a rel32 to jump to `target`.
    // The rel32 field is at `site`; the instruction ends 4 bytes after it.
    static void patch_rel32(uint8_t* site, const uint8_t* target) {
        int32_t delta = (int32_t)(target - (site + 4));
        memcpy(site, &delta, 4);
    }
};

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------

// Map Zydis 32-bit register enum to [0..7] x86 encoding index.
// Returns false for registers we don't handle (segment, MMX, XMM, etc.).
inline bool reg32_enc(ZydisRegister r, uint8_t& out) {
    switch (r) {
        case ZYDIS_REGISTER_EAX: out = 0; return true;
        case ZYDIS_REGISTER_ECX: out = 1; return true;
        case ZYDIS_REGISTER_EDX: out = 2; return true;
        case ZYDIS_REGISTER_EBX: out = 3; return true;
        case ZYDIS_REGISTER_ESP: out = 4; return true;
        case ZYDIS_REGISTER_EBP: out = 5; return true;
        case ZYDIS_REGISTER_ESI: out = 6; return true;
        case ZYDIS_REGISTER_EDI: out = 7; return true;
        default: return false;
    }
}

// Map Zydis register to GP array index (same as encoding for EAX-EDI).
inline int reg32_gp(ZydisRegister r) {
    uint8_t enc;
    return reg32_enc(r, enc) ? enc : -1;
}

// ---------------------------------------------------------------------------
// Sequence: save/load all guest GP registers to/from GuestContext
//
// MOV [R13 + gp_offset(i)], reg_i
// Encoding: REX=0x41 (REX.B for R13 base), 0x89, ModRM, disp8
//   ModRM = mod=01 | (reg_enc<<3) | 5   (rm=5 = R13&7, mod=01 = disp8)
// ---------------------------------------------------------------------------

inline void emit_save_gp(Emitter& e, uint8_t reg_enc, uint8_t offset) {
    e.emit8(0x41); e.emit8(0x89);
    e.emit8(uint8_t(0x40u | (reg_enc << 3) | 5u));
    e.emit8(offset);
}

inline void emit_load_gp(Emitter& e, uint8_t reg_enc, uint8_t offset) {
    e.emit8(0x41); e.emit8(0x8B);
    e.emit8(uint8_t(0x40u | (reg_enc << 3) | 5u));
    e.emit8(offset);
}

// Save all guest GP regs except ESP (index 4).
// ESP lives exclusively in ctx->gp[GP_ESP] and is managed by inline
// emit_sub/add_ctx_esp sequences.  Host RSP is the host call stack.
inline void emit_save_all_gp(Emitter& e) {
    for (int i = 0; i < 8; ++i) {
        if (i == GP_ESP) continue;
        emit_save_gp(e, uint8_t(i), gp_offset(i));
    }
}

// Reload all guest GP regs except ESP.
inline void emit_load_all_gp(Emitter& e) {
    for (int i = 0; i < 8; ++i) {
        if (i == GP_ESP) continue;
        emit_load_gp(e, uint8_t(i), gp_offset(i));
    }
}

// ---------------------------------------------------------------------------
// Write ctx->next_eip = imm32
//   MOV R14D, imm32   (REX.B=1: 0x41 0xBE imm32)
//   MOV [R13+40], R14D (REX.R=1,REX.B=1: 0x45 0x89 0x75 0x28)
// ---------------------------------------------------------------------------

inline void emit_write_next_eip_imm(Emitter& e, uint32_t eip) {
    e.emit8(0x41); e.emit8(0xBE); e.emit32(eip);          // MOV R14D, imm32
    e.emit8(0x45); e.emit8(0x89); e.emit8(0x75); e.emit8(40); // MOV [R13+40], R14D
}

// Write ctx->next_eip from a guest register already in ctx (via its gp index).
inline void emit_write_next_eip_gpreg(Emitter& e, uint8_t gp_idx) {
    // The value is already in ctx->gp[gp_idx] after emit_save_all_gp.
    // Copy ctx->gp[gp_idx] → ctx->next_eip:
    //   MOV R14D, [R13 + gp_offset(gp_idx)]
    emit_load_gp(e, 6 /*R14&7=6, with REX.R→encoding differs*/,
                 gp_offset(gp_idx));
    // Wait: emit_load_gp uses REX.B(R13 base) + reg field for dest.
    // For R14D as dest, reg=6, REX.R=1 → REX=0x45 not 0x41.
    // Fixup: emit manually:
    // Undo the emit_load_gp we just did (3 bytes for the call above already
    // emitted - this is wrong). Instead, handle inline below.
    // NOTE: the call above emitted the wrong encoding. We overwrite by
    // backing up pos (valid since we know exact size = 4 bytes).
    e.pos -= 4; // undo incorrect emit_load_gp
    // Correct: MOV R14D, [R13 + offset]
    // REX = 0x45 (REX.R=1 for R14 as dest, REX.B=1 for R13 as base)
    e.emit8(0x45); e.emit8(0x8B);
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u)); // mod=01, reg=6(R14&7), rm=5(R13&7)
    e.emit8(gp_offset(gp_idx));
    // Now store R14D → ctx->next_eip (offset 40)
    e.emit8(0x45); e.emit8(0x89); e.emit8(0x75); e.emit8(40);
}

// ---------------------------------------------------------------------------
// Trace epilog: save all GP regs, write next_eip, RET
// ---------------------------------------------------------------------------

inline void emit_epilog_static(Emitter& e, uint32_t next_eip) {
    emit_save_all_gp(e);
    emit_write_next_eip_imm(e, next_eip);
    e.emit8(0xC3); // RET
}

inline void emit_epilog_dynamic(Emitter& e, uint8_t eip_gp_idx) {
    emit_save_all_gp(e);
    emit_write_next_eip_gpreg(e, eip_gp_idx);
    e.emit8(0xC3); // RET
}

// ---------------------------------------------------------------------------
// Conditional trace exit:  save regs, Jcc taken/not-taken, then epilog each.
// `jcc_short` is the 1-byte short Jcc opcode (e.g. 0x75 for JNZ).
// ---------------------------------------------------------------------------

inline void emit_epilog_conditional(Emitter& e,
                                     uint8_t  jcc_short,
                                     uint32_t taken_eip,
                                     uint32_t fallthrough_eip) {
    // Save regs first (MOV does not affect EFLAGS — condition still valid).
    emit_save_all_gp(e);

    // Near Jcc: 0F (jcc_short + 0x10) rel32
    e.emit8(0x0F);
    e.emit8(uint8_t(jcc_short + 0x10u));
    uint8_t* taken_site = e.reserve_rel32();

    // Fallthrough path
    emit_write_next_eip_imm(e, fallthrough_eip);
    e.emit8(0xC3);

    // Taken path
    Emitter::patch_rel32(taken_site, e.cur());
    emit_write_next_eip_imm(e, taken_eip);
    e.emit8(0xC3);
}

// ---------------------------------------------------------------------------
// EA synthesis: LEA R14D, [guest memory operand]
//
// R14 = register 14: reg field = R14&7 = 6, requires REX.R=1 → REX base 0x44
// All guest base/index are EAX–EDI (indices 0-7), no REX.B/REX.X needed.
// ---------------------------------------------------------------------------

inline bool emit_ea_to_r14(Emitter& e, const ZydisDecodedOperand& op) {
    assert(op.type == ZYDIS_OPERAND_TYPE_MEMORY);
    const auto& m = op.mem;

    // We do not handle segment overrides (FS:, GS:) here.
    if (m.segment != ZYDIS_REGISTER_NONE &&
        m.segment != ZYDIS_REGISTER_DS   &&
        m.segment != ZYDIS_REGISTER_SS)
        return false;

    bool has_base  = (m.base  != ZYDIS_REGISTER_NONE &&
                      m.base  != ZYDIS_REGISTER_EIP);
    bool has_index = (m.index != ZYDIS_REGISTER_NONE);
    bool has_disp  = m.disp.has_displacement;
    auto disp      = (int32_t)m.disp.value;

    constexpr uint8_t R14_REG = 6; // R14 & 7
    constexpr uint8_t REX_R14 = 0x44; // REX.R = 1

    // --- Displacement only (no base, no index) ---
    if (!has_base && !has_index) {
        // MOV R14D, imm32
        e.emit8(0x41); e.emit8(uint8_t(0xB8u + R14_REG)); e.emit32((uint32_t)disp);
        return true;
    }

    // --- Base only (no index) ---
    if (has_base && !has_index) {
        uint8_t base_enc;
        if (!reg32_enc(m.base, base_enc)) return false;

        if (!has_disp || disp == 0) {
            // EBP special: mod=00 rm=101 means disp32-only, force mod=01 disp8=0
            if (base_enc == 5) {
                e.emit8(REX_R14); e.emit8(0x8D);
                e.emit8(uint8_t(0x40u | (R14_REG << 3) | 5u)); e.emit8(0);
                return true;
            }
            // ESP special: rm=100 requires SIB
            if (base_enc == 4) {
                e.emit8(REX_R14); e.emit8(0x8D);
                e.emit8(uint8_t((R14_REG << 3) | 4u)); // mod=00 rm=4
                e.emit8(0x24); // SIB: scale=0 index=4(none) base=4(ESP)
                return true;
            }
            // Simple: MOV R14D, base_reg  (mod=11)
            e.emit8(REX_R14); e.emit8(0x8B);
            e.emit8(uint8_t(0xC0u | (R14_REG << 3) | base_enc));
            return true;
        }

        bool d8   = (disp >= -128 && disp <= 127);
        uint8_t mod = d8 ? 1 : 2;

        if (base_enc == 4) { // ESP + disp → SIB
            e.emit8(REX_R14); e.emit8(0x8D);
            e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | 4u));
            e.emit8(0x24);
        } else {
            e.emit8(REX_R14); e.emit8(0x8D);
            e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | base_enc));
        }
        if (d8) e.emit8((uint8_t)(int8_t)disp);
        else    e.emit32((uint32_t)disp);
        return true;
    }

    // --- Has index (and possibly base) ---
    uint8_t index_enc;
    if (!reg32_enc(m.index, index_enc)) return false;
    if (index_enc == 4) return false; // ESP cannot be index

    uint8_t scale_enc;
    switch (m.scale) {
        case 2:  scale_enc = 1; break;
        case 4:  scale_enc = 2; break;
        case 8:  scale_enc = 3; break;
        default: scale_enc = 0; break;
    }

    if (!has_base) {
        // [index*scale + disp32]: SIB base=101 (no base), mod=00
        uint8_t sib = uint8_t((scale_enc << 6) | (index_enc << 3) | 5u);
        e.emit8(REX_R14); e.emit8(0x8D);
        e.emit8(uint8_t((R14_REG << 3) | 4u)); // mod=00 rm=4
        e.emit8(sib);
        e.emit32((uint32_t)(has_disp ? disp : 0));
        return true;
    }

    uint8_t base_enc;
    if (!reg32_enc(m.base, base_enc)) return false;

    uint8_t sib = uint8_t((scale_enc << 6) | (index_enc << 3) | base_enc);

    uint8_t mod;
    if (!has_disp || disp == 0)
        mod = (base_enc == 5) ? 1 : 0; // EBP needs mod=01 even with 0
    else
        mod = (disp >= -128 && disp <= 127) ? 1 : 2;

    e.emit8(REX_R14); e.emit8(0x8D);
    e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | 4u)); // rm=4 → SIB
    e.emit8(sib);
    if      (mod == 1) e.emit8((uint8_t)(int8_t)(has_disp ? disp : 0));
    else if (mod == 2) e.emit32((uint32_t)disp);
    return true;
}

// ---------------------------------------------------------------------------
// Fastmem check: CMP R14, R15  (compare EA against ram_size)
//   REX=0x45 (REX.R=1 R14, REX.B=1 R15), 0x3B, ModRM=mod=11 reg=6 rm=7 = 0xF7
// ---------------------------------------------------------------------------

inline void emit_cmp_r14_r15(Emitter& e) {
    e.emit8(0x45); e.emit8(0x3B); e.emit8(0xF7);
}

// JAE rel32 (jump if unsigned EA >= ram_size → slow/MMIO path)
inline uint8_t* emit_jae_fwd(Emitter& e) {
    e.emit8(0x0F); e.emit8(0x83); return e.reserve_rel32();
}

// JMP rel32
inline uint8_t* emit_jmp_fwd(Emitter& e) {
    e.emit8(0xE9); return e.reserve_rel32();
}

// ---------------------------------------------------------------------------
// EFLAGS save/restore — wraps memory dispatch sequences so that the inline
// CMP R14, R15 (and SUB/ADD for ESP management) does not clobber guest flags.
//
// PUSHFQ shifts RSP by 8, breaking 16-byte alignment.  We fix that with
// LEA RSP, [RSP ± 8] which does NOT affect EFLAGS.
// ---------------------------------------------------------------------------

inline void emit_save_flags(Emitter& e) {
    e.emit8(0x9C);                                             // PUSHFQ
    // LEA RSP, [RSP - 8]  (re-align to 16 bytes; LEA is flag-neutral)
    e.emit8(0x48); e.emit8(0x8D); e.emit8(0x64); e.emit8(0x24); e.emit8(0xF8);
}

inline void emit_restore_flags(Emitter& e) {
    // LEA RSP, [RSP + 8]  (undo alignment padding)
    e.emit8(0x48); e.emit8(0x8D); e.emit8(0x64); e.emit8(0x24); e.emit8(0x08);
    e.emit8(0x9D);                                             // POPFQ
}

// ---------------------------------------------------------------------------
// Fastmem access: OP reg, [R12+R14]  or  OP [R12+R14], reg
//
// R12 (base, REX.B=1) + R14 (index, REX.X=1): REX = 0x43
// SIB: scale=0, index=6(R14&7), base=4(R12&7) → 0x34
// ModRM: mod=00, reg=guest_enc, rm=4(SIB) → guest_enc<<3 | 4
// ---------------------------------------------------------------------------

inline void emit_fastmem_op(Emitter& e, uint8_t guest_enc,
                              unsigned size_bits, bool is_load) {
    if (size_bits == 16) e.emit8(0x66);
    uint8_t rex = 0x43; // REX.B(R12) | REX.X(R14)
    uint8_t opc = is_load ? 0x8B : 0x89;
    if (size_bits == 8) opc = is_load ? 0x8A : 0x88;

    e.emit8(rex);
    e.emit8(opc);
    e.emit8(uint8_t((guest_enc << 3) | 4u)); // mod=00, reg=guest_enc, rm=4
    e.emit8(0x34);                            // SIB: scale=0 idx=R14 base=R12
}

// MOVZX r32, [R12+R14]  for 8/16-bit loads with zero-extension
inline void emit_fastmem_movzx(Emitter& e, uint8_t guest_enc, unsigned src_bits) {
    e.emit8(0x43); e.emit8(0x0F);
    e.emit8(src_bits == 8 ? 0xB6u : 0xB7u);
    e.emit8(uint8_t((guest_enc << 3) | 4u));
    e.emit8(0x34);
}

// ---------------------------------------------------------------------------
// Absolute call via R14 (clobbers R14 — safe: EA already committed or saved)
//   MOVABS R14, imm64 + CALL R14
//   REX.W=1, REX.B=1 (R14): 0x49 0xBE imm64
//   CALL R14: REX.B=1 → 0x41 0xFF 0xD6
// On Windows, allocates/frees the required 32-byte shadow space.
// ---------------------------------------------------------------------------

inline void emit_call_abs(Emitter& e, const void* target) {
#ifdef _WIN32
    // SUB RSP, 0x20
    e.emit8(0x48); e.emit8(0x83); e.emit8(0xEC); e.emit8(0x20);
#endif
    e.emit8(0x49); e.emit8(0xBE); e.emit64((uint64_t)(uintptr_t)target);
    e.emit8(0x41); e.emit8(0xFF); e.emit8(0xD6);
#ifdef _WIN32
    // ADD RSP, 0x20
    e.emit8(0x48); e.emit8(0x83); e.emit8(0xC4); e.emit8(0x20);
#endif
}

// ---------------------------------------------------------------------------
// Sub/Add DWORD PTR [R13 + offset], imm8
//   REX.B=1: 0x41, 0x83, ModRM, disp8, imm8
//   ModRM for SUB: mod=01 reg=5(SUB) rm=5(R13&7) = 01_101_101 = 0x6D
//   ModRM for ADD: mod=01 reg=0(ADD) rm=5(R13&7) = 01_000_101 = 0x45
// ---------------------------------------------------------------------------

inline void emit_sub_ctx_esp(Emitter& e, uint8_t amount) {
    e.emit8(0x41); e.emit8(0x83);
    e.emit8(0x6D); // SUB [R13+?]
    e.emit8(gp_offset(GP_ESP));
    e.emit8(amount);
}

inline void emit_add_ctx_esp(Emitter& e, uint8_t amount) {
    e.emit8(0x41); e.emit8(0x83);
    e.emit8(0x45); // ADD [R13+?]
    e.emit8(gp_offset(GP_ESP));
    e.emit8(amount);
}

// Load ctx->esp into R14D:
//   MOV R14D, [R13 + gp_offset(GP_ESP)]
//   REX=0x45 (REX.R R14, REX.B R13), 0x8B, mod=01 reg=6(R14&7) rm=5(R13&7), disp8
inline void emit_load_esp_to_r14(Emitter& e) {
    e.emit8(0x45); e.emit8(0x8B);
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));
    e.emit8(gp_offset(GP_ESP));
}

// ---------------------------------------------------------------------------
// MMIO slow-path call helpers.
// Called after emit_save_all_gp so all guest-mapped host registers are free.
// Each helper emits the correct register for the current platform's ABI.
//
// System V AMD64 ABI:  args in RDI, RSI, RDX, RCX
// Windows x64 ABI:     args in RCX, RDX, R8,  R9
//
// Both ABIs: R13 = ctx (callee-saved, not clobbered by the call).
// ---------------------------------------------------------------------------

// arg0 = GuestContext* (from R13)
inline void emit_ccall_arg0_ctx(Emitter& e) {
#ifdef _WIN32
    // MOV RCX, R13
    e.emit8(0x4C); e.emit8(0x89); e.emit8(0xE9);
#else
    // MOV RDI, R13
    e.emit8(0x49); e.emit8(0x8B); e.emit8(0xFD);
#endif
}

// arg1 = PA (from R14D)
inline void emit_ccall_arg1_pa(Emitter& e) {
#ifdef _WIN32
    // MOV EDX, R14D
    e.emit8(0x44); e.emit8(0x89); e.emit8(0xF2);
#else
    // MOV ESI, R14D (zero-extends to RSI)
    e.emit8(0x41); e.emit8(0x8B); e.emit8(0xF6);
#endif
}

// arg2 = imm32
inline void emit_ccall_arg2_imm(Emitter& e, uint32_t v) {
#ifdef _WIN32
    // MOV R8D, imm32
    e.emit8(0x41); e.emit8(0xB8); e.emit32(v);
#else
    // MOV EDX, imm32
    e.emit8(0xBA); e.emit32(v);
#endif
}

// arg3 = imm32
inline void emit_ccall_arg3_imm(Emitter& e, uint32_t v) {
#ifdef _WIN32
    // MOV R9D, imm32
    e.emit8(0x41); e.emit8(0xB9); e.emit32(v);
#else
    // MOV ECX, imm32
    e.emit8(0xB9); e.emit32(v);
#endif
}

// ---------------------------------------------------------------------------
// emit_clean_insn — copy a "clean" (no memory, not privileged) instruction,
// re-encoding the cases that differ between 32-bit and 64-bit mode.
//
// Problem: short-form INC/DEC (opcodes 0x40-0x4F) are REX prefixes in x86-64.
//   DEC ECX = 0x49 in 32-bit → REX.WB in 64-bit (wrong!)
//
// Fix: detect via Zydis mnemonic, emit the ModRM two-byte forms instead.
//   INC r32: FF /0  → REX(none), 0xFF, ModRM(mod=11 reg=0 rm=enc)
//   DEC r32: FF /1  → REX(none), 0xFF, ModRM(mod=11 reg=1 rm=enc)
//
// All other clean instructions (no REX-space conflict) are verbatim-copied.
// ---------------------------------------------------------------------------

inline bool emit_clean_insn(Emitter& e,
                              const ZydisDecodedInstruction& insn,
                              const ZydisDecodedOperand* ops,
                              const uint8_t* raw_bytes) {
    // INC r32 (short form: 0x40..0x47 in 32-bit)
    if (insn.mnemonic == ZYDIS_MNEMONIC_INC &&
        insn.operand_count >= 1 &&
        ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        uint8_t enc = 0;
        if (reg32_enc(ops[0].reg.value, enc)) {
            // FF /0  ModRM = 11_000_enc
            e.emit8(0xFF);
            e.emit8(uint8_t(0xC0u | enc));
            return true;
        }
    }
    // DEC r32 (short form: 0x48..0x4F in 32-bit)
    if (insn.mnemonic == ZYDIS_MNEMONIC_DEC &&
        insn.operand_count >= 1 &&
        ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        uint8_t enc = 0;
        if (reg32_enc(ops[0].reg.value, enc)) {
            // FF /1  ModRM = 11_001_enc
            e.emit8(0xFF);
            e.emit8(uint8_t(0xC8u | enc));
            return true;
        }
    }
    // Default: verbatim copy
    e.copy(raw_bytes, insn.length);
    return true;
}
