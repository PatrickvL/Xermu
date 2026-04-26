// ---------------------------------------------------------------------------
// jit_helpers.cpp — C-linkage helper functions called from JIT-compiled code.
//
// These are slow-path helpers invoked when the JIT needs to perform operations
// that can't be done inline:
//   - MMIO dispatch (read/write/write_imm)
//   - VA→PA page-table walk
//   - Guest memory read/write (paging + MMIO aware)
//   - PUSH/POP/CALL helpers for ESP (not in a host register)
//   - High-byte register MOV helpers (AH/CH/DH/BH)
//   - PUSHFD/POPFD, PUSHAD/POPAD, ENTER, XLATB
//   - String instructions (MOVS/STOS/LODS/CMPS/SCAS)
//   - IRETD
//
// All functions use extern "C" linkage so JIT code can CALL them directly.
// GP registers are saved to GuestContext before each call.
// ---------------------------------------------------------------------------

#include "trace_builder.hpp"
#include "executor.hpp"
#include <cstring>

extern "C" {

void mmio_dispatch_read(GuestContext* ctx, uint32_t pa,
                        uint32_t dst_gp_idx, uint32_t size_bytes) {
    uint32_t val;
    if (pa < GUEST_RAM_SIZE) {
        // Direct RAM access (site was patched on prior MMIO fault for this EIP,
        // but this execution's PA happens to land in RAM).
        val = 0;
        memcpy(&val, reinterpret_cast<uint8_t*>(ctx->fastmem_base) + pa, size_bytes);
    } else if (ctx->mmio) {
        val = ctx->mmio->read(pa, size_bytes);
    } else {
        val = MmioMap::BUS_FLOAT;
    }
    // Merge into the destination register, preserving upper bytes for sub-32-bit
    // reads (e.g. MOV AL,[mem] must not clobber AH or bits 16-31).
    if (size_bytes == 1)
        ctx->gp[dst_gp_idx] = (ctx->gp[dst_gp_idx] & 0xFFFFFF00u) | (val & 0xFFu);
    else if (size_bytes == 2)
        ctx->gp[dst_gp_idx] = (ctx->gp[dst_gp_idx] & 0xFFFF0000u) | (val & 0xFFFFu);
    else
        ctx->gp[dst_gp_idx] = val;
}

void mmio_dispatch_write(GuestContext* ctx, uint32_t pa,
                         uint32_t src_gp_idx, uint32_t size_bytes) {
    if (pa < GUEST_RAM_SIZE) {
        uint32_t val = ctx->gp[src_gp_idx];
        memcpy(reinterpret_cast<uint8_t*>(ctx->fastmem_base) + pa, &val, size_bytes);
    } else if (ctx->mmio) {
        ctx->mmio->write(pa, ctx->gp[src_gp_idx], size_bytes);
    }
}

void mmio_dispatch_write_imm(GuestContext* ctx, uint32_t pa,
                             uint32_t value, uint32_t size_bytes) {
    if (pa < GUEST_RAM_SIZE) {
        memcpy(reinterpret_cast<uint8_t*>(ctx->fastmem_base) + pa, &value, size_bytes);
    } else if (ctx->mmio) {
        ctx->mmio->write(pa, value, size_bytes);
    }
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
    if (!(pde & PTE_PRESENT)) goto fault;

    // 4 MB page (PS=1)?
    if (pde & PDE_PS) {
        uint32_t pa = (pde & PDE_4MB_BASE) | (va & PDE_4MB_OFF);
        if (is_write && !(pde & PTE_RW)) goto fault;
        if (!(pde & PTE_ACCESSED) || (is_write && !(pde & PTE_DIRTY))) {
            pde |= PTE_ACCESSED;
            if (is_write) pde |= PTE_DIRTY;
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
        if (!(pte & PTE_PRESENT)) goto fault;
        if (is_write && !(pte & PTE_RW)) goto fault;
        uint32_t pa = (pte & GUEST_PAGE_MASK) | (va & 0xFFF);
        bool need_pde_update = !(pde & PTE_ACCESSED);
        bool need_pte_update = !(pte & PTE_ACCESSED) || (is_write && !(pte & PTE_DIRTY));
        if (need_pde_update) { pde |= PTE_ACCESSED; memcpy(ram + pde_pa, &pde, 4); }
        if (need_pte_update) {
            pte |= PTE_ACCESSED;
            if (is_write) pte |= PTE_DIRTY;
            memcpy(ram + pt_pa, &pte, 4);
        }
        return pa;
    }

fault:
    ctx->cr2 = va;
    return ~0u;
}

// ---------------------------------------------------------------------------
// Guest memory access helpers (paging + MMIO aware).
// ---------------------------------------------------------------------------

// Translate a guest address to PA if paging is enabled, otherwise pass through.
static inline uint32_t guest_translate(GuestContext* ctx, uint32_t addr, bool is_write) {
    if (ctx->cr0 & 0x80000000u)
        return translate_va_jit(ctx, addr, is_write ? 1 : 0);
    return addr;
}

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

static inline void guest_write(GuestContext* ctx, uint32_t addr, uint32_t val, uint32_t size) {
    uint32_t pa = guest_translate(ctx, addr, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &val, size);
        return;
    }
    if (ctx->mmio) ctx->mmio->write(pa, val, size);
}

// ---------------------------------------------------------------------------
// PUSHFD / POPFD helpers.
// ---------------------------------------------------------------------------

void pushfd_helper(GuestContext* ctx, uint32_t eflags_val) {
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    uint32_t pa = guest_translate(ctx, esp, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &eflags_val, 4);
    }
}

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

// ---------------------------------------------------------------------------
// Guest memory read/write (32-bit).
// ---------------------------------------------------------------------------

uint32_t read_guest_mem32(GuestContext* ctx, uint32_t addr) {
    uint32_t pa = guest_translate(ctx, addr, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        uint32_t v; memcpy(&v, base + pa, 4); return v;
    }
    return ctx->mmio ? ctx->mmio->read(pa, 4) : MmioMap::BUS_FLOAT;
}

void write_guest_mem32(GuestContext* ctx, uint32_t addr, uint32_t val) {
    uint32_t pa = guest_translate(ctx, addr, true);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        memcpy(base + pa, &val, 4);
        return;
    }
    if (ctx->mmio) ctx->mmio->write(pa, val, 4);
}

// ---------------------------------------------------------------------------
// CALL [mem] / PUSH ESP / POP ESP helpers.
// ---------------------------------------------------------------------------

uint32_t call_mem_helper(GuestContext* ctx, uint32_t pa, uint32_t retaddr) {
    uint32_t target = read_guest_mem32(ctx, pa);
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    write_guest_mem32(ctx, esp, retaddr);
    return target;
}

void push_esp_helper(GuestContext* ctx) {
    uint32_t old_esp = ctx->gp[GP_ESP];
    uint32_t new_esp = old_esp - 4;
    ctx->gp[GP_ESP] = new_esp;
    write_guest_mem32(ctx, new_esp, old_esp);
}

void pop_esp_helper(GuestContext* ctx) {
    uint32_t old_esp = ctx->gp[GP_ESP];
    uint32_t val = read_guest_mem32(ctx, old_esp);
    ctx->gp[GP_ESP] = val;
}

// ---------------------------------------------------------------------------
// MOV ESP ↔ [mem] helpers.
// ---------------------------------------------------------------------------

void mov_esp_from_mem(GuestContext* ctx, uint32_t pa) {
    ctx->gp[GP_ESP] = read_guest_mem32(ctx, pa);
}

void mov_esp_to_mem(GuestContext* ctx, uint32_t pa) {
    write_guest_mem32(ctx, pa, ctx->gp[GP_ESP]);
}

// ---------------------------------------------------------------------------
// High-byte register MOV helpers (AH/CH/DH/BH can't use REX in x86-64).
// ---------------------------------------------------------------------------

void mov_highbyte_from_mem(GuestContext* ctx, uint32_t pa, uint32_t gp_idx) {
    auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
    uint8_t byte = (pa < GUEST_RAM_SIZE) ? base[pa] : 0;
    ctx->gp[gp_idx] = (ctx->gp[gp_idx] & 0xFFFF00FFu) | ((uint32_t)byte << 8);
}

void mov_highbyte_to_mem(GuestContext* ctx, uint32_t pa, uint32_t gp_idx) {
    uint8_t byte = (uint8_t)(ctx->gp[gp_idx] >> 8);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        base[pa] = byte;
    }
}

// ---------------------------------------------------------------------------
// PUSH [mem] / POP [mem] helpers.
// ---------------------------------------------------------------------------

void push_mem_helper(GuestContext* ctx, uint32_t pa) {
    uint32_t val = read_guest_mem32(ctx, pa);
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    ctx->gp[GP_ESP] = esp;
    write_guest_mem32(ctx, esp, val);
}

void pop_mem_helper(GuestContext* ctx, uint32_t pa) {
    uint32_t esp = ctx->gp[GP_ESP];
    uint32_t val = read_guest_mem32(ctx, esp);
    ctx->gp[GP_ESP] = esp + 4;
    write_guest_mem32(ctx, pa, val);
}

// ---------------------------------------------------------------------------
// PUSHAD / POPAD helpers.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// ENTER helper.
// ---------------------------------------------------------------------------

void enter_helper(GuestContext* ctx, uint32_t alloc_size, uint32_t nesting) {
    nesting &= 0x1F; // mask to 0..31

    // Step 1: PUSH EBP
    uint32_t esp = ctx->gp[GP_ESP] - 4;
    write_guest_mem32(ctx, esp, ctx->gp[GP_EBP]);
    uint32_t frame_ptr = esp;

    // Step 2: higher nesting levels push previous frame pointers
    if (nesting > 0) {
        for (uint32_t i = 1; i < nesting; ++i) {
            ctx->gp[GP_EBP] -= 4;
            esp -= 4;
            write_guest_mem32(ctx, esp, read_guest_mem32(ctx, ctx->gp[GP_EBP]));
        }
        esp -= 4;
        write_guest_mem32(ctx, esp, frame_ptr);
    }

    // Step 3: MOV EBP, frame_ptr; SUB ESP, alloc_size
    ctx->gp[GP_EBP] = frame_ptr;
    ctx->gp[GP_ESP] = esp - alloc_size;
}

// ---------------------------------------------------------------------------
// XLATB helper: AL = byte at [EBX + zero_extend(AL)].
// ---------------------------------------------------------------------------

uint32_t xlatb_helper(GuestContext* ctx) {
    uint32_t ea = ctx->gp[GP_EBX] + (ctx->gp[GP_EAX] & 0xFF);
    uint32_t pa = guest_translate(ctx, ea, false);
    if (pa < GUEST_RAM_SIZE) {
        auto* base = reinterpret_cast<uint8_t*>(ctx->fastmem_base);
        return base[pa];
    }
    return ctx->mmio ? ctx->mmio->read(pa, 1) : 0xFF;
}

// ---------------------------------------------------------------------------
// IRETD helper: pop EIP, CS, EFLAGS from guest stack.
// ---------------------------------------------------------------------------

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
