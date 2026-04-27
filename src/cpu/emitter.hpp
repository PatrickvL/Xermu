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
    bool     paging   = false; // true when CR0.PG is set at build time
    uint32_t fault_eip = 0;    // guest EIP for #PF exit on translate fault

    // Pending link slots collected during trace emission.
    // Transferred to the Trace after building.
    struct PendingLink {
        uint8_t* patch_site;
        uint32_t target_eip;
    };
    static constexpr int MAX_PENDING_LINKS = 4;
    PendingLink pending_links[MAX_PENDING_LINKS] = {};
    int num_pending_links = 0;

    // Memory-op site table: accumulated during emit, transferred to Trace.
    // host_offset is provided explicitly by the caller (offset of the patchable region).
    struct MemSite {
        uint32_t host_offset;
        uint32_t guest_eip;
        uint8_t  patch_len;
    };
    static constexpr int MAX_MEM_SITES = 256;
    MemSite mem_sites[MAX_MEM_SITES] = {};
    int num_mem_sites = 0;

    // Record a mem_site.  For patchable sites, host_off is the start of the
    // patchable region and plen is the total bytes (fastmem insn + pad NOPs).
    // Non-patchable sites (ALU/FPU/SSE) pass plen=0.
    void add_mem_site(uint32_t host_off, uint32_t geip, uint8_t plen = 0) {
        if (num_mem_sites < MAX_MEM_SITES)
            mem_sites[num_mem_sites++] = { host_off, geip, plen };
    }

    void add_pending_link(uint8_t* site, uint32_t target) {
        if (num_pending_links < MAX_PENDING_LINKS)
            pending_links[num_pending_links++] = { site, target };
    }

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

// Pad the current emission point up to `min_len` bytes from `start_pos`
// with single-byte NOPs (0x90).  Used to ensure patchable fastmem regions
// are at least 5 bytes (enough for a CALL rel32).
inline void emit_pad_to(Emitter& e, size_t start_pos, size_t min_len) {
    while (e.pos - start_pos < min_len) e.emit8(0x90);
}

// ---------------------------------------------------------------------------
// x86-64 REX prefix constants (0x40 | W<<3 | R<<2 | X<<1 | B)
// ---------------------------------------------------------------------------
constexpr uint8_t REX_B   = 0x41;   // R13/R14/R15 in r/m or SIB base
constexpr uint8_t REX_XB  = 0x43;   // R13 base + extended SIB index (R12)
constexpr uint8_t REX_R   = 0x44;   // R8-R15 in ModRM.reg
constexpr uint8_t REX_RB  = 0x45;   // extended reg + extended r/m (R14/R13)
constexpr uint8_t REX_W   = 0x48;   // 64-bit operand size
constexpr uint8_t REX_WB  = 0x49;   // 64-bit + extended r/m
constexpr uint8_t REX_WR  = 0x4C;   // 64-bit + extended reg
constexpr uint8_t REX_WRB = 0x4D;   // 64-bit + extended reg + extended r/m

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

// Map any guest GP register (8/16/32-bit) to its 3-bit encoding.
// For 8-bit: only low registers (AL/CL/DL/BL) are supported because
// AH/CH/DH/BH encodings conflict with REX prefix (become SPL/BPL/SIL/DIL).
// For 16-bit and 32-bit: all registers including SP/ESP map correctly.
inline bool guest_reg_enc(ZydisRegister r, uint8_t& out) {
    switch (r) {
        // 32-bit
        case ZYDIS_REGISTER_EAX: out = 0; return true;
        case ZYDIS_REGISTER_ECX: out = 1; return true;
        case ZYDIS_REGISTER_EDX: out = 2; return true;
        case ZYDIS_REGISTER_EBX: out = 3; return true;
        case ZYDIS_REGISTER_ESP: out = 4; return true;
        case ZYDIS_REGISTER_EBP: out = 5; return true;
        case ZYDIS_REGISTER_ESI: out = 6; return true;
        case ZYDIS_REGISTER_EDI: out = 7; return true;
        // 16-bit
        case ZYDIS_REGISTER_AX:  out = 0; return true;
        case ZYDIS_REGISTER_CX:  out = 1; return true;
        case ZYDIS_REGISTER_DX:  out = 2; return true;
        case ZYDIS_REGISTER_BX:  out = 3; return true;
        case ZYDIS_REGISTER_SP:  out = 4; return true;
        case ZYDIS_REGISTER_BP:  out = 5; return true;
        case ZYDIS_REGISTER_SI:  out = 6; return true;
        case ZYDIS_REGISTER_DI:  out = 7; return true;
        // 8-bit low (safe with REX prefix)
        case ZYDIS_REGISTER_AL:  out = 0; return true;
        case ZYDIS_REGISTER_CL:  out = 1; return true;
        case ZYDIS_REGISTER_DL:  out = 2; return true;
        case ZYDIS_REGISTER_BL:  out = 3; return true;
        default: return false; // AH/CH/DH/BH not supported (REX conflict)
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
// Encoding: REX.B (R13 base), 0x89, ModRM, disp8
//   ModRM = mod=01 | (reg_enc<<3) | 5   (rm=5 = R13&7, mod=01 = disp8)
// ---------------------------------------------------------------------------

inline void emit_save_gp(Emitter& e, uint8_t reg_enc, uint8_t offset) {
    e.emit8(REX_B); e.emit8(0x89);
    e.emit8(uint8_t(0x40u | (reg_enc << 3) | 5u));
    e.emit8(offset);
}

inline void emit_load_gp(Emitter& e, uint8_t reg_enc, uint8_t offset) {
    e.emit8(REX_B); e.emit8(0x8B);
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
    e.emit8(REX_B); e.emit8(0xBE); e.emit32(eip);          // MOV R14D, imm32
    e.emit8(REX_RB); e.emit8(0x89); e.emit8(0x75); e.emit8(CTX_NEXT_EIP); // MOV [R13+next_eip], R14D
}

// ---------------------------------------------------------------------------
// Write ctx->stop_reason = imm32   (offset 44)
//   MOV DWORD [R13 + 44], imm32
//   REX.B=1: 0x41  opcode: 0xC7  ModRM: 0x45 (mod=01 /0 rm=5)  disp8: 0x2C
// ---------------------------------------------------------------------------

inline void emit_set_stop_reason(Emitter& e, uint32_t reason) {
    e.emit8(REX_B); e.emit8(0xC7); e.emit8(0x45); e.emit8(CTX_STOP_REASON);
    e.emit32(reason);
}

// ---------------------------------------------------------------------------
// Save host EFLAGS → ctx->eflags (offset 36).
// Used before privileged-instruction traps so that deliver_interrupt() can
// push the correct EFLAGS (including flags set by the last guest instruction).
//   PUSHFQ            ; capture RFLAGS
//   POP R14           ; R14 = RFLAGS (R14 is EA scratch, freely clobberable)
//   MOV [R13+36], R14D
// ---------------------------------------------------------------------------

inline void emit_save_eflags(Emitter& e) {
    e.emit8(0x9C);                              // PUSHFQ
    e.emit8(REX_B); e.emit8(0x5E);               // POP R14
    // MOV DWORD [R13+eflags], R14D — REX.RB (R14.R + R13.B)
    e.emit8(REX_RB); e.emit8(0x89);
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));   // mod=01 reg=6(R14) rm=5(R13)
    e.emit8(CTX_EFLAGS);                        // disp8 = offsetof(eflags)
}

// Write ctx->next_eip from a guest register already in ctx (via its gp index).
// Precondition: emit_save_all_gp has stored the live value into ctx->gp[gp_idx].
//
//   MOV R14D, [R13 + gp_offset(gp_idx)]   ; load saved guest reg
//   MOV [R13 + 40], R14D                   ; store to ctx->next_eip
//
// REX = 0x45 (REX.R for R14 dest/src, REX.B for R13 base).
// Note: emit_load_gp cannot be reused here because it hardcodes REX=0x41
// (REX.B only), which is wrong when the destination is R14 (needs REX.R too).
inline void emit_write_next_eip_gpreg(Emitter& e, uint8_t gp_idx) {
    // MOV R14D, DWORD [R13 + gp_offset(gp_idx)]
    e.emit8(REX_RB); e.emit8(0x8B);                              // REX.RB + MOV r,r/m
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));                  // mod=01 reg=6(R14) rm=5(R13)
    e.emit8(gp_offset(gp_idx));
    // MOV DWORD [R13 + next_eip], R14D
    e.emit8(REX_RB); e.emit8(0x89);                              // REX.RB + MOV r/m,r
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));                  // mod=01 reg=6(R14) rm=5(R13)
    e.emit8(CTX_NEXT_EIP);                                      // disp8 = offsetof(next_eip)
}

// ---------------------------------------------------------------------------
// Trace epilog: save all GP regs, write next_eip, RET.
//
// Block linking: static epilogs emit a JMP rel32 before the save/write/RET
// sequence.  Initially the JMP targets the fallback (next instruction).
// The caller records the JMP's rel32 address as a "link slot" — when the
// target trace is later compiled the JMP is patched to go directly there,
// bypassing the save/exit/trampoline/run-loop overhead entirely.
//
// Linkable epilog layout:
//   JMP rel32   → fallback (5 bytes, rel32 initially = 0)
//   fallback:     save_all_gp + write_next_eip + RET
//
// `patch_site` is set to the address of the rel32 in the JMP, or nullptr
// for dynamic/unlinkable exits.
// ---------------------------------------------------------------------------

inline uint8_t* emit_epilog_static(Emitter& e, uint32_t next_eip) {
    // JMP rel32 (link slot — initially points to the very next byte)
    e.emit8(0xE9);
    uint8_t* patch_site = e.cur();
    e.emit32(0); // rel32 = 0 → fallthrough to the instruction right after

    // Fallback exit path
    emit_save_all_gp(e);
    emit_write_next_eip_imm(e, next_eip);
    e.emit8(0xC3); // RET

    e.add_pending_link(patch_site, next_eip);
    return patch_site;
}

inline void emit_epilog_dynamic(Emitter& e, uint8_t eip_gp_idx) {
    // Dynamic targets cannot be linked.
    emit_save_all_gp(e);
    emit_write_next_eip_gpreg(e, eip_gp_idx);
    e.emit8(0xC3); // RET
}

// ---------------------------------------------------------------------------
// Conditional trace exit:  save regs, Jcc taken/not-taken, then epilog each.
// Both paths get linkable JMP rel32 slots.
//
// Returns {fallthrough_patch, taken_patch} via out parameters.
// ---------------------------------------------------------------------------

inline void emit_epilog_conditional(Emitter& e,
                                     uint8_t  jcc_short,
                                     uint32_t taken_eip,
                                     uint32_t fallthrough_eip,
                                     uint8_t** out_ft_patch = nullptr,
                                     uint8_t** out_tk_patch = nullptr) {
    // Save regs first (MOV does not affect EFLAGS — condition still valid).
    emit_save_all_gp(e);

    // Near Jcc: 0F (jcc_short + 0x10) rel32
    e.emit8(0x0F);
    e.emit8(uint8_t(jcc_short + 0x10u));
    uint8_t* taken_site = e.reserve_rel32();

    // Fallthrough path: linkable JMP + fallback exit
    e.emit8(0xE9);
    uint8_t* ft_patch = e.cur();
    e.emit32(0);
    emit_write_next_eip_imm(e, fallthrough_eip);
    e.emit8(0xC3);

    // Taken path: linkable JMP + fallback exit
    Emitter::patch_rel32(taken_site, e.cur());
    e.emit8(0xE9);
    uint8_t* tk_patch = e.cur();
    e.emit32(0);
    emit_write_next_eip_imm(e, taken_eip);
    e.emit8(0xC3);

    e.add_pending_link(ft_patch, fallthrough_eip);
    e.add_pending_link(tk_patch, taken_eip);

    if (out_ft_patch) *out_ft_patch = ft_patch;
    if (out_tk_patch) *out_tk_patch = tk_patch;
}

// ---------------------------------------------------------------------------
// EA synthesis: LEA R14D, [guest memory operand]
//
// R14 = register 14: reg field = R14&7 = 6, requires REX.R=1 → REX base 0x44
// All guest base/index are EAX–EDI (indices 0-7), no REX.B/REX.X needed.
// ---------------------------------------------------------------------------
// Forward declarations for helpers used in emit_ea_to_r14 (defined below).
inline void emit_load_esp_to_r14(Emitter& e);
inline void emit_store_r14_to_esp(Emitter& e);

inline bool emit_ea_to_r14(Emitter& e, const ZydisDecodedOperand& op) {
    assert(op.type == ZYDIS_OPERAND_TYPE_MEMORY);
    const auto& m = op.mem;

    // Determine segment base offset (FS/GS add ctx field; DS/SS/NONE = 0)
    int seg_ctx_offset = -1; // -1 = no segment base adjustment needed
    if (m.segment == ZYDIS_REGISTER_FS) {
        seg_ctx_offset = CTX_FS_BASE;
    } else if (m.segment == ZYDIS_REGISTER_GS) {
        seg_ctx_offset = CTX_GS_BASE;
    } else if (m.segment != ZYDIS_REGISTER_NONE &&
               m.segment != ZYDIS_REGISTER_DS   &&
               m.segment != ZYDIS_REGISTER_SS) {
        return false; // CS/ES not supported
    }

    bool has_base  = (m.base  != ZYDIS_REGISTER_NONE &&
                      m.base  != ZYDIS_REGISTER_EIP);
    bool has_index = (m.index != ZYDIS_REGISTER_NONE);
    bool has_disp  = m.disp.has_displacement;
    auto disp      = (int32_t)m.disp.value;

    constexpr uint8_t R14_REG = 6; // R14 & 7

    // --- Displacement only (no base, no index) ---
    if (!has_base && !has_index) {
        // MOV R14D, imm32
        e.emit8(REX_B); e.emit8(uint8_t(0xB8u + R14_REG)); e.emit32((uint32_t)disp);
        goto apply_segment;
    }

    // --- Base only (no index) ---
    if (has_base && !has_index) {
        uint8_t base_enc;
        if (!reg32_enc(m.base, base_enc)) return false;

        // ESP special: guest ESP is NOT in a host register; read from ctx.
        if (base_enc == 4) {
            emit_load_esp_to_r14(e); // R14D = ctx->gp[GP_ESP]
            if (has_disp && disp != 0) {
                // LEA R14D, [R14+disp]  (no flags clobbered)
                // R14 as both src and dst: REX.R=1 REX.B=1 → 0x45
                bool d8 = (disp >= -128 && disp <= 127);
                uint8_t mod = d8 ? 1u : 2u;
                e.emit8(REX_RB); e.emit8(0x8D);
                e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | R14_REG));
                if (d8) e.emit8((uint8_t)(int8_t)disp);
                else    e.emit32((uint32_t)disp);
            }
            goto apply_segment;
        }

        if (!has_disp || disp == 0) {
            // EBP special: mod=00 rm=101 means disp32-only, force mod=01 disp8=0
            if (base_enc == 5) {
                e.emit8(REX_R); e.emit8(0x8D);
                e.emit8(uint8_t(0x40u | (R14_REG << 3) | 5u)); e.emit8(0);
                goto apply_segment;
            }
            // Simple: MOV R14D, base_reg  (mod=11)
            e.emit8(REX_R); e.emit8(0x8B);
            e.emit8(uint8_t(0xC0u | (R14_REG << 3) | base_enc));
            goto apply_segment;
        }

        bool d8   = (disp >= -128 && disp <= 127);
        uint8_t mod = d8 ? 1 : 2;

        e.emit8(REX_R); e.emit8(0x8D);
        e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | base_enc));
        if (d8) e.emit8((uint8_t)(int8_t)disp);
        else    e.emit32((uint32_t)disp);
        goto apply_segment;
    }

    // --- Has index (and possibly base) ---
    {
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
            e.emit8(REX_R); e.emit8(0x8D);
            e.emit8(uint8_t((R14_REG << 3) | 4u)); // mod=00 rm=4
            e.emit8(sib);
            e.emit32((uint32_t)(has_disp ? disp : 0));
            goto apply_segment;
        }

        uint8_t base_enc;
        if (!reg32_enc(m.base, base_enc)) return false;

        // ESP as base: load guest ESP into R14 first, then use R14 as base.
        if (base_enc == 4) {
            emit_load_esp_to_r14(e); // R14D = ctx->gp[GP_ESP]
            // LEA R14D, [R14 + index*scale + disp]
            // REX: R14 dest (REX.R=1), R14 base (REX.B=1), index is 0-7 (REX.X=0)
            uint8_t sib_r14 = uint8_t((scale_enc << 6) | (index_enc << 3) | (R14_REG));
            uint8_t mod;
            if (!has_disp || disp == 0)
                mod = 0;
            else
                mod = (disp >= -128 && disp <= 127) ? 1u : 2u;
            // R14 as both reg and base: REX.R + REX.B
            e.emit8(REX_RB); e.emit8(0x8D);
            e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | 4u)); // rm=4 → SIB
            e.emit8(sib_r14);
            if      (mod == 1) e.emit8((uint8_t)(int8_t)(has_disp ? disp : 0));
            else if (mod == 2) e.emit32((uint32_t)disp);
            goto apply_segment;
        }

        uint8_t sib = uint8_t((scale_enc << 6) | (index_enc << 3) | base_enc);

        uint8_t mod;
        if (!has_disp || disp == 0)
            mod = (base_enc == 5) ? 1 : 0; // EBP needs mod=01 even with 0
        else
            mod = (disp >= -128 && disp <= 127) ? 1 : 2;

        e.emit8(REX_R); e.emit8(0x8D);
        e.emit8(uint8_t((mod << 6) | (R14_REG << 3) | 4u)); // rm=4 → SIB
        e.emit8(sib);
        if      (mod == 1) e.emit8((uint8_t)(int8_t)(has_disp ? disp : 0));
        else if (mod == 2) e.emit32((uint32_t)disp);
    }

apply_segment:
    // If FS/GS segment override, add the segment base from GuestContext.
    // ADD R14D, DWORD PTR [R13 + seg_ctx_offset]
    //   REX.RB (REX.R for R14, REX.B for R13)
    //   opcode = 0x03 (ADD r32, r/m32)
    //   ModRM: mod=01, reg=R14&7=6, rm=R13&7=5, disp8
    if (seg_ctx_offset >= 0) {
        e.emit8(REX_RB); e.emit8(0x03);
        e.emit8(uint8_t(0x40u | (6u << 3) | 5u)); // mod=01 reg=6 rm=5
        e.emit8((uint8_t)seg_ctx_offset);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Generic memory-operand rewriter: rewrites any instruction to use [R12+R14]
// instead of its original memory addressing. Handles x87, SSE, and integer
// instructions uniformly.
//
// Precondition: EA has been computed into R14 by emit_ea_to_r14().
//
// Strategy:
//   1. Copy legacy prefixes (skip address-size override and segment overrides)
//   2. Emit REX = REX_XB (REX.X for R14 index, REX.B for R12 base)
//   3. Copy opcode escape bytes (0F, 0F38, 0F3A) + opcode byte
//   4. Emit new ModRM: mod=00, reg=original, rm=100 (SIB follows)
//   5. Emit SIB = 0x34 (base=R12, index=R14, scale=1)
//   6. Copy any immediate bytes
// ---------------------------------------------------------------------------

inline bool emit_rewrite_mem_to_fastmem(Emitter& e,
                                         const ZydisDecodedInstruction& insn,
                                         const uint8_t* raw,
                                         bool esp_in_r8 = false) {
    // 1. Copy legacy prefixes (skip address-size, segment overrides, REX)
    for (uint8_t i = 0; i < insn.raw.prefix_count; ++i) {
        uint8_t val = insn.raw.prefixes[i].value;
        // Skip address-size override (not needed in 64-bit rewritten form)
        if (val == 0x67) continue;
        // Skip segment overrides (EA already resolved by emit_ea_to_r14)
        if (val == 0x26 || val == 0x2E || val == 0x36 || val == 0x3E ||
            val == 0x64 || val == 0x65) continue;
        // Skip REX (guest is 32-bit, shouldn't have one; we emit our own)
        if (val >= 0x40 && val <= 0x4F) continue;
        e.emit8(val);
    }

    // 2. Emit REX: 0x43 = REX.X(R14)+REX.B(R12)
    //    When esp_in_r8: 0x47 = REX.R(R8)+REX.X(R14)+REX.B(R12)
    e.emit8(esp_in_r8 ? 0x47u : 0x43u);

    // 3. Emit opcode escape bytes + opcode
    switch (insn.opcode_map) {
    case ZYDIS_OPCODE_MAP_DEFAULT: break;
    case ZYDIS_OPCODE_MAP_0F:     e.emit8(0x0F); break;
    case ZYDIS_OPCODE_MAP_0F38:   e.emit8(0x0F); e.emit8(0x38); break;
    case ZYDIS_OPCODE_MAP_0F3A:   e.emit8(0x0F); e.emit8(0x3A); break;
    default: return false; // VEX/EVEX/XOP not supported
    }
    e.emit8(insn.opcode);

    // 4. New ModRM: mod=00, reg field, rm=100 (SIB follows)
    //    When esp_in_r8: reg=0 (R8 low bits) instead of original reg field (4=RSP)
    uint8_t reg = esp_in_r8 ? 0u : insn.raw.modrm.reg;
    e.emit8(uint8_t((reg << 3) | 0x04));

    // 5. SIB: scale=0, index=R14(110), base=R12(100) = 0x34
    e.emit8(0x34);

    // 6. Displacement is absorbed into R14 — skip it.

    // 7. Copy immediate bytes (if any, e.g. SHUFPS xmm, [mem], imm8)
    for (int j = 0; j < 2; ++j) {
        if (insn.raw.imm[j].size > 0) {
            e.copy(raw + insn.raw.imm[j].offset, insn.raw.imm[j].size / 8);
        }
    }

    return true;
}

// JMP rel32
inline uint8_t* emit_jmp_fwd(Emitter& e) {
    e.emit8(0xE9); return e.reserve_rel32();
}

// ---------------------------------------------------------------------------
// EFLAGS save/restore — wraps memory dispatch sequences so that the inline
// SUB/ADD for ESP management does not clobber guest flags.
//
// PUSHFQ shifts RSP by 8, breaking 16-byte alignment.  We fix that with
// LEA RSP, [RSP ± 8] which does NOT affect EFLAGS.
// ---------------------------------------------------------------------------

inline void emit_save_flags(Emitter& e) {
    e.emit8(0x9C);                                             // PUSHFQ
    // LEA RSP, [RSP - 8]  (re-align to 16 bytes; LEA is flag-neutral)
    e.emit8(REX_W); e.emit8(0x8D); e.emit8(0x64); e.emit8(0x24); e.emit8(0xF8);
}

inline void emit_restore_flags(Emitter& e) {
    // LEA RSP, [RSP + 8]  (undo alignment padding)
    e.emit8(REX_W); e.emit8(0x8D); e.emit8(0x64); e.emit8(0x24); e.emit8(0x08);
    e.emit8(0x9D);                                             // POPFQ
}

// ---------------------------------------------------------------------------
// Fastmem access: OP reg, [R12+R14]  or  OP [R12+R14], reg
//
// R12 (base, REX.B=1) + R14 (index, REX.X=1): REX = REX_XB
// SIB: scale=0, index=6(R14&7), base=4(R12&7) → 0x34
// ModRM: mod=00, reg=guest_enc, rm=4(SIB) → guest_enc<<3 | 4
// ---------------------------------------------------------------------------

inline void emit_fastmem_op(Emitter& e, uint8_t guest_enc,
                              unsigned size_bits, bool is_load) {
    if (size_bits == 16) e.emit8(0x66);
    uint8_t rex = REX_XB; // REX.B(R12) | REX.X(R14)
    uint8_t opc = is_load ? 0x8B : 0x89;
    if (size_bits == 8) opc = is_load ? 0x8A : 0x88;

    e.emit8(rex);
    e.emit8(opc);
    e.emit8(uint8_t((guest_enc << 3) | 4u)); // mod=00, reg=guest_enc, rm=4
    e.emit8(0x34);                            // SIB: scale=0 idx=R14 base=R12
}

// MOVZX r32, [R12+R14]  for 8/16-bit loads with zero-extension
// When esp_in_r8=true: use R8D as destination (REX.R=1, reg=0) for guest ESP.
inline void emit_fastmem_movzx(Emitter& e, uint8_t guest_enc, unsigned src_bits,
                                bool esp_in_r8 = false) {
    uint8_t rex = esp_in_r8 ? 0x47u : 0x43u;
    uint8_t enc = esp_in_r8 ? 0u : guest_enc;
    e.emit8(rex); e.emit8(0x0F);
    e.emit8(src_bits == 8 ? 0xB6u : 0xB7u);
    e.emit8(uint8_t((enc << 3) | 4u));
    e.emit8(0x34);
}

// MOVSX r32, [R12+R14]  for 8/16-bit loads with sign-extension
// When esp_in_r8=true: use R8D as destination (REX.R=1, reg=0) for guest ESP.
inline void emit_fastmem_movsx(Emitter& e, uint8_t guest_enc, unsigned src_bits,
                                bool esp_in_r8 = false) {
    uint8_t rex = esp_in_r8 ? 0x47u : 0x43u;
    uint8_t enc = esp_in_r8 ? 0u : guest_enc;
    e.emit8(rex); e.emit8(0x0F);
    e.emit8(src_bits == 8 ? 0xBEu : 0xBFu);
    e.emit8(uint8_t((enc << 3) | 4u));
    e.emit8(0x34);
}

// MOV DWORD PTR [R12+R14], imm32
// REX=0x43, opcode=0xC7, ModRM=mod=00 reg=0 rm=4(SIB), SIB=0x34, imm32
inline void emit_fastmem_store_imm32(Emitter& e, uint32_t imm) {
    e.emit8(REX_XB); e.emit8(0xC7);
    e.emit8(0x04); e.emit8(0x34);
    e.emit32(imm);
}

// MOV WORD PTR [R12+R14], imm16
inline void emit_fastmem_store_imm16(Emitter& e, uint16_t imm) {
    e.emit8(0x66);  // operand-size prefix
    e.emit8(REX_XB); e.emit8(0xC7);
    e.emit8(0x04); e.emit8(0x34);
    e.emit8(uint8_t(imm & 0xFF));
    e.emit8(uint8_t(imm >> 8));
}

// MOV BYTE PTR [R12+R14], imm8
inline void emit_fastmem_store_imm8(Emitter& e, uint8_t imm) {
    e.emit8(REX_XB); e.emit8(0xC6);
    e.emit8(0x04); e.emit8(0x34);
    e.emit8(imm);
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
    e.emit8(REX_W); e.emit8(0x83); e.emit8(0xEC); e.emit8(0x20);
#endif
    e.emit8(REX_WB); e.emit8(0xBE); e.emit64((uint64_t)(uintptr_t)target);
    e.emit8(REX_B); e.emit8(0xFF); e.emit8(0xD6);
#ifdef _WIN32
    // ADD RSP, 0x20
    e.emit8(REX_W); e.emit8(0x83); e.emit8(0xC4); e.emit8(0x20);
#endif
}

// ---------------------------------------------------------------------------
// Sub/Add DWORD PTR [R13 + offset], imm8
//   REX.B=1: 0x41, 0x83, ModRM, disp8, imm8
//   ModRM for SUB: mod=01 reg=5(SUB) rm=5(R13&7) = 01_101_101 = 0x6D
//   ModRM for ADD: mod=01 reg=0(ADD) rm=5(R13&7) = 01_000_101 = 0x45
// ---------------------------------------------------------------------------

inline void emit_sub_ctx_esp(Emitter& e, uint8_t amount) {
    e.emit8(REX_B); e.emit8(0x83);
    e.emit8(0x6D); // SUB [R13+?]
    e.emit8(gp_offset(GP_ESP));
    e.emit8(amount);
}

inline void emit_add_ctx_esp(Emitter& e, uint8_t amount) {
    e.emit8(REX_B); e.emit8(0x83);
    e.emit8(0x45); // ADD [R13+?]
    e.emit8(gp_offset(GP_ESP));
    e.emit8(amount);
}

// Load ctx->esp into R14D:
//   MOV R14D, [R13 + gp_offset(GP_ESP)]
//   REX=0x45 (REX.R R14, REX.B R13), 0x8B, mod=01 reg=6(R14&7) rm=5(R13&7), disp8
inline void emit_load_esp_to_r14(Emitter& e) {
    e.emit8(REX_RB); e.emit8(0x8B);
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));
    e.emit8(gp_offset(GP_ESP));
}

// Store R14D back to ctx->gp[GP_ESP]:
//   MOV [R13 + gp_offset(GP_ESP)], R14D
//   REX=0x45 (REX.R R14, REX.B R13), 0x89, mod=01 reg=6(R14&7) rm=5(R13&7), disp8
inline void emit_store_r14_to_esp(Emitter& e) {
    e.emit8(REX_RB); e.emit8(0x89);
    e.emit8(uint8_t(0x40u | (6u << 3) | 5u));
    e.emit8(gp_offset(GP_ESP));
}

// Load ctx->esp into R8D (scratch for ESP-in-reg memory operations):
//   MOV R8D, [R13 + gp_offset(GP_ESP)]
//   REX=0x45 (REX.R R8, REX.B R13), 0x8B, mod=01 reg=0(R8&7) rm=5(R13&7), disp8
inline void emit_load_esp_to_r8(Emitter& e) {
    e.emit8(REX_RB); e.emit8(0x8B);
    e.emit8(uint8_t(0x40u | (0u << 3) | 5u));
    e.emit8(gp_offset(GP_ESP));
}

// Store R8D back to ctx->gp[GP_ESP]:
//   MOV [R13 + gp_offset(GP_ESP)], R8D
//   REX=0x45 (REX.R R8, REX.B R13), 0x89, mod=01 reg=0(R8&7) rm=5(R13&7), disp8
inline void emit_store_r8_to_esp(Emitter& e) {
    e.emit8(REX_RB); e.emit8(0x89);
    e.emit8(uint8_t(0x40u | (0u << 3) | 5u));
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
    e.emit8(REX_WR); e.emit8(0x89); e.emit8(0xE9);
#else
    // MOV RDI, R13
    e.emit8(REX_WB); e.emit8(0x8B); e.emit8(0xFD);
#endif
}

// arg1 = PA (from R14D)
inline void emit_ccall_arg1_pa(Emitter& e) {
#ifdef _WIN32
    // MOV EDX, R14D
    e.emit8(REX_R); e.emit8(0x89); e.emit8(0xF2);
#else
    // MOV ESI, R14D (zero-extends to RSI)
    e.emit8(REX_B); e.emit8(0x8B); e.emit8(0xF6);
#endif
}

// arg1 = imm32
inline void emit_ccall_arg1_imm(Emitter& e, uint32_t v) {
#ifdef _WIN32
    // MOV EDX, imm32
    e.emit8(0xBA); e.emit32(v);
#else
    // MOV ESI, imm32
    e.emit8(0xBE); e.emit32(v);
#endif
}

// arg2 = imm32
inline void emit_ccall_arg2_imm(Emitter& e, uint32_t v) {
#ifdef _WIN32
    // MOV R8D, imm32
    e.emit8(REX_B); e.emit8(0xB8); e.emit32(v);
#else
    // MOV EDX, imm32
    e.emit8(0xBA); e.emit32(v);
#endif
}

// arg3 = imm32
inline void emit_ccall_arg3_imm(Emitter& e, uint32_t v) {
#ifdef _WIN32
    // MOV R9D, imm32
    e.emit8(REX_B); e.emit8(0xB9); e.emit32(v);
#else
    // MOV ECX, imm32
    e.emit8(0xB9); e.emit32(v);
#endif
}

// ---------------------------------------------------------------------------
// VA→PA translation call emitted inline when paging (CR0.PG) is active.
//
// After emit_ea_to_r14() has synthesized the guest VA into R14, this emits
// a call to translate_va_jit(ctx, VA, is_write) → PA.  If the translation
// faults (returns ~0u), the trace exits with STOP_PAGE_FAULT.
//
// The caller is responsible for saving/restoring GP regs around this if they
// are live (typically emit_save_all_gp has already been called, or we call
// it here).
// ---------------------------------------------------------------------------
extern "C" uint32_t translate_va_jit(GuestContext*, uint32_t, uint32_t);

// Emit: PUSHFQ → save GP → translate_va_jit(ctx, R14, is_write) →
//       check fault → MOV R14D,EAX → load GP → POPFQ.
//
// Saves/restores both guest flags (RFLAGS) and GP regs around the C call.
// On fault: writes STOP_PAGE_FAULT + fault_eip to ctx, POPFQs, RETs cleanly.
// MUST be called BEFORE any emit_save_flags to keep the stack clean on fault.
inline void emit_translate_r14(Emitter& e, bool is_write, uint32_t fault_eip) {
    // Save guest RFLAGS before C call (C ABI clobbers flags)
    e.emit8(0x9C); // PUSHFQ

    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);        // arg0 = ctx
    emit_ccall_arg1_pa(e);         // arg1 = R14D = VA
    emit_ccall_arg2_imm(e, is_write ? 1 : 0); // arg2 = is_write
    emit_call_abs(e, reinterpret_cast<void*>(translate_va_jit));

    // Check for fault: EAX == ~0u
    // CMP EAX, -1  →  83 F8 FF
    e.emit8(0x83); e.emit8(0xF8); e.emit8(0xFF);
    // JNE +N (skip fault handler); short jump, patch later
    e.emit8(0x75);
    uint8_t* skip = e.cur();
    e.emit8(0x00); // placeholder

    // Fault path: store STOP_PAGE_FAULT, fault_eip, then exit trace.
    // MOV DWORD [R13 + stop_reason], STOP_PAGE_FAULT (2)
    e.emit8(REX_B); e.emit8(0xC7); e.emit8(0x45);
    e.emit8(CTX_STOP_REASON);
    e.emit32(STOP_PAGE_FAULT);
    // MOV DWORD [R13 + next_eip], fault_eip
    e.emit8(REX_B); e.emit8(0xC7); e.emit8(0x45);
    e.emit8(CTX_NEXT_EIP);
    e.emit32(fault_eip);
    // Restore GP regs + RFLAGS, then RET (clean stack: only PUSHFQ on it)
    emit_load_all_gp(e);
    e.emit8(0x9D); // POPFQ — balance the PUSHFQ
    e.emit8(0xC3); // RET

    // Patch the JNE skip target
    *skip = (uint8_t)(e.cur() - skip - 1);

    // Normal path: R14D = EAX (PA)
    // MOV R14D, EAX
    e.emit8(REX_B); e.emit8(0x89); e.emit8(0xC6);
    emit_load_all_gp(e);
    // Restore guest RFLAGS (preserved across C call)
    e.emit8(0x9D); // POPFQ
}

// Translate R14 from VA→PA if paging is active. No-op when paging is off.
// MUST be called BEFORE emit_save_flags to keep the stack clean on fault.
inline void emit_paging_translate(Emitter& e, bool is_write) {
    if (e.paging) emit_translate_r14(e, is_write, e.fault_eip);
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
