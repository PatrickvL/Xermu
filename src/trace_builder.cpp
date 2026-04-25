#include "trace_builder.hpp"
#include "executor.hpp"
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

// VA→PA page-table walk called from JIT code when paging is enabled.
// Returns PA on success, ~0u on fault (CR2 set to faulting VA).
uint32_t translate_va_jit(GuestContext* ctx, uint32_t va, uint32_t is_write) {
    auto* ram = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    uint32_t ram_size = GUEST_RAM_SIZE;
    uint32_t cr3 = ctx->cr3;

    uint32_t pdir_pa = cr3 & GUEST_PAGE_MASK;
    uint32_t pdi     = (va >> 22) & 0x3FF;
    uint32_t pti     = (va >> 12) & 0x3FF;

    // Read PDE
    uint32_t pde_pa = pdir_pa + pdi * 4;
    if (pde_pa + 4 > ram_size) goto fault;
    uint32_t pde;
    memcpy(&pde, ram + pde_pa, 4);
    if (!(pde & 1)) goto fault;

    // 4 MB page (PS=1)?
    if (pde & 0x80) {
        uint32_t pa = (pde & 0xFFC00000u) | (va & 0x003FFFFFu);
        if (is_write && !(pde & 2)) goto fault;
        if (!(pde & 0x20) || (is_write && !(pde & 0x40))) {
            pde |= 0x20;
            if (is_write) pde |= 0x40;
            memcpy(ram + pde_pa, &pde, 4);
        }
        return pa;
    }

    // 4 KB page table
    {
        uint32_t pt_pa = (pde & GUEST_PAGE_MASK) + pti * 4;
        if (pt_pa + 4 > ram_size) goto fault;
        uint32_t pte;
        memcpy(&pte, ram + pt_pa, 4);
        if (!(pte & 1)) goto fault;
        if (is_write && !(pte & 2)) goto fault;
        uint32_t pa = (pte & GUEST_PAGE_MASK) | (va & 0xFFF);
        bool need_pde_update = !(pde & 0x20);
        bool need_pte_update = !(pte & 0x20) || (is_write && !(pte & 0x40));
        if (need_pde_update) { pde |= 0x20; memcpy(ram + pde_pa, &pde, 4); }
        if (need_pte_update) {
            pte |= 0x20;
            if (is_write) pte |= 0x40;
            memcpy(ram + pt_pa, &pte, 4);
        }
        return pa;
    }

fault:
    ctx->cr2 = va;
    return ~0u;
}

// Translate a guest address to PA if paging is enabled, otherwise pass through.
// Used by C helpers that access guest memory by VA.
static inline uint32_t guest_translate(GuestContext* ctx, uint32_t addr, bool is_write) {
    if (ctx->cr0 & 0x80000000u)
        return translate_va_jit(ctx, addr, is_write ? 1 : 0);
    return addr;
}

// Generic guest memory read (handles paging + MMIO).
static inline uint32_t guest_read(GuestContext* ctx, uint32_t addr, uint32_t size) {
    uint32_t pa = guest_translate(ctx, addr, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        uint32_t v = 0;
        memcpy(&v, base + pa, size);
        return v;
    }
    return ctx->mmio ? ctx->mmio->read(pa, size) : 0xFFFFFFFFu;
}

// Generic guest memory write (handles paging + MMIO + SMC bump).
static inline void guest_write(GuestContext* ctx, uint32_t addr, uint32_t val, uint32_t size) {
    uint32_t pa = guest_translate(ctx, addr, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &val, size);
        return;
    }
    if (ctx->mmio) ctx->mmio->write(pa, val, size);
}

// PUSHFD helper: push EFLAGS onto the guest stack.
// Called with the captured EFLAGS value in `eflags_val`.
void pushfd_helper(GuestContext* ctx, uint32_t eflags_val) {
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    uint32_t pa = guest_translate(ctx, esp, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &eflags_val, 4);
    }
}

// POPFD helper: pop EFLAGS from the guest stack.
// Returns the 32-bit EFLAGS value.
uint32_t popfd_helper(GuestContext* ctx) {
    uint32_t esp = ctx->gp[GP_ESP];
    ctx->gp[GP_ESP] = esp + 4;
    uint32_t pa = guest_translate(ctx, esp, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        uint32_t v; memcpy(&v, base + pa, 4); return v;
    }
    return 0;
}

// Read a 32-bit value from guest physical memory (or MMIO).
// Used by JMP/CALL [mem] handlers.
uint32_t read_guest_mem32(GuestContext* ctx, uint32_t addr) {
    uint32_t pa = guest_translate(ctx, addr, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        uint32_t v; memcpy(&v, base + pa, 4); return v;
    }
    return ctx->mmio ? ctx->mmio->read(pa, 4) : 0xFFFF'FFFFu;
}

// Write a 32-bit value to guest physical memory (or MMIO).
// Used by PUSH [mem] and CALL [mem] handlers.
void write_guest_mem32(GuestContext* ctx, uint32_t addr, uint32_t val) {
    uint32_t pa = guest_translate(ctx, addr, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &val, 4);
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

// PUSH ESP helper: pushes pre-decrement ESP value onto the guest stack.
// Guest ESP is NOT in a host register, so the generic PUSH r32 path would
// incorrectly store the host RSP.  This helper reads old ESP from ctx,
// decrements, and writes the old value — matching real x86 PUSH ESP semantics.
void push_esp_helper(GuestContext* ctx) {
    uint32_t old_esp = ctx->gp[GP_ESP];
    uint32_t new_esp = old_esp - 4;
    ctx->gp[GP_ESP] = new_esp;
    write_guest_mem32(ctx, new_esp, old_esp);
}

// POP ESP helper: pops a 32-bit value from [old ESP] and sets ESP to that
// value.  On real x86, the +4 from POP is overwritten by the popped value.
void pop_esp_helper(GuestContext* ctx) {
    uint32_t old_esp = ctx->gp[GP_ESP];
    uint32_t val = read_guest_mem32(ctx, old_esp);
    ctx->gp[GP_ESP] = val;
}

// MOV ESP, [mem] helper: read 32-bit value from PA, store to ctx->gp[GP_ESP].
void mov_esp_from_mem(GuestContext* ctx, uint32_t pa) {
    ctx->gp[GP_ESP] = read_guest_mem32(ctx, pa);
}

// MOV [mem], ESP helper: write ctx->gp[GP_ESP] to PA.
void mov_esp_to_mem(GuestContext* ctx, uint32_t pa) {
    write_guest_mem32(ctx, pa, ctx->gp[GP_ESP]);
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

// ENTER helper: ENTER imm16, nesting_level.
// Level 0 (common): PUSH EBP; MOV EBP, ESP; SUB ESP, size.
// Higher levels push additional frame pointers.
void enter_helper(GuestContext* ctx, uint32_t alloc_size, uint32_t nesting) {
    nesting &= 0x1F; // mask to 0..31

    // Step 1: PUSH EBP
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    write_guest_mem32(ctx, esp, ctx->gp[GP_EBP]);
    uint32_t frame_ptr = esp; // value after the initial PUSH EBP

    // Step 2: higher nesting levels push previous frame pointers
    if (nesting > 0) {
        for (uint32_t i = 1; i < nesting; ++i) {
            ctx->gp[GP_EBP] -= 4;
            esp -= 4;
            write_guest_mem32(ctx, esp, read_guest_mem32(ctx, ctx->gp[GP_EBP]));
        }
        // Push the new frame pointer itself
        esp -= 4;
        write_guest_mem32(ctx, esp, frame_ptr);
    }

    // Step 3: MOV EBP, frame_ptr; SUB ESP, alloc_size
    ctx->gp[GP_EBP] = frame_ptr;
    ctx->gp[GP_ESP] = esp - alloc_size;
}

// XLATB helper: AL = byte at [EBX + zero_extend(AL)].
// Returns the byte value to store in AL.
uint32_t xlatb_helper(GuestContext* ctx) {
    uint32_t ea = ctx->gp[GP_EBX] + (ctx->gp[GP_EAX] & 0xFF);
    uint32_t pa = guest_translate(ctx, ea, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        return base[pa];
    }
    return ctx->mmio ? ctx->mmio->read(pa, 1) : 0xFF;
}

// IRETD helper: pop EIP, CS, EFLAGS from guest stack (12 bytes).
// Sets ctx->next_eip and ctx->eflags.  CS is ignored (flat model).
// Also restores virtual_if from the IF bit of the popped EFLAGS.
void iret_helper(GuestContext* ctx) {
    uint32_t esp = ctx->gp[GP_ESP];
    uint32_t new_eip    = read_guest_mem32(ctx, esp); esp += 4;
    /* uint32_t cs = */    read_guest_mem32(ctx, esp); esp += 4; // ignored
    uint32_t new_eflags = read_guest_mem32(ctx, esp); esp += 4;
    ctx->gp[GP_ESP] = esp;
    ctx->next_eip   = new_eip;
    // Preserve only safe flags (mask out VM, IOPL, VIF, VIP, RF):
    ctx->eflags = (new_eflags & 0x003F7FD5u) | 0x02u; // bit 1 always set
    // Restore interrupt enable from the IF bit.
    ctx->virtual_if = (new_eflags & 0x200u) != 0;
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
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        uint32_t val = guest_read(ctx, esi, elem_size);
        guest_write(ctx, edi, val, elem_size);
        esi += dir; edi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_stos_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t eax = ctx->gp[GP_EAX];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        guest_write(ctx, edi, eax, elem_size);
        edi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_lods_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& eax = ctx->gp[GP_EAX];
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& ecx = ctx->gp[GP_ECX];

    auto do_one = [&]() {
        uint32_t val = guest_read(ctx, esi, elem_size);
        switch (elem_size) {
        case 1: eax = (eax & 0xFFFFFF00u) | (val & 0xFF); break;
        case 2: eax = (eax & 0xFFFF0000u) | (val & 0xFFFF); break;
        case 4: eax = val; break;
        }
        esi += dir;
    };
    if (rep_mode == 0) do_one();
    else while (ecx > 0) { do_one(); ecx--; }
    return eflags;
}

uint32_t string_cmps_helper(GuestContext* ctx, uint32_t eflags,
                            uint32_t elem_size, uint32_t rep_mode) {
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t& esi = ctx->gp[GP_ESI];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];
    uint32_t rf = eflags;

    auto do_one = [&]() {
        uint32_t s = guest_read(ctx, esi, elem_size);
        uint32_t d = guest_read(ctx, edi, elem_size);
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
    int dir = (eflags & 0x400u) ? -(int)elem_size : (int)elem_size;
    uint32_t eax = ctx->gp[GP_EAX];
    uint32_t& edi = ctx->gp[GP_EDI];
    uint32_t& ecx = ctx->gp[GP_ECX];
    uint32_t rf = eflags;

    auto do_one = [&]() {
        uint32_t d = guest_read(ctx, edi, elem_size);
        uint32_t s = 0;
        switch (elem_size) {
        case 1: s = eax & 0xFFu;   break;
        case 2: s = eax & 0xFFFFu; break;
        case 4: s = eax;           break;
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
//
// Two modes based on e.slow_path:
//   false → emit bare fastmem op; VEH handles faults and sets bitmap bit
//   true  → emit slow-path MMIO call directly (bitmap was set on prior fault)
static bool emit_fastmem_dispatch(Emitter& e, const ZydisDecodedOperand& mem_op,
                                   uint8_t guest_enc, unsigned size_bits,
                                   bool is_load, bool save_flags) {
    if (!emit_ea_to_r14(e, mem_op)) return false;
    emit_paging_translate(e, /*is_write=*/!is_load);

    if (!e.slow_path) {
        // Fast path: single instruction; VEH intercepts faults and sets bitmap.
        e.add_mem_site(e.fault_eip);
        emit_fastmem_op(e, guest_enc, size_bits, is_load);
    } else {
        // Slow path: full MMIO dispatch call.
        e.add_mem_site(e.fault_eip);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, guest_enc);
        emit_ccall_arg3_imm(e, size_bits / 8);
        emit_call_abs(e, is_load
            ? reinterpret_cast<void*>(mmio_dispatch_read)
            : reinterpret_cast<void*>(mmio_dispatch_write));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
    }
    return true;
}

// Shared: emit inline fastmem dispatch for MOV [mem], imm32
static bool emit_fastmem_dispatch_store_imm(Emitter& e,
                                             const ZydisDecodedOperand& mem_op,
                                             uint32_t imm, unsigned size_bits,
                                             bool save_flags) {
    if (!emit_ea_to_r14(e, mem_op)) return false;
    emit_paging_translate(e, /*is_write=*/true);

    if (!e.slow_path) {
        // Fast path: single instruction; VEH intercepts faults and sets bitmap.
        e.add_mem_site(e.fault_eip);
        if (size_bits == 32)     emit_fastmem_store_imm32(e, imm);
        else if (size_bits == 16) emit_fastmem_store_imm16(e, (uint16_t)imm);
        else                     emit_fastmem_store_imm8(e, (uint8_t)imm);
    } else {
        // Slow path: full MMIO dispatch call.
        e.add_mem_site(e.fault_eip);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, imm);           // arg2 = value (not gp index)
        emit_ccall_arg3_imm(e, size_bits / 8);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write_imm));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
    }
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

    // Special case: LEA ESP, [...] → store R14D to ctx->gp[GP_ESP]
    if (dst_enc == GP_ESP) {
        emit_store_r14_to_esp(e);
        return true;
    }

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
        // reg-reg MOV: check for ESP involvement.
        // Guest ESP is NOT in a host register, so verbatim copy would read/write
        // host RSP.  Handle ESP cases via ctx->gp[GP_ESP] through R14.
        bool dst_esp = (insn.operand_count_visible >= 1 &&
                        ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        ops[0].reg.value == ZYDIS_REGISTER_ESP);
        bool src_esp = (insn.operand_count_visible >= 2 &&
                        ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        ops[1].reg.value == ZYDIS_REGISTER_ESP);
        // MOV ESP, imm32 → store immediate to ctx->gp[GP_ESP]
        if (dst_esp && insn.operand_count_visible >= 2 &&
            ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            uint32_t imm = (uint32_t)(int32_t)ops[1].imm.value.s;
            // MOV DWORD [R13+16], imm32: REX.B=0x41, C7, mod=01 /0 rm=5(R13), disp8, imm32
            e.emit8(0x41); e.emit8(0xC7);
            e.emit8(uint8_t(0x40u | (0u << 3) | 5u));
            e.emit8(gp_offset(GP_ESP));
            e.emit32(imm);
            return true;
        }
        if (dst_esp && !src_esp) {
            // MOV ESP, r32 → store source register to ctx->gp[GP_ESP]
            uint8_t src_enc = 0;
            if (!reg32_enc(ops[1].reg.value, src_enc)) return false;
            // MOV [R13+16], src_reg:  REX.B=0x41, 0x89, mod=01 reg=src rm=5(R13), disp8=16
            e.emit8(0x41); e.emit8(0x89);
            e.emit8(uint8_t(0x40u | (src_enc << 3) | 5u));
            e.emit8(gp_offset(GP_ESP));
            return true;
        }
        if (src_esp && !dst_esp) {
            // MOV r32, ESP → load ctx->gp[GP_ESP] into destination register
            uint8_t dst_enc = 0;
            if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;
            // MOV dst_reg, [R13+16]:  REX.B=0x41, 0x8B, mod=01 reg=dst rm=5(R13), disp8=16
            e.emit8(0x41); e.emit8(0x8B);
            e.emit8(uint8_t(0x40u | (dst_enc << 3) | 5u));
            e.emit8(gp_offset(GP_ESP));
            return true;
        }
        if (dst_esp && src_esp) {
            return true; // MOV ESP, ESP → no-op
        }
        // No ESP involvement: verbatim copy
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

    // ESP special case: guest ESP is not in a host register.
    // MOV ESP, [mem] → load from guest memory, store to ctx->gp[GP_ESP].
    // MOV [mem], ESP → read ctx->gp[GP_ESP], store to guest memory.
    if (guest_enc == GP_ESP) {
        if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
        emit_paging_translate(e, /*is_write=*/!is_load);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        if (is_load) {
            // MOV ESP, [mem]: read from PA, store to ctx->gp[GP_ESP]
            emit_call_abs(e, reinterpret_cast<void*>(mov_esp_from_mem));
        } else {
            // MOV [mem], ESP: read ctx->gp[GP_ESP], write to PA
            emit_call_abs(e, reinterpret_cast<void*>(mov_esp_to_mem));
        }
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
        return true;
    }

    return emit_fastmem_dispatch(e, ops[mem_idx], guest_enc,
                                  insn.operand_width, is_load, save_flags);
}

// ---------------------------------------------------------------------------
// emit_alu_esp_reg — Handle any reg-reg / reg-imm ALU instruction where one
// operand is ESP.  Since guest ESP is not in a host register, we load it from
// ctx into R14D, re-encode the instruction with R14D substituted for ESP,
// then store R14D back to ctx->gp[GP_ESP] if ESP was the destination.
//
// Covers: ADD/SUB/AND/OR/XOR/CMP/TEST ESP,imm; ADD/SUB ESP,r32; XCHG ESP,r32;
//         INC ESP; DEC ESP; NOT ESP; NEG ESP; etc.
//
// Strategy: scan the raw instruction bytes, find the ModRM byte, and replace
// the ESP encoding (4) with R14 encoding (6) + REX.B or REX.R.  R14 is our
// scratch register and no other JIT code uses it inside a trace body.
// ---------------------------------------------------------------------------
static bool emit_alu_esp_reg(Emitter& e,
                              const ZydisDecodedInstruction& insn,
                              const ZydisDecodedOperand* ops,
                              const uint8_t* raw) {
    // Determine if ESP is the destination (written) so we know to store back.
    bool esp_is_dst = false;
    for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[i].reg.value == ZYDIS_REGISTER_ESP &&
            (ops[i].actions & ZYDIS_OPERAND_ACTION_WRITE))
            esp_is_dst = true;
    }

    // Load guest ESP from ctx into R14D.
    emit_load_esp_to_r14(e);

    // Re-encode the instruction replacing ESP (encoding 4) with R14 (encoding 6).
    // For mod=11 instructions, the raw layout is:
    //   [prefixes...] [opcode bytes...] [ModRM] [SIB?] [disp?] [imm?]
    // We need to find the ModRM byte and patch it.
    // Also need to add REX bits for the R14 extended register.

    // Use Zydis raw info to locate the ModRM byte.
    // The ModRM byte offset is after prefixes + opcodes.
    // For short-form INC/DEC (0x40-0x47, 0x48-0x4F in 32-bit), there is no
    // ModRM — the register is in the opcode itself.

    if (insn.mnemonic == ZYDIS_MNEMONIC_INC &&
        ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        ops[0].reg.value == ZYDIS_REGISTER_ESP) {
        // INC R14D: REX.B=0x41, 0xFF, ModRM=C0|0<<3|6 = 0xC6
        e.emit8(0x41); e.emit8(0xFF); e.emit8(0xC6);
        emit_store_r14_to_esp(e);
        return true;
    }
    if (insn.mnemonic == ZYDIS_MNEMONIC_DEC &&
        ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        ops[0].reg.value == ZYDIS_REGISTER_ESP) {
        // DEC R14D: REX.B=0x41, 0xFF, ModRM=C0|1<<3|6 = 0xCE
        e.emit8(0x41); e.emit8(0xFF); e.emit8(0xCE);
        emit_store_r14_to_esp(e);
        return true;
    }

    // For instructions with a ModRM byte (mod=11), we re-encode with R14.
    if (!insn.raw.modrm.offset) return false; // no ModRM → unsupported

    uint8_t modrm = raw[insn.raw.modrm.offset];
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t reg = (modrm >> 3) & 7;
    uint8_t rm  = modrm & 7;

    if (mod != 3) return false; // not reg-reg form — shouldn't happen here

    // Determine which field(s) have ESP (encoding 4) and need R14 (encoding 6).
    uint8_t rex = 0x40; // base REX prefix (may not be needed if no extended regs)
    uint8_t new_reg = reg;
    uint8_t new_rm = rm;

    if (rm == 4) {
        new_rm = 6;     // R14 & 7
        rex |= 0x01;    // REX.B for extended rm
    }
    if (reg == 4) {
        new_reg = 6;    // R14 & 7
        rex |= 0x04;    // REX.R for extended reg
    }

    uint8_t new_modrm = uint8_t((3u << 6) | (new_reg << 3) | new_rm);

    // Re-emit the instruction: prefixes, REX, opcode(s), ModRM, immediates.
    // The raw bytes before modrm_off contain legacy prefixes + opcode byte(s).
    // REX must come after legacy prefixes but before the opcode.
    int modrm_off = (int)insn.raw.modrm.offset;

    // Determine where the opcode starts.  The opcode map tells us:
    //   DEFAULT: 1 opcode byte right before ModRM
    //   0F:      2 bytes (0x0F + opcode) right before ModRM
    //   0F38/3A: 3 bytes (0x0F + 0x38/0x3A + opcode) right before ModRM
    int opc_len = 1; // default
    if (insn.opcode_map == ZYDIS_OPCODE_MAP_0F)   opc_len = 2;
    if (insn.opcode_map == ZYDIS_OPCODE_MAP_0F38)  opc_len = 3;
    if (insn.opcode_map == ZYDIS_OPCODE_MAP_0F3A)  opc_len = 3;
    int opc_start = modrm_off - opc_len;

    // 1. Copy legacy prefix bytes (everything before the opcode).
    for (int i = 0; i < opc_start; ++i) {
        uint8_t b = raw[i];
        if (b >= 0x40 && b <= 0x4F) continue; // skip 32-bit REX-range bytes
        e.emit8(b);
    }
    // 2. Emit REX (only if we need extended registers)
    if (rex != 0x40) e.emit8(rex);
    // 3. Emit opcode byte(s)
    for (int i = opc_start; i < modrm_off; ++i)
        e.emit8(raw[i]);
    // 4. Emit the patched ModRM byte
    e.emit8(new_modrm);
    // 5. Copy everything after ModRM (SIB, displacement, immediates)
    for (int i = modrm_off + 1; i < (int)insn.length; ++i)
        e.emit8(raw[i]);

    if (esp_is_dst)
        emit_store_r14_to_esp(e);

    return true;
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
        // No mem operand → clean copy, unless ESP is involved.
        // Guest ESP is not in a host register; intercept ESP operands.
        bool has_esp = false;
        for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
            if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ops[i].reg.value == ZYDIS_REGISTER_ESP) {
                has_esp = true;
                break;
            }
        }
        if (has_esp) {
            // Load guest ESP into R14, execute instruction with R14
            // substituted for ESP, then store R14 back if ESP was written.
            return emit_alu_esp_reg(e, insn, ops, raw);
        }
        return emit_clean_insn(e, insn, ops, raw);
    }

    // Memory form: compute EA, bounds check, rewrite for fastmem
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    emit_paging_translate(e, /*is_write=*/mem_idx == 0);

    // ESP in reg field: guest ESP is not in a host register.  ModRM reg=4
    // would encode host RSP.  Load guest ESP into R8D, rewrite with R8.
    // IMPORTANT: only when reg field actually encodes a register, not an
    // opcode extension (e.g. AND [mem],imm has reg=4 as opcode ext, not ESP).
    bool esp_in_reg = false;
    bool esp_is_dst_mem = false;
    for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[i].reg.value == ZYDIS_REGISTER_ESP) {
            esp_in_reg = true;
            if (ops[i].actions & ZYDIS_OPERAND_ACTION_WRITE)
                esp_is_dst_mem = true;
            break;
        }
    }
    if (esp_in_reg) emit_load_esp_to_r8(e);

    if (!e.slow_path) {
        // Fast path: rewrite the instruction to use [R12+R14]; VEH handles faults.
        if (save_flags) emit_save_flags(e);
        e.add_mem_site(e.fault_eip);
        if (!emit_rewrite_mem_to_fastmem(e, insn, raw, esp_in_reg)) return false;
        if (esp_is_dst_mem) emit_store_r8_to_esp(e);
        if (save_flags) emit_restore_flags(e);
    } else {
        // Slow path: MMIO not supported for ALU-mem — UD2 trap
        e.add_mem_site(e.fault_eip);
        e.emit8(0x0F); e.emit8(0x0B); // UD2
    }
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_flagmem — SETcc [mem] / CMOVcc r, [mem]
//
// These instructions READ EFLAGS *and* access memory.  The SUB/ADD RSP
// sequences in save_flags/restore_flags would clobber guest flags, so we
// must save/restore EFLAGS around the memory dispatch:
//
//   EA → R14
//   PUSHFQ                 ; save guest EFLAGS
//   <fastmem or slow path> ; memory access (may clobber flags)
//   POPFQ                  ; restore EFLAGS before SETcc/CMOVcc
//   <rewritten insn>
// ---------------------------------------------------------------------------
bool emit_handler_flagmem(Emitter& e, const ZydisDecodedInstruction& insn,
                          const ZydisDecodedOperand* ops, const uint8_t* raw,
                          GuestContext* /*ctx*/, bool /*save_flags*/) {
    int mem_idx = -1;
    if (!TraceBuilder::has_mem_operand(ops, insn.operand_count, mem_idx)) {
        // reg-reg form: SETcc r8, CMOVcc r,r
        // For CMOVcc with ESP operand, route through emit_alu_esp_reg
        // which correctly substitutes R14 for ESP.
        if (insn.mnemonic >= ZYDIS_MNEMONIC_CMOVB &&
            insn.mnemonic <= ZYDIS_MNEMONIC_CMOVZ) {
            for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
                if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    ops[i].reg.value == ZYDIS_REGISTER_ESP)
                    return emit_alu_esp_reg(e, insn, ops, raw);
            }
        }
        e.copy(raw, insn.length);
        return true;
    }

    // Memory form — flag-preserving bounds check
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    emit_paging_translate(e, /*is_write=*/mem_idx == 0); // SETcc writes, CMOVcc reads

    // ESP in reg field: CMOVcc ESP, [mem]. Load guest ESP into R8D.
    // Use operand scan, not raw reg field (which may be an opcode extension).
    bool esp_in_reg = false;
    for (int i = 0; i < (int)insn.operand_count_visible; ++i) {
        if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[i].reg.value == ZYDIS_REGISTER_ESP) {
            esp_in_reg = true;
            break;
        }
    }
    if (esp_in_reg) emit_load_esp_to_r8(e);

    if (!e.slow_path) {
        // Fast path: rewrite the instruction to use [R12+R14]; VEH handles faults.
        e.add_mem_site(e.fault_eip);
        if (!emit_rewrite_mem_to_fastmem(e, insn, raw, esp_in_reg)) return false;
        if (esp_in_reg) emit_store_r8_to_esp(e);
    } else {
        // Slow path: MMIO not supported for SETcc/CMOVcc — UD2 trap
        e.add_mem_site(e.fault_eip);
        e.emit8(0x0F); e.emit8(0x0B); // UD2
    }
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_enter — ENTER imm16, imm8 (create stack frame via C helper)
// ---------------------------------------------------------------------------
bool emit_handler_enter(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, const uint8_t* /*raw*/,
                        GuestContext* /*ctx*/, bool save_flags) {
    // ops[0] = imm16 (allocation size), ops[1] = imm8 (nesting level)
    if (insn.operand_count_visible < 2) return false;
    uint32_t alloc_size = (uint32_t)(uint16_t)ops[0].imm.value.u;
    uint32_t nesting    = (uint32_t)(uint8_t)ops[1].imm.value.u;

    if (save_flags) emit_save_flags(e);
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_ccall_arg1_imm(e, alloc_size);
    emit_ccall_arg2_imm(e, nesting);
    emit_call_abs(e, reinterpret_cast<void*>(enter_helper));
    emit_load_all_gp(e);
    if (save_flags) emit_restore_flags(e);
    return true;
}

// ---------------------------------------------------------------------------
// emit_handler_xlatb — XLATB: AL = [EBX + zero_extend(AL)]
// Calls C helper which reads the byte and returns it; we then store to AL.
// ---------------------------------------------------------------------------
bool emit_handler_xlatb(Emitter& e, const ZydisDecodedInstruction& /*insn*/,
                        const ZydisDecodedOperand* /*ops*/, const uint8_t* /*raw*/,
                        GuestContext* /*ctx*/, bool save_flags) {
    if (save_flags) emit_save_flags(e);
    emit_save_all_gp(e);
    emit_ccall_arg0_ctx(e);
    emit_call_abs(e, reinterpret_cast<void*>(xlatb_helper));
    // EAX = byte value returned by helper.
    // We need to merge into just AL of the saved EAX in ctx.
    // MOV byte [R13 + gp_offset(EAX)], AL
    //   REX.B=0x41, opcode=0x88, ModRM: mod=01, reg=0(AL), rm=5(R13), disp8=0
    e.emit8(0x41); e.emit8(0x88); e.emit8(0x45); e.emit8(gp_offset(GP_EAX));
    emit_load_all_gp(e);
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

    // Memory form: compute EA, rewrite for fastmem
    if (!emit_ea_to_r14(e, ops[mem_idx])) return false;
    emit_paging_translate(e, /*is_write=*/false);

    if (!e.slow_path) {
        // Fast path: rewrite the instruction to use [R12+R14]; VEH handles faults.
        if (save_flags) emit_save_flags(e);
        e.add_mem_site(e.fault_eip);
        if (!emit_rewrite_mem_to_fastmem(e, insn, raw)) return false;
        if (save_flags) emit_restore_flags(e);
    } else {
        // Slow path: UD2
        e.add_mem_site(e.fault_eip);
        e.emit8(0x0F); e.emit8(0x0B); // UD2
    }
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
        emit_sub_ctx_esp(e, 4);
        emit_load_esp_to_r14(e);
        emit_paging_translate(e, /*is_write=*/true);

        if (!e.slow_path) {
            e.add_mem_site(e.fault_eip);
            emit_fastmem_store_imm32(e, imm);
        } else {
            e.add_mem_site(e.fault_eip);
            if (save_flags) emit_save_flags(e);
            emit_save_all_gp(e);
            emit_ccall_arg0_ctx(e);
            emit_ccall_arg1_pa(e);
            emit_ccall_arg2_imm(e, imm);
            emit_ccall_arg3_imm(e, 4);
            emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write_imm));
            emit_load_all_gp(e);
            if (save_flags) emit_restore_flags(e);
        }
        return true;
    }

    // PUSH r32
    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        uint8_t reg_enc = 0;
        if (!reg32_enc(ops[0].reg.value, reg_enc)) return false;

        // PUSH ESP special case: ESP is not in a host register, so the
        // generic fastmem_op path would incorrectly store host RSP.
        // Delegate to a C helper that reads/writes ctx->gp[GP_ESP].
        if (reg_enc == GP_ESP) {
            if (save_flags) emit_save_flags(e);
            emit_save_all_gp(e);
            emit_ccall_arg0_ctx(e);
            emit_call_abs(e, reinterpret_cast<void*>(push_esp_helper));
            emit_load_all_gp(e);
            if (save_flags) emit_restore_flags(e);
            return true;
        }

        emit_sub_ctx_esp(e, 4);
        emit_load_esp_to_r14(e);
        emit_paging_translate(e, /*is_write=*/true);

        if (!e.slow_path) {
            e.add_mem_site(e.fault_eip);
            emit_fastmem_op(e, reg_enc, 32, false);
        } else {
            e.add_mem_site(e.fault_eip);
            if (save_flags) emit_save_flags(e);
            emit_save_all_gp(e);
            emit_ccall_arg0_ctx(e);
            emit_ccall_arg1_pa(e);
            emit_ccall_arg2_imm(e, reg_enc);
            emit_ccall_arg3_imm(e, 4);
            emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_write));
            emit_load_all_gp(e);
            if (save_flags) emit_restore_flags(e);
        }
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

        // POP ESP special case: ESP is not in a host register, so the
        // generic fastmem_op path would incorrectly load into host RSP.
        // On real x86, POP ESP sets ESP = [old ESP] (the +4 is overwritten).
        if (reg_enc == GP_ESP) {
            if (save_flags) emit_save_flags(e);
            emit_save_all_gp(e);
            emit_ccall_arg0_ctx(e);
            emit_call_abs(e, reinterpret_cast<void*>(pop_esp_helper));
            emit_load_all_gp(e);
            if (save_flags) emit_restore_flags(e);
            return true;
        }

        emit_load_esp_to_r14(e);
        emit_paging_translate(e, /*is_write=*/false);

        if (!e.slow_path) {
            e.add_mem_site(e.fault_eip);
            emit_fastmem_op(e, reg_enc, 32, true);
        } else {
            e.add_mem_site(e.fault_eip);
            if (save_flags) emit_save_flags(e);
            emit_save_all_gp(e);
            emit_ccall_arg0_ctx(e);
            emit_ccall_arg1_pa(e);
            emit_ccall_arg2_imm(e, reg_enc);
            emit_ccall_arg3_imm(e, 4);
            emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
            emit_load_all_gp(e);
            if (save_flags) emit_restore_flags(e);
        }
        emit_add_ctx_esp(e, 4);
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
// emit_handler_push_imm — PUSH imm routes through the general PUSH handler.
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
    emit_load_esp_to_r14(e);
    emit_paging_translate(e, /*is_write=*/false);

    if (!e.slow_path) {
        e.add_mem_site(e.fault_eip);
        emit_fastmem_op(e, GP_EBP, 32, true);
    } else {
        e.add_mem_site(e.fault_eip);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, GP_EBP);
        emit_ccall_arg3_imm(e, 4);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
    }
    emit_add_ctx_esp(e, 4);
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
    e.emit8(0x9C);                     // PUSHFQ
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
    e.emit8(0x9D);                     // POPFQ

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
    // except MOVZX ESP, r8 which would corrupt host RSP.
    if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        if (ops[0].reg.value == ZYDIS_REGISTER_ESP)
            return emit_alu_esp_reg(e, insn, ops, raw);
        e.copy(raw, insn.length);
        return true;
    }

    uint8_t dst_enc = 0;
    if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;

    unsigned src_bits = ops[1].size; // 8 or 16
    bool esp_dst = (dst_enc == GP_ESP);

    if (!emit_ea_to_r14(e, ops[1])) return false;
    emit_paging_translate(e, /*is_write=*/false);

    if (!e.slow_path) {
        e.add_mem_site(e.fault_eip);
        emit_fastmem_movzx(e, dst_enc, src_bits, esp_dst);
        if (esp_dst) emit_store_r8_to_esp(e);
    } else {
        e.add_mem_site(e.fault_eip);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, dst_enc);
        emit_ccall_arg3_imm(e, src_bits / 8);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
    }
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
    // except MOVSX ESP, r8 which would corrupt host RSP.
    if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        if (ops[0].reg.value == ZYDIS_REGISTER_ESP)
            return emit_alu_esp_reg(e, insn, ops, raw);
        e.copy(raw, insn.length);
        return true;
    }

    uint8_t dst_enc = 0;
    if (!reg32_enc(ops[0].reg.value, dst_enc)) return false;

    unsigned src_bits = ops[1].size; // 8 or 16
    bool esp_dst = (dst_enc == GP_ESP);

    if (!emit_ea_to_r14(e, ops[1])) return false;
    emit_paging_translate(e, /*is_write=*/false);

    if (!e.slow_path) {
        e.add_mem_site(e.fault_eip);
        emit_fastmem_movsx(e, dst_enc, src_bits, esp_dst);
        if (esp_dst) emit_store_r8_to_esp(e);
    } else {
        e.add_mem_site(e.fault_eip);
        if (save_flags) emit_save_flags(e);
        emit_save_all_gp(e);
        emit_ccall_arg0_ctx(e);
        emit_ccall_arg1_pa(e);
        emit_ccall_arg2_imm(e, dst_enc);
        emit_ccall_arg3_imm(e, src_bits / 8);
        emit_call_abs(e, reinterpret_cast<void*>(mmio_dispatch_read));
        emit_load_all_gp(e);
        if (save_flags) emit_restore_flags(e);
    }
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
    emit_paging_translate(e, /*is_write=*/mem_idx == 0);

    if (!e.slow_path) {
        // Fast path: rewrite the instruction to use [R12+R14]; VEH handles faults.
        if (save_flags) emit_save_flags(e);
        e.add_mem_site(e.fault_eip);
        if (!emit_rewrite_mem_to_fastmem(e, insn, raw)) {
            return false;
        }
        if (save_flags) emit_restore_flags(e);
    } else {
        // Slow path: MMIO not supported for FPU — UD2 trap (unreachable in practice)
        e.add_mem_site(e.fault_eip);
        e.emit8(0x0F); e.emit8(0x0B); // UD2
    }
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
                // EAX = target. Write directly to ctx->next_eip.
                // MOV [R13+next_eip], EAX : REX.B=0x41, 0x89, mod=01 reg=0 rm=5
                e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(CTX_NEXT_EIP);
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
            emit_paging_translate(e, /*is_write=*/true);
            e.add_mem_site(e.fault_eip);
            emit_fastmem_store_imm32(e, pc_after);
            emit_epilog_static(e, target);  // ends with RET
            return;
        }
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc = 0;
            if (reg32_enc(op.reg.value, enc)) {
                emit_sub_ctx_esp(e, 4);
                emit_load_esp_to_r14(e);
                emit_paging_translate(e, /*is_write=*/true);
                e.add_mem_site(e.fault_eip);
                emit_fastmem_store_imm32(e, pc_after);
                emit_epilog_dynamic(e, enc);  // ends with RET
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
                // EAX = target. Write to ctx->next_eip.
                e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(CTX_NEXT_EIP);
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

    // Step 3: fastmem load.
    emit_paging_translate(e, /*is_write=*/false);
    // EAX = [R12 + R14]  — return address from guest stack.
    // VEH handles any fault (stack in unmapped page — shouldn't happen in practice).
    e.add_mem_site(e.fault_eip);
    emit_fastmem_op(e, GP_EAX, 32, true);

    // Step 4: ctx->esp += 4.
    emit_add_ctx_esp(e, 4);

    // Step 6: ctx->next_eip = EAX  (direct write; bypass gpreg slot).
    //   MOV [R13+next_eip], EAX  →  REX.B=0x41  MOV-store=0x89  ModRM mod=01 reg=0 rm=5=0x45
    e.emit8(0x41); e.emit8(0x89); e.emit8(0x45); e.emit8(CTX_NEXT_EIP);

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

// Arithmetic flags clobbered by the inline SUB/ADD ctx->gp[ESP] in
// PUSH/POP sequences and by slow-path MMIO CALL sequences.
static constexpr uint32_t ARITH_FLAGS =
    ZYDIS_CPUFLAG_CF | ZYDIS_CPUFLAG_PF | ZYDIS_CPUFLAG_AF |
    ZYDIS_CPUFLAG_ZF | ZYDIS_CPUFLAG_SF | ZYDIS_CPUFLAG_OF;

Trace* TraceBuilder::build(uint32_t            guest_eip,
                            const uint8_t*      ram,
                            CodeCache&          cc,
                            TraceArena&         arena,
                            GuestContext*       ctx,
                            const FaultBitmaps* fb) {
    constexpr uint32_t ram_size = GUEST_RAM_SIZE;
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
            // In the 4GB fastmem model, bare fastmem ops don't clobber flags.
            // Only slow-path CALL sequences (bitmap-set instructions) do.
            bool dispatch = (icf & ICF_HAS_DISPATCH) != 0 &&
                            fb && fb->test(scan_guest_pc);

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
    e.paging = (ctx->cr0 & 0x80000000u) != 0;  // CR0.PG at build time
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
        e.fault_eip = guest_pc; // for paging fault exit stubs
        // Set slow_path flag: if the bitmap says this guest PA faulted before,
        // the handler will emit a full MMIO CALL instead of a bare fastmem op.
        e.slow_path = fb && fb->test(guest_pc);

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
                // Restore guest EFLAGS into host RFLAGS from ctx->eflags.
                // MOV R14D, [R13+eflags]  (REX=0x45, MOV r,r/m, mod=01 reg=6 rm=5)
                e.emit8(0x45); e.emit8(0x8B);
                e.emit8(uint8_t(0x40u | (6u << 3) | 5u));
                e.emit8(CTX_EFLAGS);
                e.emit8(0x41); e.emit8(0x56);  // PUSH R14
                e.emit8(0x9D);                  // POPFQ — set host EFLAGS
                // Reload GP regs from ctx (C call clobbered EAX/ECX/EDX).
                emit_load_all_gp(e);
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
                if (insn.mnemonic == ZYDIS_MNEMONIC_LOOP) {
                    // DEC ECX (ModRM form: 0xFF /1 → 0xFF 0xC9)
                    e.emit8(0xFF); e.emit8(0xC9);
                    // TEST ECX, ECX (0x85 0xC9) — sets ZF if ECX==0
                    e.emit8(0x85); e.emit8(0xC9);
                    emit_epilog_conditional(e, 0x75 /*JNZ*/, taken_eip,
                                            pc_after);
                } else if (insn.mnemonic == ZYDIS_MNEMONIC_LOOPE) {
                    // Check original ZF before DEC clobbers it.
                    // ZF==0 → not taken (skip to DEC + fallthrough).
                    e.emit8(0x0F); e.emit8(0x85);  // JNZ rel32
                    uint8_t* zf_fail = e.reserve_rel32();
                    e.emit8(0xFF); e.emit8(0xC9);  // DEC ECX
                    e.emit8(0x85); e.emit8(0xC9);  // TEST ECX, ECX
                    emit_epilog_conditional(e, 0x75 /*JNZ*/, taken_eip,
                                            pc_after);
                    // ZF==0 landing: still DEC ECX, then fallthrough.
                    Emitter::patch_rel32(zf_fail, e.cur());
                    e.emit8(0xFF); e.emit8(0xC9);  // DEC ECX
                    emit_epilog_static(e, pc_after);
                } else {
                    // LOOPNE: check original ZF before DEC clobbers it.
                    // ZF==1 → not taken (skip to DEC + fallthrough).
                    e.emit8(0x0F); e.emit8(0x84);  // JZ rel32
                    uint8_t* zf_fail = e.reserve_rel32();
                    e.emit8(0xFF); e.emit8(0xC9);  // DEC ECX
                    e.emit8(0x85); e.emit8(0xC9);  // TEST ECX, ECX
                    emit_epilog_conditional(e, 0x75 /*JNZ*/, taken_eip,
                                            pc_after);
                    // ZF==1 landing: still DEC ECX, then fallthrough.
                    Emitter::patch_rel32(zf_fail, e.cur());
                    e.emit8(0xFF); e.emit8(0xC9);  // DEC ECX
                    emit_epilog_static(e, pc_after);
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
            emit_save_eflags(e);
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
                emit_save_eflags(e);
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
    t->code_pa   = guest_eip;
    t->host_code = emit_buf;
    t->host_size = (uint32_t)e.pos;
    t->valid     = true;

    // Transfer pending link slots from the emitter to the trace.
    t->num_links = 0;
    {
        int start = (e.num_pending_links > Trace::MAX_LINKS)
                  ? e.num_pending_links - Trace::MAX_LINKS : 0;
        for (int i = start; i < e.num_pending_links; ++i)
            t->add_link(e.pending_links[i].patch_site,
                         e.pending_links[i].target_eip);
    }

    // Transfer memory-op site table from emitter to trace (for VEH lookup).
    t->num_mem_sites = 0;
    for (int i = 0; i < e.num_mem_sites; ++i)
        t->add_mem_site(e.mem_sites[i].host_offset, e.mem_sites[i].guest_eip);

    return t;
}
