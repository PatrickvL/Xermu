#include "trace_builder.hpp"
#include "insn_dispatch.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>

// ---------------------------------------------------------------------------
// C-linkage MMIO slow-path helpers called from JIT code.
// All guest GP regs are saved to GuestContext before the call.
// ---------------------------------------------------------------------------

extern "C" {

void mmio_dispatch_read(GuestContext* ctx, uint32_t pa,
                        uint32_t dst_gp_idx, uint32_t size_bytes) {
    ctx->gp[dst_gp_idx] = ctx->mmio
        ? ctx->mmio->read(pa, size_bytes)
        : 0xFFFF'FFFFu;
}

void mmio_dispatch_write(GuestContext* ctx, uint32_t pa,
                         uint32_t src_gp_idx, uint32_t size_bytes) {
    if (ctx->mmio)
        ctx->mmio->write(pa, ctx->gp[src_gp_idx], size_bytes);
}

void mmio_dispatch_write_imm(GuestContext* ctx, uint32_t pa,
                             uint32_t value, uint32_t size_bytes) {
    if (ctx->mmio)
        ctx->mmio->write(pa, value, size_bytes);
}

// PUSHFD helper: push EFLAGS onto the guest stack.
// Called with the captured EFLAGS value in `eflags_val`.
void pushfd_helper(GuestContext* ctx, uint32_t eflags_val) {
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    if (esp < ctx->ram_size) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        *reinterpret_cast<uint32_t*>(base + esp) = eflags_val;
    }
}

// POPFD helper: pop EFLAGS from the guest stack.
// Returns the 32-bit EFLAGS value.
uint32_t popfd_helper(GuestContext* ctx) {
    uint32_t esp = ctx->gp[GP_ESP];
    ctx->gp[GP_ESP] = esp + 4;
    if (esp < ctx->ram_size) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        return *reinterpret_cast<uint32_t*>(base + esp);
    }
    return 0;
}

// Read a 32-bit value from guest physical memory (or MMIO).
// Used by JMP/CALL [mem] handlers.
uint32_t read_guest_mem32(GuestContext* ctx, uint32_t pa) {
    if (pa < ctx->ram_size) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        return *reinterpret_cast<uint32_t*>(base + pa);
    }
    return ctx->mmio ? ctx->mmio->read(pa, 4) : 0xFFFF'FFFFu;
}

// Write a 32-bit value to guest physical memory (or MMIO).
// Used by PUSH [mem] and CALL [mem] handlers.
void write_guest_mem32(GuestContext* ctx, uint32_t pa, uint32_t val) {
    if (pa < ctx->ram_size) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        *reinterpret_cast<uint32_t*>(base + pa) = val;
        return;
    }
    if (ctx->mmio) ctx->mmio->write(pa, val, 4);
}

// CALL [mem] helper: reads jump target from `pa`, pushes `retaddr` onto
// guest stack, returns the call target address.
uint32_t call_mem_helper(GuestContext* ctx, uint32_t pa, uint32_t retaddr) {
    uint32_t target = read_guest_mem32(ctx, pa);
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    write_guest_mem32(ctx, esp, retaddr);
    return target;
}

// PUSH [mem] helper: reads 32-bit value from `pa`, pushes onto guest stack.
void push_mem_helper(GuestContext* ctx, uint32_t pa) {
    uint32_t val = read_guest_mem32(ctx, pa);
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    write_guest_mem32(ctx, esp, val);
}

// POP [mem] helper: pops 32-bit value from guest stack, writes to `pa`.
void pop_mem_helper(GuestContext* ctx, uint32_t pa) {
    uint32_t esp = ctx->gp[GP_ESP];
    uint32_t val = read_guest_mem32(ctx, esp);
    ctx->gp[GP_ESP] = esp + 4;
    write_guest_mem32(ctx, pa, val);
}

// PUSHAD helper: push EAX, ECX, EDX, EBX, original-ESP, EBP, ESI, EDI
void pushad_helper(GuestContext* ctx) {
    uint32_t orig_esp = ctx->gp[GP_ESP];
    uint32_t esp = orig_esp;
    static constexpr int order[] = { GP_EAX, GP_ECX, GP_EDX, GP_EBX };
    for (int i : order) {
        esp -= 4;
        write_guest_mem32(ctx, esp, ctx->gp[i]);
    }
    esp -= 4;
    write_guest_mem32(ctx, esp, orig_esp); // push original ESP
    static constexpr int order2[] = { GP_EBP, GP_ESI, GP_EDI };
    for (int i : order2) {
        esp -= 4;
        write_guest_mem32(ctx, esp, ctx->gp[i]);
    }
    ctx->gp[GP_ESP] = esp;
}

// POPAD helper: pop EDI, ESI, EBP, skip-ESP, EBX, EDX, ECX, EAX
void popad_helper(GuestContext* ctx) {
    uint32_t esp = ctx->gp[GP_ESP];
    ctx->gp[GP_EDI] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_ESI] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_EBP] = read_guest_mem32(ctx, esp); esp += 4;
    esp += 4; // skip ESP slot
    ctx->gp[GP_EBX] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_EDX] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_ECX] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_EAX] = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_ESP] = esp;
}

// IRETD helper: pop EIP, CS, EFLAGS from guest stack (12 bytes).
// Sets ctx->next_eip and ctx->eflags.  CS is ignored (flat model).
void iret_helper(GuestContext* ctx) {
    uint32_t esp = ctx->gp[GP_ESP];
    uint32_t new_eip    = read_guest_mem32(ctx, esp); esp += 4;
    /* uint32_t cs = */    read_guest_mem32(ctx, esp); esp += 4; // ignored
    uint32_t new_eflags = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_ESP] = esp;
    ctx->next_eip   = new_eip;
    // Preserve only safe flags (mask out VM, IOPL, VIF, VIP, RF):
    ctx->eflags = (new_eflags & 0x003F7FD5u) | 0x02u; // bit 1 always set
}

// ---------------------------------------------------------------------------
// String instruction helpers (MOVS/STOS/LODS/CMPS/SCAS).
//
// Called from JIT code with all GP regs saved to ctx.
//   arg0  ctx        — GuestContext*
//   arg1  eflags     — captured host EFLAGS (DF bit controls direction)
//   arg2  elem_size  — 1, 2, or 4
//   arg3  rep_mode   — 0 = no prefix, 1 = REP/REPE, 2 = REPNE
//
// Returns the EFLAGS to restore.  MOVS/STOS/LODS return the input value
// unchanged; CMPS/SCAS return updated arithmetic flags.
// ---------------------------------------------------------------------------

static uint32_t compute_sub_flags(uint32_t a, uint32_t b, uint32_t elem_size) {
    uint32_t mask = (elem_size == 1) ? 0xFFu
                  : (elem_size == 2) ? 0xFFFFu
                  :                    0xFFFFFFFFu;
    a &= mask; b &= mask;
    uint32_t result = (a - b) & mask;
    uint32_t flags = 0;
    if (a < b)                    flags |= (1u << 0);  // CF
    { uint8_t p = (uint8_t)result;
      p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
      if (!(p & 1))              flags |= (1u << 2); } // PF
    if ((a ^ b ^ result) & 0x10) flags |= (1u << 4);  // AF
    if (result == 0)             flags |= (1u << 6);  // ZF
    uint32_t sb = (elem_size == 1) ? 7 : (elem_size == 2) ? 15 : 31;
    if ((result >> sb) & 1)      flags |= (1u << 7);  // SF
    uint32_t sa = (a >> sb) & 1, sbb = (b >> sb) & 1, sr = (result >> sb) & 1;
    if (sa != sbb && sa != sr)   flags |= (1u << 11); // OF
    return flags;
}

static constexpr uint32_t ARITH_FLAGS_MASK = 0x8D5u; // CF|PF|AF|ZF|SF|OF

uint32_t string_movs_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        switch (elem_size) {
        case 1: *(uint8_t *)(base+edi) = *(uint8_t *)(base+esi); break;
        case 2: *(uint16_t*)(base+edi) = *(uint16_t*)(base+esi); break;
        case 4: *(uint32_t*)(base+edi) = *(uint32_t*)(base+esi); break;
        }
        esi += dir; edi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_stos_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t eax = ctx->gp[GP_EAX];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        switch (elem_size) {
        case 1: *(uint8_t *)(base+edi) = (uint8_t)eax;  break;
        case 2: *(uint16_t*)(base+edi) = (uint16_t)eax;  break;
        case 4: *(uint32_t*)(base+edi) = eax;             break;
        }
        edi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_lods_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& eax = ctx->gp[GP_EAX];
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        switch (elem_size) {
        case 1: eax = (eax & 0xFFFFFF00u) | *(uint8_t *)(base+esi); break;
        case 2: eax = (eax & 0xFFFF0000u) | *(uint16_t*)(base+esi); break;
        case 4: eax = *(uint32_t*)(base+esi);                        break;
        }
        esi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_cmps_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];
    uint32_t rf = eflags;

    auto do_one = [&]() {
        uint32_t s = 0, d = 0;
        switch (elem_size) {
        case 1: s = *(uint8_t *)(base+esi); d = *(uint8_t *)(base+edi); break;
        case 2: s = *(uint16_t*)(base+esi); d = *(uint16_t*)(base+edi); break;
        case 4: s = *(uint32_t*)(base+esi); d = *(uint32_t*)(base+edi); break;
        }
        rf = (rf & ~ARITH_FLAGS_MASK) | compute_sub_flags(s, d, elem_size);
        esi += dir; edi += dir;
    };

    if (rep_mode == 0) {
        do_one();
    } else {
        while (ecx > 0) {
            do_one(); ecx--;
            if (rep_mode == 1 && !(rf & (1u<<6))) break; // REPE: stop if ZF=0
            if (rep_mode == 2 &&  (rf & (1u<<6))) break; // REPNE: stop if ZF=1
        }
    }
    return rf;
}

uint32_t string_scas_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t eax = ctx->gp[GP_EAX];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];
    uint32_t rf = eflags;

    auto do_one = [&]() {
        uint32_t d = 0, s = 0;
        switch (elem_size) {
        case 1: s = eax & 0xFFu;     d = *(uint8_t *)(base+edi); break;
        case 2: s = eax & 0xFFFFu;   d = *(uint16_t*)(base+edi); break;
        case 4: s = eax;              d = *(uint32_t*)(base+edi); break;
        }
        rf = (rf & ~ARITH_FLAGS_MASK) | compute_sub_flags(s, d, elem_size);
        edi += dir;
    };

    if (rep_mode == 0) {
        do_one();
    } else {
        while (ecx > 0) {
            do_one(); ecx--;
            if (rep_mode == 1 && !(rf & (1u<<6))) break; // REPE: stop if ZF=0
            if (rep_mode == 2 &&  (rf & (1u<<6))) break; // REPNE: stop if ZF=1
        }
    }
    return rf;
}

} // extern "C"

// ---------------------------------------------------------------------------
// TraceBuilder
// ---------------------------------------------------------------------------

TraceBuilder::TraceBuilder() {
    ZydisDecoderInit(&decoder_,
                     ZYDIS_MACHINE_MODE_LEGACY_32,
                     ZYDIS_STACK_WIDTH_32);
    init_mnemonic_table();
}

// ---------------------------------------------------------------------------
// Static helpers — defined as class members to match the .hpp declarations
// ---------------------------------------------------------------------------

bool TraceBuilder::has_mem_operand(const ZydisDecodedOperand* ops,
                                    uint8_t count, int& mem_idx) {
    for (int i = 0; i < (int)count; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            mem_idx = i;
            return true;
        }
    }
    return false;
}

uint8_t TraceBuilder::jcc_short_opcode(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_JO:    return 0x70;
        case ZYDIS_MNEMONIC_JNO:   return 0x71;
        case ZYDIS_MNEMONIC_JB:    return 0x72;
        case ZYDIS_MNEMONIC_JNB:   return 0x73;
        case ZYDIS_MNEMONIC_JZ:    return 0x74;
        case ZYDIS_MNEMONIC_JNZ:   return 0x75;
        case ZYDIS_MNEMONIC_JBE:   return 0x76;
        case ZYDIS_MNEMONIC_JNBE:  return 0x77;
        case ZYDIS_MNEMONIC_JS:    return 0x78;
        case ZYDIS_MNEMONIC_JNS:   return 0x79;
        case ZYDIS_MNEMONIC_JP:    return 0x7A;
        case ZYDIS_MNEMONIC_JNP:   return 0x7B;
        case ZYDIS_MNEMONIC_JL:    return 0x7C;
        case ZYDIS_MNEMONIC_JNL:   return 0x7D;
        case ZYDIS_MNEMONIC_JLE:   return 0x7E;
        case ZYDIS_MNEMONIC_JNLE:  return 0x7F;
        default:                    return 0;
    }
}

// ===========================================================================
// Emit handlers — free functions called via the dispatch table
// ===========================================================================

// Shared: emit inline fastmem dispatch for a reg/mem MOV-like instruction.
// `guest_enc` is the register encoding, `size_bits` the operand width,
// `is_load` true for load, false for store.
static bool emit_fastmem_dispatch(Emitter& e, const ZydisDecodedOperand& mem_op,
                                   uint8_t guest_enc, unsigned size_bits,
                                   bool is_load, bool save_flags) {
    if (!emit_ea_to_r14(e, mem_op)) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);
    emit_fastmem_op(e, guest_enc, size_bits, is_load);
    uint8_t* done_site = emit_jmp_fwd(e);

    Emitter::patch_rel32(slow_site, e.cur());
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, guest_enc);
    emit_ccall_arg3_imm(e, size_bits / 8);
    emit_call_abs(e, is_load
        ? reinterpret_cast<void*>(mmio_dispatch_read)
        : reinterpret_cast<void*>(mmio_dispatch_write));
    emit_load_all_gp(e);

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// Shared: emit inline fastmem dispatch for MOV [mem], imm32
static bool emit_fastmem_dispatch_store_imm(Emitter& e,
                                             const ZydisDecodedOperand& mem_op,
                                             uint32_t imm, unsigned size_bits,
                                             bool save_flags) {
    if (!emit_ea_to_r14(e, mem_op)) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);
    // fast: store immediate to [R12+R14]
    if (size_bits == 32)     emit_fastmem_store_imm32(e, imm);
    else if (size_bits == 16) emit_fastmem_store_imm16(e, (uint16_t)imm);
    else                     emit_fastmem_store_imm8(e, (uint8_t)imm);
    uint8_t* done_site = emit_jmp_fwd(e);

    Emitter::patch_rel32(slow_site, e.cur());
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, imm);           // arg2 = value (not gp index)
    emit_ccall_arg3_imm(e, size_bits / 8);
    emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write_imm));
    emit_load_all_gp(e);

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_clean — verbatim copy with INC/DEC r32 re-encoding
// ---------------------------------------------------------------------------
bool emit_handler_clean(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, const uint8_t* raw,
                        GuestContext* /*ctx*/, bool /*save_flags*/) {
    return emit_clean_insn(e, insn, ops, raw);
}

// ---------------------------------------------------------------------------
// emit_handler_lea — LEA has no memory access; re-encode to host form
// ---------------------------------------------------------------------------
bool emit_handler_lea(Emitter& e, const ZydisDecodedInstruction& insn,
                      const ZydisDecodedOperand* ops, const uint8_t* raw,
                      GuestContext* /*ctx*/, bool /*save_flags*/) {
    // LEA reg, [mem] — compute EA in R14, then MOV guest_reg, R14D
    if (insn.operand_count < 2) return false;
    if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
    if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY)   return false;

    uint8_t dst_enc = 0;
    if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;

    if (!emit_ea_to_r14(e, ops[1])) return false;

    // MOV dst_reg, R14D:  REX=0x44 (REX.R for R14), 0x89, ModRM=0xC0|dst
    e.emit8(0x44); e.emit8(0x89);
    e.emit8(uint8_t(0xC0u | (6u << 3) | dst_enc)); // mod=11 reg=R14&7=6 rm=dst
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_mov_mem — MOV r,[m] / MOV [m],r / MOV [m],imm / MOV CRn,r
// ---------------------------------------------------------------------------

static bool is_control_register(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_CR0 && r <= ZYDIS_REGISTER_CR15;
}

static bool is_debug_register(ZydisRegister r) {
    return r >= ZYDIS_REGISTER_DR0 && r <= ZYDIS_REGISTER_DR15;
}

bool emit_handler_mov_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* /*ctx*/, bool save_flags) {
    int mem_idx = -1;
    if (!TraceBuilder::has_mem_operand(ops, insn.operand_count, mem_idx)) {
        // reg-reg MOV: verbatim copy
        e.copy(raw, insn.length);
        return true;
    }

    int other_idx = (mem_idx == 0) ? 1 : 0;
    bool is_load = (mem_idx != 0); // load = mem is source

    // MOV [m], imm
    if (ops[other_idx].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        uint32_t imm = (uint32_t)(int32_t)ops[other_idx].imm.value.s;
        return emit_fastmem_dispatch_store_imm(e, ops[mem_idx], imm,
                                                insn.operand_width, save_flags);
    }

    // MOV r, [m]  or  MOV [m], r
    if (ops[other_idx].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;
    uint8_t guest_enc = 0;
    if (!guest_reg_enc(ops[other_idx].reg.value, guest_enc)) return false;
    return emit_fastmem_dispatch(e, ops[mem_idx], guest_enc,
                                  insn.operand_width, is_load, save_flags);
}

// ---------------------------------------------------------------------------
// emit_handler_alu_mem — ALU r,[m] / [m],r / [m],imm
// For instructions with no mem operand (reg-reg), falls back to clean copy.
// Memory forms use the generic rewriter to execute ALU on [R12+R14].
// ---------------------------------------------------------------------------
bool emit_handler_alu_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* /*ctx*/, bool save_flags) {
    int mem_idx = -1;
    if (!TraceBuilder::has_mem_operand(ops, insn.operand_count, mem_idx)) {
        // No mem operand → clean copy (handles INC/DEC re-encoding too)
        return emit_clean_insn(e, insn, ops, raw);
    }

    // Memory form: compute EA, bounds check, rewrite for fastmem
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);

    // Fast path: rewrite the instruction to use [R12+R14]
    if (!emit_rewrite_mem_to_fastmem(e, insn, raw)) return false;
    uint8_t* done_site = emit_jmp_fwd(e);

    // Slow path: MMIO not supported for ALU-mem — UD2 trap
    Emitter::patch_rel32(slow_site, e.cur());
    e.emit8(0x0F); e.emit8(0x0B); // UD2

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_test_mem — TEST r,[m] / [m],r  (read-only)
// Memory forms use the generic rewriter.
// ---------------------------------------------------------------------------
bool emit_handler_test_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                           const ZydisDecodedOperand* ops, const uint8_t* raw,
                           GuestContext* /*ctx*/, bool save_flags) {
    int mem_idx = -1;
    if (!TraceBuilder::has_mem_operand(ops, insn.operand_count, mem_idx)) {
        e.copy(raw, insn.length);
        return true;
    }

    // Memory form: compute EA, bounds check, rewrite for fastmem
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);

    // Fast path: rewrite the instruction to use [R12+R14]
    if (!emit_rewrite_mem_to_fastmem(e, insn, raw)) return false;
    uint8_t* done_site = emit_jmp_fwd(e);

    // Slow path: UD2
    Emitter::patch_rel32(slow_site, e.cur());
    e.emit8(0x0F); e.emit8(0x0B); // UD2

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_push — PUSH r32 / PUSH imm (dispatch based on operand)
// ---------------------------------------------------------------------------
bool emit_handler_push(Emitter& e, const ZydisDecodedInstruction& insn,
                       const ZydisDecodedOperand* ops, const uint8_t* raw,
                       GuestContext* /*ctx*/, bool save_flags) {
    // PUSH imm8/imm32
    if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        uint32_t imm = (uint32_t)(int32_t)ops[0].imm.value.s;
        if (save_flags) emit_save_flags(e);
        emit_sub_ctx_esp(e, 4);
        emit_load_esp_to_r14(e);

        emit_cmp_r14_r15(e);
        uint8_t* slow_site = emit_jae_fwd(e);
        emit_fastmem_store_imm32(e, imm);
        uint8_t* done_site = emit_jmp_fwd(e);

        Emitter::patch_rel32(slow_site, e.cur());
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, imm);
        emit_ccall_arg3_imm(e, 4);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write_imm));
        emit_load_all_gp(e);

        Emitter::patch_rel32(done_site, e.cur());
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    // PUSH r32
    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        uint8_t reg_enc = 0;
        if (!reg32_enc(ops[0].reg.value, reg_enc)) return false;

        if (save_flags) emit_save_flags(e);
        emit_sub_ctx_esp(e, 4);
        emit_load_esp_to_r14(e);

        emit_cmp_r14_r15(e);
        uint8_t* slow_site = emit_jae_fwd(e);
        emit_fastmem_op(e, reg_enc, 32, false);
        uint8_t* done_site = emit_jmp_fwd(e);

        Emitter::patch_rel32(slow_site, e.cur());
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, reg_enc);
        emit_ccall_arg3_imm(e, 4);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write));
        emit_load_all_gp(e);

        Emitter::patch_rel32(done_site, e.cur());
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    // PUSH [mem] — read from memory, push onto guest stack
    if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (!emit_ea_to_r14(e, ops[0])) return false;
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_call_abs(e, reinterpret_cast<void*>(push_mem_helper));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// emit_handler_pop — POP r32
// ---------------------------------------------------------------------------
bool emit_handler_pop(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                      const ZydisDecodedOperand* ops, const uint8_t* /*raw*/,
                      GuestContext* /*ctx*/, bool save_flags) {
    // POP r32
    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        uint8_t reg_enc = 0;
        if (!reg32_enc(ops[0].reg.value, reg_enc)) return false;

        if (save_flags) emit_save_flags(e);
        emit_load_esp_to_r14(e);

        emit_cmp_r14_r15(e);
        uint8_t* slow_site = emit_jae_fwd(e);
        emit_fastmem_op(e, reg_enc, 32, true);
        uint8_t* done_site = emit_jmp_fwd(e);

        Emitter::patch_rel32(slow_site, e.cur());
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, reg_enc);
        emit_ccall_arg3_imm(e, 4);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
        emit_load_all_gp(e);

        Emitter::patch_rel32(done_site, e.cur());
        emit_add_ctx_esp(e, 4);
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    // POP [mem] — pop from guest stack, write to memory address
    if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (!emit_ea_to_r14(e, ops[0])) return false;
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_call_abs(e, reinterpret_cast<void*>(pop_mem_helper));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// emit_handler_push_imm — (unused, PUSH routes through emit_handler_push)
// ---------------------------------------------------------------------------
bool emit_handler_push_imm(Emitter& e, const ZydisDecodedInstruction& insn,
                           const ZydisDecodedOperand* ops, const uint8_t* raw,
                           GuestContext* ctx, bool save_flags) {
    return emit_handler_push(e, insn, ops, raw, ctx, save_flags);
}

// ---------------------------------------------------------------------------
// emit_handler_pushad — PUSHAD: push all 8 GP regs onto guest stack.
// ---------------------------------------------------------------------------
bool emit_handler_pushad(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                         const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                         GuestContext* /*ctx*/, bool save_flags) {
    if (save_flags) emit_save_flags(e);
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_call_abs(e, reinterpret_cast<void*>(pushad_helper));
    emit_load_all_gp(e);
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_popad — POPAD: pop all 8 GP regs from guest stack (skip ESP).
// ---------------------------------------------------------------------------
bool emit_handler_popad(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                        const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                        GuestContext* /*ctx*/, bool save_flags) {
    if (save_flags) emit_save_flags(e);
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_call_abs(e, reinterpret_cast<void*>(popad_helper));
    emit_load_all_gp(e);
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_leave — LEAVE = MOV ESP, EBP; POP EBP
// ---------------------------------------------------------------------------
bool emit_handler_leave(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                        const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                        GuestContext* /*ctx*/, bool save_flags) {
    // Step 1: ctx->gp[ESP] = EBP (host EBP register, which is guest EBP)
    // MOV [R13 + gp_offset(ESP)], EBP:  REX.B=0x41, 0x89, mod=01 reg=EBP(5) rm=R13(5), disp8
    e.emit8(0x41); e.emit8(0x89);
    e.emit8(uint8_t(0x45u | (GP_EBP << 3)));  // mod=01, reg=EBP=5, rm=R13&7=5
    e.emit8(gp_offset(GP_ESP));

    // Step 2: POP EBP — read dword from [ESP], write to EBP, ESP += 4
    if (save_flags) emit_save_flags(e);
    emit_load_esp_to_r14(e);

    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);
    emit_fastmem_op(e, GP_EBP, 32, true);
    uint8_t* done_site = emit_jmp_fwd(e);

    Emitter::patch_rel32(slow_site, e.cur());
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, GP_EBP);
    emit_ccall_arg3_imm(e, 4);
    emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
    emit_load_all_gp(e);

    Emitter::patch_rel32(done_site, e.cur());
    emit_add_ctx_esp(e, 4);
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_pushfd — PUSHFD: push EFLAGS onto guest stack.
//
// 1. Capture host EFLAGS into R14 (PUSHFQ + POP R14).
// 2. Save all GP registers, call pushfd_helper(ctx, R14D), restore GP.
// 3. The helper decrements guest ESP and stores EFLAGS at [fastmem+ESP].
//
// PUSHFD is read-only w.r.t. EFLAGS, so the save_flags wrapper preserves
// them for subsequent instructions.
// ---------------------------------------------------------------------------
bool emit_handler_pushfd(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                         const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                         GuestContext* /*ctx*/, bool save_flags) {
    // Capture EFLAGS → R14D
    e.emit8(0x9C);                      // PUSHFQ
    e.emit8(0x41); e.emit8(0x5E);      // POP R14

    if (save_flags) emit_save_flags(e);

    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);             // arg0 = ctx
    emit_ccall_arg1_pa(e);              // arg1 = R14D (captured EFLAGS)
    emit_call_abs(e, reinterpret_cast<void*>(pushfd_helper));
    emit_load_all_gp(e);

    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_popfd — POPFD: pop EFLAGS from guest stack.
//
// 1. Save all GP, call popfd_helper(ctx) → returns EFLAGS in EAX.
// 2. PUSH RAX + POPFQ to set host EFLAGS from returned value.
// 3. Restore all GP registers (MOV does not affect EFLAGS).
//
// POPFD DEFINES new EFLAGS, so save_flags from a prior instruction is moot —
// we still wrap to protect the call sequence, but the final POPFQ sets the
// new flag state.
// ---------------------------------------------------------------------------
bool emit_handler_popfd(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                        const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                        GuestContext* /*ctx*/, bool /*save_flags*/) {
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);             // arg0 = ctx
    emit_call_abs(e, reinterpret_cast<void*>(popfd_helper));
    // EAX = popped EFLAGS value
    e.emit8(0x50);                      // PUSH RAX
    e.emit8(0x9D);                      // POPFQ  — sets host EFLAGS
    emit_load_all_gp(e);               // MOVs don't touch flags
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_string — MOVS/STOS/LODS/CMPS/SCAS (± REP/REPE/REPNE).
//
// 1. Capture host EFLAGS → R14 (for DF direction bit).
// 2. Save GP, call the appropriate C helper(ctx, eflags, elem_size, rep_mode).
// 3. CMPS/SCAS: helper returns new EFLAGS in EAX → save to R14.
//    MOVS/STOS/LODS: helper returns input EFLAGS unchanged → R14 is still good.
// 4. Load GP (ESI/EDI/ECX updated by helper), PUSH R14 + POPFQ to set flags.
// ---------------------------------------------------------------------------
bool emit_handler_string(Emitter& e, const ZydisDecodedInstruction& insn,
                         const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                         GuestContext* /*ctx*/, bool /*save_flags*/) {
    uint32_t elem_size;
    const void* helper;

    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: elem_size = 1; helper = (const void*)string_movs_helper; break;
    case ZYDIS_MNEMONIC_MOVSW: elem_size = 2; helper = (const void*)string_movs_helper; break;
    case ZYDIS_MNEMONIC_MOVSD: elem_size = 4; helper = (const void*)string_movs_helper; break;
    case ZYDIS_MNEMONIC_STOSB: elem_size = 1; helper = (const void*)string_stos_helper; break;
    case ZYDIS_MNEMONIC_STOSW: elem_size = 2; helper = (const void*)string_stos_helper; break;
    case ZYDIS_MNEMONIC_STOSD: elem_size = 4; helper = (const void*)string_stos_helper; break;
    case ZYDIS_MNEMONIC_LODSB: elem_size = 1; helper = (const void*)string_lods_helper; break;
    case ZYDIS_MNEMONIC_LODSW: elem_size = 2; helper = (const void*)string_lods_helper; break;
    case ZYDIS_MNEMONIC_LODSD: elem_size = 4; helper = (const void*)string_lods_helper; break;
    case ZYDIS_MNEMONIC_CMPSB: elem_size = 1; helper = (const void*)string_cmps_helper; break;
    case ZYDIS_MNEMONIC_CMPSW: elem_size = 2; helper = (const void*)string_cmps_helper; break;
    case ZYDIS_MNEMONIC_CMPSD: elem_size = 4; helper = (const void*)string_cmps_helper; break;
    case ZYDIS_MNEMONIC_SCASB: elem_size = 1; helper = (const void*)string_scas_helper; break;
    case ZYDIS_MNEMONIC_SCASW: elem_size = 2; helper = (const void*)string_scas_helper; break;
    case ZYDIS_MNEMONIC_SCASD: elem_size = 4; helper = (const void*)string_scas_helper; break;
    default: return false;
    }

    // Determine REP mode from instruction attributes
    uint32_t rep_mode = 0;
    if (insn.attributes & ZYDIS_ATTRIB_HAS_REP)   rep_mode = 1;
    if (insn.attributes & ZYDIS_ATTRIB_HAS_REPE)  rep_mode = 1;
    if (insn.attributes & ZYDIS_ATTRIB_HAS_REPNE) rep_mode = 2;

    // Capture EFLAGS (need DF for direction)
    e.emit8(0x9C);                      // PUSHFQ
    e.emit8(0x41); e.emit8(0x5E);      // POP R14

    // Save GP to ctx
    emit_save_all_gp(e);

    // Call helper(ctx, eflags, elem_size, rep_mode)
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);              // arg1 = R14D (captured EFLAGS)
    emit_ccall_arg2_imm(e, elem_size);
    emit_ccall_arg3_imm(e, rep_mode);
    emit_call_abs(e, helper);

    // All helpers return EFLAGS in EAX; save before load_all_gp clobbers EAX
    e.emit8(0x41); e.emit8(0x89); e.emit8(0xC6); // MOV R14D, EAX

    // Load updated GP (ESI/EDI/ECX/EAX modified by helper)
    emit_load_all_gp(e);

    // Set host EFLAGS from R14 (original or new comparison result)
    e.emit8(0x41); e.emit8(0x56);      // PUSH R14
    e.emit8(0x9D);                      // POPFQ

    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_movzx_mem — MOVZX r32, [m] (byte/word source)
// ---------------------------------------------------------------------------
bool emit_handler_movzx_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                            const ZydisDecodedOperand* ops, const uint8_t* raw,
                            GuestContext* /*ctx*/, bool save_flags) {
    if (insn.operand_count < 2) return false;
    if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;

    // Register-register form: verbatim copy (works natively in 64-bit mode)
    if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        e.copy(raw, insn.length);
        return true;
    }

    uint8_t dst_enc = 0;
    if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;

    unsigned src_bits = ops[1].size; // 8 or 16

    if (!emit_ea_to_r14(e, ops[1])) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);
    emit_fastmem_movzx(e, dst_enc, src_bits);
    uint8_t* done_site = emit_jmp_fwd(e);

    Emitter::patch_rel32(slow_site, e.cur());
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, dst_enc);
    emit_ccall_arg3_imm(e, src_bits / 8);
    emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
    emit_load_all_gp(e);

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_movsx_mem — MOVSX r32, [m] (byte/word source)
// ---------------------------------------------------------------------------
bool emit_handler_movsx_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                            const ZydisDecodedOperand* ops, const uint8_t* raw,
                            GuestContext* /*ctx*/, bool save_flags) {
    if (insn.operand_count < 2) return false;
    if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return false;

    // Register-register form: verbatim copy (works natively in 64-bit mode)
    if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        e.copy(raw, insn.length);
        return true;
    }

    uint8_t dst_enc = 0;
    if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;

    unsigned src_bits = ops[1].size; // 8 or 16

    if (!emit_ea_to_r14(e, ops[1])) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);
    emit_fastmem_movsx(e, dst_enc, src_bits);
    uint8_t* done_site = emit_jmp_fwd(e);

    Emitter::patch_rel32(slow_site, e.cur());
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, dst_enc);
    emit_ccall_arg3_imm(e, src_bits / 8);
    emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
    emit_load_all_gp(e);

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_fpu_mem — x87 instructions with memory operand forms.
// Register-only forms (FADD ST(i),ST(0) etc.) are verbatim-copied.
// Memory forms use the generic rewriter to access [R12+R14].
// ---------------------------------------------------------------------------
bool emit_handler_fpu_mem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* /*ctx*/, bool save_flags) {
    // Check if this instance actually has a memory operand.
    int mem_idx = -1;
    if (!TraceBuilder::has_mem_operand(ops, insn.operand_count, mem_idx)) {
        // Register-only form: verbatim copy (works natively in 64-bit mode)
        e.copy(raw, insn.length);
        return true;
    }

    // Memory form: compute EA, bounds check, rewrite for fastmem
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    if (save_flags) emit_save_flags(e);
    emit_cmp_r14_r15(e);
    uint8_t* slow_site = emit_jae_fwd(e);

    // Fast path: rewrite the instruction to use [R12+R14]
    if (!emit_rewrite_mem_to_fastmem(e, insn, raw)) {
        return false;
    }
    uint8_t* done_site = emit_jmp_fwd(e);

    // Slow path: MMIO not supported for FPU — UD2 trap (unreachable in practice)
    Emitter::patch_rel32(slow_site, e.cur());
    e.emit8(0x0F); e.emit8(0x0B); // UD2

    Emitter::patch_rel32(done_site, e.cur());
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// Conditional branch exit
// ---------------------------------------------------------------------------

void TraceBuilder::emit_cond_exit(Emitter& e,
                                   const ZydisDecodedInstruction& insn,
                                   const ZydisDecodedOperand* ops,
                                   uint32_t pc_after) {
    uint32_t taken_eip = pc_after;
    for (int i = 0; i < (int)insn.operand_count; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            ops[i].imm.is_relative) {
            taken_eip = pc_after + (uint32_t)(int32_t)ops[i].imm.value.s;
            break;
        }
    }
    uint8_t jcc = jcc_short_opcode(insn.mnemonic);
    if (!jcc) { emit_epilog_static(e, taken_eip); return; }
    emit_epilog_conditional(e, jcc, taken_eip, pc_after);
}

// ---------------------------------------------------------------------------
// Unconditional JMP exit
// ---------------------------------------------------------------------------

void TraceBuilder::emit_jmp_exit(Emitter& e,
                                  const ZydisDecodedInstruction& insn,
                                  const ZydisDecodedOperand* ops,
                                  uint32_t pc_after,
                                  GuestContext* /*ctx*/) {
    for (int i = 0; i < (int)insn.operand_count; ++i) {
        const auto& op = ops[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
            emit_epilog_static(e,
                pc_after + (uint32_t)(int32_t)op.imm.value.s);
            return;
        }
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc = 0;
            if (reg32_enc(op.reg.value, enc)) {
                emit_epilog_dynamic(e, enc);
                return;
            }
        }
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (emit_ea_to_r14(e, op)) {
                // Save guest regs (preserves all original values in ctx)
                emit_save_all_gp(e);
                // Call read_guest_mem32(ctx, R14D) → EAX = target
                emit_ccall_arg0_ctx(e);
                emit_ccall_arg1_pa(e);
                emit_call_abs(e, reinterpret_cast<void*>(read_guest_mem32));
                // EAX = target. Write directly to ctx->next_eip (offset 40).
                // MOV [R13+40], EAX : REX.B=0x41, 0x89, mod=01 reg=0 rm=5, disp8=40
                e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(40);
                // Reload guest regs from ctx (C call clobbered host regs)
                emit_load_all_gp(e);
                e.emit8(0xC3); // RET
                return;
            }
        }
    }
    fprintf(stderr, "[trace] unhandled JMP operand at %08X\n", pc_after);
    emit_epilog_static(e, pc_after);
}

// ---------------------------------------------------------------------------
// CALL near exit
// ---------------------------------------------------------------------------

void TraceBuilder::emit_call_exit(Emitter& e,
                                   const ZydisDecodedInstruction& insn,
                                   const ZydisDecodedOperand* ops,
                                   uint32_t pc_after,
                                   GuestContext* /*ctx*/) {
    for (int i = 0; i < (int)insn.operand_count; ++i) {
        const auto& op = ops[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
            uint32_t target = pc_after + (uint32_t)(int32_t)op.imm.value.s;
            emit_sub_ctx_esp(e, 4);
            emit_load_esp_to_r14(e);
            e.emit8(0xB9); e.emit32(pc_after); // MOV ECX, retaddr
            emit_fastmem_op(e, GP_ECX, 32, false);
            emit_epilog_static(e, target);
            return;
        }
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc = 0;
            if (reg32_enc(op.reg.value, enc)) {
                emit_sub_ctx_esp(e, 4);
                emit_load_esp_to_r14(e);
                e.emit8(0xB9); e.emit32(pc_after);
                emit_fastmem_op(e, GP_ECX, 32, false);
                emit_epilog_dynamic(e, enc);
                return;
            }
        }
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (emit_ea_to_r14(e, op)) {
                // Save guest regs (preserves all original values in ctx)
                emit_save_all_gp(e);
                // Call call_mem_helper(ctx, R14D, pc_after) → EAX = target
                emit_ccall_arg0_ctx(e);
                emit_ccall_arg1_pa(e);
                emit_ccall_arg2_imm(e, pc_after);
                emit_call_abs(e, reinterpret_cast<void*>(call_mem_helper));
                // EAX = target. Write to ctx->next_eip (offset 40).
                e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(40);
                // Reload guest regs from ctx (C call clobbered host regs)
                emit_load_all_gp(e);
                e.emit8(0xC3); // RET
                return;
            }
        }
    }
    fprintf(stderr, "[trace] unhandled CALL operand at %08X\n", pc_after);
    emit_epilog_static(e, pc_after);
}

// ---------------------------------------------------------------------------
// RET exit: save GP regs FIRST (preserving live EAX), then read return address
// from guest stack, write it directly to ctx->next_eip, restore EAX from ctx.
// ---------------------------------------------------------------------------

void TraceBuilder::emit_ret_exit(Emitter& e, GuestContext* /*ctx*/) {
    // Step 1: save all live guest GP regs (EAX = in-trace value, e.g. sum=55).
    emit_save_all_gp(e);

    // Step 2: R14D = ctx->esp (ESP was never in a host register; read from ctx).
    emit_load_esp_to_r14(e);

    // Step 3: fastmem check.
    emit_cmp_r14_r15(e);
    uint8_t* slow = emit_jae_fwd(e);

    // Step 4a (fast): EAX = [R12 + R14]  — return address from guest stack.
    emit_fastmem_op(e, GP_EAX, 32, true);
    uint8_t* done = emit_jmp_fwd(e);

    // Step 4b (slow / MMIO): call mmio_dispatch_read; result lands in ctx->gp[EAX].
    // Then reload EAX from ctx so both paths converge: EAX = return address.
    Emitter::patch_rel32(slow, e.cur());
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_pa(e);
    emit_ccall_arg2_imm(e, GP_EAX);
    emit_ccall_arg3_imm(e, 4);
    emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
    emit_load_gp(e, GP_EAX, gp_offset(GP_EAX));   // EAX = ctx->gp[EAX] (retaddr)

    Emitter::patch_rel32(done, e.cur());

    // Step 5: ctx->esp += 4.
    emit_add_ctx_esp(e, 4);

    // Step 6: ctx->next_eip = EAX  (direct write; bypass gpreg slot).
    //   MOV [R13+40], EAX  →  REX.B=0x41  MOV-store=0x89  ModRM mod=01 reg=0 rm=5=0x45  disp8=40
    e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(40);

    // Step 7: restore EAX from ctx->gp[0] (the value saved in Step 1).
    // The trampoline will re-save it, but now it will see the correct value.
    emit_load_gp(e, GP_EAX, gp_offset(GP_EAX));

    e.emit8(0xC3); // RET → trampoline
}

// ---------------------------------------------------------------------------
// Build entry point
// ---------------------------------------------------------------------------

static constexpr size_t MAX_TRACE_BYTES = 8192;
static constexpr size_t MAX_INSN_COUNT  = 512;

// Arithmetic flags clobbered by the inline CMP R14,R15 bounds check and
// SUB/ADD ctx->gp[ESP] in memory dispatch sequences.
static constexpr uint32_t ARITH_FLAGS =
    ZYDIS_CPUFLAG_CF | ZYDIS_CPUFLAG_PF | ZYDIS_CPUFLAG_AF |
    ZYDIS_CPUFLAG_ZF | ZYDIS_CPUFLAG_SF | ZYDIS_CPUFLAG_OF;

Trace* TraceBuilder::build(uint32_t            guest_eip,
                            const uint8_t*      ram,
                            uint32_t            ram_size,
                            CodeCache&          cc,
                            TraceArena&         arena,
                            const PageVersions& pv,
                            GuestContext*       ctx) {
    if (guest_eip >= ram_size) {
        fprintf(stderr, "[trace] EIP %08X outside RAM\n", guest_eip);
        return nullptr;
    }

    // =================================================================
    // Phase 1: Pre-scan — decode instructions, extract flag metadata.
    // =================================================================
    struct InsnMeta {
        uint8_t  length;
        uint32_t flags_tested;
        uint32_t flags_written;
        bool     has_dispatch;  // fastmem CMP may clobber flags
        bool     need_save;     // set by Phase 2
    };

    InsnMeta meta[MAX_INSN_COUNT];
    size_t   n_insns = 0;

    {
        const uint8_t* scan_pc       = ram + guest_eip;
        uint32_t       scan_guest_pc = guest_eip;
        const uint32_t scan_page     = guest_eip & ~0xFFFu;

        for (size_t i = 0; i < MAX_INSN_COUNT; ++i) {
            if (scan_guest_pc >= ram_size) break;

            ZydisDecodedInstruction insn;
            ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
            ZyanUSize avail = ram_size - scan_guest_pc;
            if (avail > 15) avail = 15;

            ZyanStatus st = ZydisDecoderDecodeFull(&decoder_, scan_pc, avail,
                                                   &insn, ops);
            if (!ZYAN_SUCCESS(st)) break;

            uint32_t tested = 0, written = 0;
            if (insn.cpu_flags) {
                tested  = insn.cpu_flags->tested;
                written = insn.cpu_flags->modified | insn.cpu_flags->set_0 |
                          insn.cpu_flags->set_1    | insn.cpu_flags->undefined;
            }

            InsnClassFlags icf = lookup_flags(insn.mnemonic);
            bool dispatch = (icf & ICF_HAS_DISPATCH) != 0;

            meta[n_insns++] = { insn.length, tested, written, dispatch, false };

            if (icf & (ICF_TERMINATOR | ICF_PRIVILEGED))
                break;

            scan_pc       += insn.length;
            scan_guest_pc += insn.length;
            if ((scan_guest_pc & ~0xFFFu) != scan_page) break;
        }
    }

    // =================================================================
    // Phase 2: Backward flag-liveness analysis.
    //
    // `live` tracks flags that are alive AFTER the current instruction.
    // At the trace exit no flags survive (next trace starts fresh).
    // For each instruction: live_before = (live_after & ~written) | tested.
    // A dispatch needs save/restore only if live_before has arithmetic flags.
    // =================================================================
    {
        uint32_t live = 0;
        for (int i = (int)n_insns - 1; i >= 0; --i) {
            uint32_t live_before =
                (live & ~meta[i].flags_written) | meta[i].flags_tested;
            meta[i].need_save =
                meta[i].has_dispatch && (live_before & ARITH_FLAGS) != 0;
            live = live_before;
        }
    }

    // =================================================================
    // Phase 3: Emit — re-decode and generate host code.
    // =================================================================
    uint8_t* emit_buf = cc.alloc(MAX_TRACE_BYTES);
    if (!emit_buf) { fprintf(stderr, "[trace] code cache full\n"); return nullptr; }

    Emitter  e(emit_buf, MAX_TRACE_BYTES);
    const uint8_t* pc        = ram + guest_eip;
    uint32_t       guest_pc  = guest_eip;
    const uint32_t page_base = guest_eip & ~0xFFFu;
    bool           done_flag = false;
    size_t         insn_idx  = 0;

    for (size_t n = 0; n < MAX_INSN_COUNT && !done_flag; ++n) {
        if (guest_pc >= ram_size) {
            emit_epilog_static(e, guest_pc);
            break;
        }

        ZydisDecodedInstruction insn;
        ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
        ZyanUSize avail = ram_size - guest_pc;
        if (avail > 15) avail = 15;

        ZyanStatus st = ZydisDecoderDecodeFull(&decoder_, pc, avail, &insn, ops);
        if (!ZYAN_SUCCESS(st)) {
            fprintf(stderr, "[trace] decode error at %08X (st=%08X)\n", guest_pc, st);
            emit_epilog_static(e, guest_pc);
            break;
        }

        uint32_t pc_after = guest_pc + insn.length;

        // Look up the instruction's class via O(1) dispatch table
        InsnClassFlags icf = lookup_flags(insn.mnemonic);

        // ---- Terminator ----
        if (icf & ICF_TERMINATOR) {
            done_flag = true;
            switch (insn.mnemonic) {
            case ZYDIS_MNEMONIC_RET:
                emit_ret_exit(e, ctx);
                break;
            case ZYDIS_MNEMONIC_IRETD:
                // IRET: pop EIP, CS, EFLAGS via C helper
                emit_save_all_gp(e);
                emit_ccall_arg0_ctx(e);
                emit_call_abs(e, reinterpret_cast<void*>(iret_helper));
                // next_eip and eflags set by helper; just RET to trampoline
                e.emit8(0xC3);
                break;
            case ZYDIS_MNEMONIC_CALL:
                emit_call_exit(e, insn, ops, pc_after, ctx);
                break;
            case ZYDIS_MNEMONIC_JMP:
                emit_jmp_exit(e, insn, ops, pc_after, ctx);
                break;
            case ZYDIS_MNEMONIC_LOOP:
            case ZYDIS_MNEMONIC_LOOPE:
            case ZYDIS_MNEMONIC_LOOPNE: {
                // Resolve target address from the relative immediate.
                uint32_t taken_eip = pc_after;
                for (int i = 0; i < (int)insn.operand_count; ++i) {
                    if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                        ops[i].imm.is_relative) {
                        taken_eip = pc_after +
                            (uint32_t)(int32_t)ops[i].imm.value.s;
                        break;
                    }
                }
                // LOOP: DEC ECX, jump if ECX != 0.
                // LOOPE: DEC ECX, jump if ECX != 0 AND ZF == 1.
                // LOOPNE: DEC ECX, jump if ECX != 0 AND ZF == 0.
                // All three do NOT modify EFLAGS per the ISA, but we are at a
                // trace boundary so corrupting flags is acceptable.
                // DEC ECX (ModRM form: 0xFF /1 → 0xFF 0xC9)
                e.emit8(0xFF); e.emit8(0xC9);
                if (insn.mnemonic == ZYDIS_MNEMONIC_LOOP) {
                    // TEST ECX, ECX (0x85 0xC9) — sets ZF if ECX==0
                    e.emit8(0x85); e.emit8(0xC9);
                    emit_epilog_conditional(e, 0x75 /*JNZ*/, taken_eip,
                                            pc_after);
                } else {
                    // LOOPE/LOOPNE: need to check both ECX!=0 and ZF.
                    // Emit static fallback for now (single-step).
                    emit_epilog_static(e, taken_eip);
                }
                break;
            }
            default: {
                uint8_t jcc = jcc_short_opcode(insn.mnemonic);
                if (jcc)
                    emit_cond_exit(e, insn, ops, pc_after);
                else {
                    fprintf(stderr, "[trace] unhandled terminator %d at %08X\n",
                            insn.mnemonic, guest_pc);
                    emit_epilog_static(e, guest_pc);
                }
                break;
            }
            }
            break;
        }

        // ---- Privileged ----
        if (icf & ICF_PRIVILEGED) {
            emit_save_all_gp(e);
            emit_write_next_eip_imm(e, guest_pc);
            emit_set_stop_reason(e, STOP_PRIVILEGED);
            e.emit8(0xC3); // RET — run loop calls handle_privileged()
            done_flag = true;
            goto advance;
        }

        // ---- MOV CRn / MOV DRn — privileged, but shares MNEMONIC_MOV ----
        if (insn.mnemonic == ZYDIS_MNEMONIC_MOV) {
            bool has_cr_dr = false;
            for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
                if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    (is_control_register(ops[i].reg.value) ||
                     is_debug_register(ops[i].reg.value))) {
                    has_cr_dr = true;
                    break;
                }
            }
            if (has_cr_dr) {
                emit_save_all_gp(e);
                emit_write_next_eip_imm(e, guest_pc);
                emit_set_stop_reason(e, STOP_PRIVILEGED);
                e.emit8(0xC3);
                done_flag = true;
                goto advance;
            }
        }

        // ---- Dispatch table handler ----
        {
            const InsnClass* ic = lookup_insn_class(insn.mnemonic);
            if (ic && ic->handler) {
                bool save = (insn_idx < n_insns) && meta[insn_idx].need_save;
                if (!ic->handler(e, insn, ops, pc, ctx, save)) {
                    fprintf(stderr, "[trace] unsupported insn mnem=%d at %08X\n",
                            insn.mnemonic, guest_pc);
                    emit_epilog_static(e, guest_pc);
                    done_flag = true;
                }
                goto advance;
            }

            // ---- No dispatch entry: clean copy (verbatim / re-encode) ----
            emit_clean_insn(e, insn, ops, pc);
        }

    advance:
        pc       += insn.length;
        guest_pc  = pc_after;
        ++insn_idx;

        if (!done_flag && (guest_pc & ~0xFFFu) != page_base) {
            emit_epilog_static(e, guest_pc);
            done_flag = true;
        }
    }

    if (!done_flag)
        emit_epilog_static(e, guest_pc);

    Trace* t = arena.alloc();
    if (!t) return nullptr;

    t->guest_eip = guest_eip;
    t->host_code = emit_buf;
    t->page_ver  = pv.get(guest_eip);
    t->valid     = true;
    return t;
}
