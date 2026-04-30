#include "executor.hpp"
#include "fault_handler.hpp"
#include "../xbox/pic.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// dispatch_trace — naked asm trampoline (GCC / Clang, x86-64)
//
// System V AMD64 ABI entry:
//   RDI = GuestContext*
//   RSI = void* host_code
//
// Pinned registers inside a trace:
//   R12 = fastmem_base   (callee-saved — preserved across the call)
//   R13 = GuestContext*  (callee-saved)
//   R14 = EA scratch     (callee-saved — we push/pop it ourselves)
//   R15 = unused         (callee-saved — push/pop for ABI compliance)
//
// Guest GP registers live in host EAXâ€“EDI while a trace runs.
// ESP is NOT mapped to host RSP; it stays in ctx->gp[GP_ESP] and is
// accessed by the inline sequences emitted for PUSH/POP/CALL/RET.
//
// Windows x64 note:
//   Swap RDIâ†”RCX and RSIâ†”RDX in the prolog; also push/pop RDI and RSI
//   (they are callee-saved on Windows but not on SysV).
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
__attribute__((naked))
void dispatch_trace([[maybe_unused]] GuestContext* ctx,
                    [[maybe_unused]] void*         host_code) {
    __asm__ volatile(
        // ---- Prolog: save callee-saved host registers --------------------
        "push %%rbx\n\t"
        "push %%rbp\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
#ifdef _WIN32
        // RDI and RSI are callee-saved on Windows x64.
        "push %%rdi\n\t"
        "push %%rsi\n\t"
        // On Windows, args arrive in RCX (ctx) and RDX (code).
        "mov %%rcx, %%rdi\n\t"   // normalise: ctx â†’ RDI
        "mov %%rdx, %%rsi\n\t"   // code â†’ RSI
#endif
        // ---- Set up pinned executor registers ----------------------------
        "mov %%rdi, %%r13\n\t"              // R13 = ctx
        "movq " CTX_STR_(CTX_ASM_FASTMEM_BASE) "(%%r13), %%r12\n\t"  // R12 = ctx->fastmem_base
        // R15 is no longer used (was ram_size); push/pop for ABI compliance only.
        // Stash host_code (RSI) in R14 before we clobber RSI with guest ESI.
        "mov %%rsi, %%r14\n\t"

        // ---- Save host FPU/SSE state, load guest FPU/SSE state ----------
        "lea " CTX_STR_(CTX_ASM_HOST_FPU) "(%%r13), %%rax\n\t"   // RAX = &ctx->host_fpu
        "fxsave (%%rax)\n\t"
        "lea " CTX_STR_(CTX_ASM_GUEST_FPU) "(%%r13), %%rax\n\t"  // RAX = &ctx->guest_fpu
        "fxrstor (%%rax)\n\t"

        // ---- Load guest GP registers into host registers -----------------
        // (ESP intentionally skipped — stays in ctx->gp[4])
        "movl " CTX_STR_(CTX_ASM_GP_EAX) "(%%r13), %%eax\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_ECX) "(%%r13), %%ecx\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_EDX) "(%%r13), %%edx\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_EBX) "(%%r13), %%ebx\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_EBP) "(%%r13), %%ebp\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_ESI) "(%%r13), %%esi\n\t"
        "movl " CTX_STR_(CTX_ASM_GP_EDI) "(%%r13), %%edi\n\t"

        // Restore guest EFLAGS into host RFLAGS.
        // R14 still holds host_code, so use the stack.
        "push %%r14\n\t"                      // save host_code
        "movl " CTX_STR_(CTX_ASM_EFLAGS) "(%%r13), %%r14d\n\t"  // R14D = ctx->eflags
        "push %%r14\n\t"
        "popfq\n\t"                            // load guest EFLAGS
        "pop %%r14\n\t"                        // restore host_code

        // ---- Dispatch into trace -----------------------------------------
        // The trace ends with RET, which returns here.
        "call *%%r14\n\t"

        // ---- Save guest EFLAGS from host RFLAGS -------------------------
        "pushfq\n\t"
        "pop %%r14\n\t"                        // R14 = guest EFLAGS
        "movl %%r14d, " CTX_STR_(CTX_ASM_EFLAGS) "(%%r13)\n\t"  // ctx->eflags

        // ---- Save guest GP registers back --------------------------------
        "movl %%eax, " CTX_STR_(CTX_ASM_GP_EAX) "(%%r13)\n\t"
        "movl %%ecx, " CTX_STR_(CTX_ASM_GP_ECX) "(%%r13)\n\t"
        "movl %%edx, " CTX_STR_(CTX_ASM_GP_EDX) "(%%r13)\n\t"
        "movl %%ebx, " CTX_STR_(CTX_ASM_GP_EBX) "(%%r13)\n\t"
        "movl %%ebp, " CTX_STR_(CTX_ASM_GP_EBP) "(%%r13)\n\t"
        "movl %%esi, " CTX_STR_(CTX_ASM_GP_ESI) "(%%r13)\n\t"
        "movl %%edi, " CTX_STR_(CTX_ASM_GP_EDI) "(%%r13)\n\t"

        // ---- Save guest FPU/SSE state, restore host FPU/SSE state -------
        "lea " CTX_STR_(CTX_ASM_GUEST_FPU) "(%%r13), %%rax\n\t"  // RAX = &ctx->guest_fpu
        "fxsave (%%rax)\n\t"
        "lea " CTX_STR_(CTX_ASM_HOST_FPU) "(%%r13), %%rax\n\t"   // RAX = &ctx->host_fpu
        "fxrstor (%%rax)\n\t"

        // ---- Epilog: restore host registers ------------------------------
#ifdef _WIN32
        "pop %%rsi\n\t"
        "pop %%rdi\n\t"
#endif
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%rbp\n\t"
        "pop %%rbx\n\t"
        "ret\n\t"
        ::: // no C-level inputs/outputs; all state is through memory (ctx)
    );
}
#endif // GCC/Clang — MSVC uses dispatch_trace.asm

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------

bool Executor::init(MmioMap* mmio) {
    // Try 4 GB fastmem window first (enables VEH-based MMIO handling).
    fastmem_window = platform::alloc_4gb_window(GUEST_RAM_SIZE);
    if (!fastmem_window.base) {
        // Probe largest free VA region to diagnose fragmentation
        size_t largest_free = 0;
        uintptr_t addr = 0;
        MEMORY_BASIC_INFORMATION mbi;
        while (VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_FREE && mbi.RegionSize > largest_free)
                largest_free = mbi.RegionSize;
            uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
        fprintf(stderr, "[mem] 4GB window failed (largest free VA: %zu MB), using 320MB\n",
                largest_free / (1024*1024));
        // Fallback: 320 MB aliased window.
        fastmem_window = platform::alloc_fastmem_window(FASTMEM_WINDOW_SIZE,
                                                         GUEST_RAM_SIZE);
    } else {
        fprintf(stderr, "[mem] 4GB window allocated at %p\n", fastmem_window.base);
    }
    if (fastmem_window.base) {
        ram = static_cast<uint8_t*>(fastmem_window.base);
    } else {
        // Fallback: plain allocation (mirror goes through MMIO slow path).
        ram = static_cast<uint8_t*>(platform::alloc_ram(GUEST_RAM_SIZE));
    }
    if (!ram) return false;
    memset(ram, 0, GUEST_RAM_SIZE); // zero-fill (matches Xbox kernel boot)

    // If we got the 4 GB window, commit memory at the NV2A register range
    // (0xFD000000-0xFDFFFFFF) so PGRAPH/PFB/PCRTC reads/writes don't fault.
    // This eliminates VEH overhead for the game's GPU init loop (millions of
    // register accesses).  The committed pages are zero-filled, which is
    // correct for PGRAPH_STATUS (0 = idle) and most other registers.
    nv2a_committed_ = false;
    if (fastmem_window.window_size >= 0x100000000ULL) {
        constexpr size_t USED_END  = 0x14000000ULL;  // placeholder starts here
        constexpr size_t NV2A_OFF  = 0xFD000000ULL;
        constexpr size_t NV2A_LEN  = 0x01000000ULL;  // 16 MB
        auto base8 = static_cast<uint8_t*>(fastmem_window.base);
        // The placeholder covers [USED_END, 4GB).
        // Split #1: [USED_END, NV2A_OFF) and [NV2A_OFF, 4GB)
        BOOL ok1 = VirtualFree(base8 + USED_END, NV2A_OFF - USED_END,
                               MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        if (ok1) {
            // Split #2: [NV2A_OFF, NV2A_OFF+NV2A_LEN) and [NV2A_OFF+NV2A_LEN, 4GB)
            BOOL ok2 = VirtualFree(base8 + NV2A_OFF, NV2A_LEN,
                                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            if (ok2) {
                // Commit the NV2A region as read-write zero-fill.
                void* nv2a_mem = VirtualAlloc(base8 + NV2A_OFF, NV2A_LEN,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (nv2a_mem) {
                    nv2a_committed_ = true;
                    fprintf(stderr, "[mmio] committed NV2A registers at fastmem+0x%llX (%zu MB)\n",
                            (unsigned long long)NV2A_OFF, NV2A_LEN / (1024*1024));
                } else {
                    fprintf(stderr, "[mmio] VirtualAlloc for NV2A failed: %lu\n", GetLastError());
                }
            } else {
                fprintf(stderr, "[mmio] VirtualFree split #2 failed: %lu\n", GetLastError());
            }
        } else {
            fprintf(stderr, "[mmio] VirtualFree split #1 failed: %lu\n", GetLastError());
        }
    }

    if (!cc.init()) return false;

    // Pre-generate shared MMIO slow-path helpers into the helper page.
    generate_mmio_helpers(cc.helper_page, HELPER_PAGE_BYTES, mmio_helpers);

    memset(&ctx, 0, sizeof(ctx));
    ctx.fastmem_base = (uint64_t)(uintptr_t)ram;
    ctx.mmio         = mmio;
    ctx.soft_tlb     = &tlb;
    ctx.cr0          = 0x00000011;  // PE=1, ET=1 (protected mode, no paging)
    ctx.eflags       = 0x00000002;  // reserved bit always set
    ctx.virtual_if   = true;
    ctx.halted       = false;
    tlb.flush();

    // Initialize guest FPU state to clean defaults.
    static constexpr uint16_t FPU_CW_INIT  = 0x037F; // all exceptions masked, double precision, round-nearest
    static constexpr uint32_t SSE_MXCSR_INIT = 0x1F80; // all exceptions masked, round-nearest
    memcpy(ctx.guest_fpu + 0, &FPU_CW_INIT, 2);
    memcpy(ctx.guest_fpu + 24, &SSE_MXCSR_INIT, 4);

    // Install VEH for 4 GB fastmem faults.
#ifdef _WIN32
    g_active_executor = this;
    veh_handle_ = AddVectoredExceptionHandler(1 /*first*/, fastmem_veh_handler);
#endif

    return true;
}

void Executor::destroy() {
#ifdef _WIN32
    if (veh_handle_) {
        RemoveVectoredExceptionHandler(veh_handle_);
        veh_handle_ = nullptr;
    }
    g_active_executor = nullptr;
#endif

    cc.destroy();
    if (fastmem_window.base) {
        if (fastmem_window.window_size >= 0x100000000ULL)
            platform::free_4gb_window(fastmem_window);
        else
            platform::free_fastmem_window(fastmem_window);
    } else if (ram) {
        platform::free_ram(ram, GUEST_RAM_SIZE);
    }
    ram = nullptr;
}

void Executor::load_guest(uint32_t pa, const void* src, size_t size) {
    assert(pa + size <= GUEST_RAM_SIZE);
    // If loading into a write-protected code page, temporarily unprotect.
    uint32_t page_start = pa & GUEST_PAGE_MASK;
    uint32_t page_end   = (pa + (uint32_t)size + GUEST_PAGE_SIZE - 1) & GUEST_PAGE_MASK;
    for (uint32_t p = page_start; p < page_end; p += GUEST_PAGE_SIZE) {
        if (is_code_page(p))
            invalidate_code_page(p);
    }
    memcpy(ram + pa, src, size);
}

// ---------------------------------------------------------------------------
// SMC page protection — mark code pages read-only, invalidate on write.
// ---------------------------------------------------------------------------

void Executor::protect_code_page(uint32_t pa) {
    uint32_t page = pa & GUEST_PAGE_MASK;
    if (is_code_page(page)) return; // already protected
    set_code_page(page);
#ifdef _WIN32
    DWORD old_prot;
    VirtualProtect(ram + page, GUEST_PAGE_SIZE, PAGE_READONLY, &old_prot);
#else
    mprotect(ram + page, GUEST_PAGE_SIZE, PROT_READ);
#endif
}

void Executor::invalidate_code_page(uint32_t pa) {
    uint32_t page = pa & GUEST_PAGE_MASK;
    if (!is_code_page(page)) return;
    clear_code_page(page);
    // Make the page writable again.
#ifdef _WIN32
    DWORD old_prot;
    VirtualProtect(ram + page, GUEST_PAGE_SIZE, PAGE_READWRITE, &old_prot);
#else
    mprotect(ram + page, GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE);
#endif
    // Invalidate all traces on this page.
    uint32_t l1 = (page >> 12) & TraceCache::L1_MASK;
    auto* pt = tcache.page_map[l1];
    if (pt) {
        for (size_t s = 0; s < TraceCache::L2_SIZE; ++s) {
            Trace* t = pt->slots[s];
            if (t && t->valid && (t->code_pa & ~0xFFFu) == page) {
                unlink_trace(t);
                // Also reset this trace's OWN outbound links — the currently
                // executing trace may have a block-linked JMP to another trace
                // on the same page that is about to be invalidated.
                for (int j = 0; j < t->num_links; ++j) {
                    auto& lk = t->links[j];
                    if (lk.linked) {
                        int32_t zero = 0;
                        memcpy(lk.jmp_rel32, &zero, 4);
                        lk.linked = false;
                    }
                }
                t->valid = false;
            }
        }
    }

    // No fault bitmap to clear — NOP sleds in invalidated traces are dead code.
}

// ---------------------------------------------------------------------------
// Block linking helpers.
// ---------------------------------------------------------------------------

// Try to patch each unlinked exit of `t` to its target trace if present
// in the trace cache.  Also validates the target's page version.
// Backward edges (target_eip <= t->guest_eip) are NOT linked — this ensures
// tight loops return to the run loop for IRQ delivery and device ticks.
void Executor::try_link_trace(Trace* t) {
    for (int i = 0; i < t->num_links; ++i) {
        auto& lk = t->links[i];
        if (lk.linked) continue;

        // Skip backward edges to guarantee run-loop re-entry for timer ticks.
        if (lk.target_eip <= t->guest_eip) continue;

        Trace* target = tcache.lookup(lk.target_eip);
        if (!target || !target->valid) continue;

        // Patch the JMP rel32 to jump directly to the target's host_code.
        Emitter::patch_rel32(lk.jmp_rel32, target->host_code);
        lk.linked = true;
    }
}

// Before invalidating a trace, unlink every trace that has a JMP patched
// into this trace's host_code.  We scan all traces in the arena — this is
// rare (only on SMC) so the O(N) scan is acceptable.
void Executor::unlink_trace(Trace* t) {
    for (size_t i = 0; i < arena.used; ++i) {
        Trace& src = arena.pool[i];
        if (!src.valid) continue;
        for (int j = 0; j < src.num_links; ++j) {
            auto& lk = src.links[j];
            if (lk.linked && lk.target_eip == t->guest_eip) {
                // Reset JMP rel32 to 0 â†’ fallthrough to the cold exit stub.
                int32_t zero = 0;
                memcpy(lk.jmp_rel32, &zero, 4);
                lk.linked = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// I/O port dispatch
// ---------------------------------------------------------------------------

void Executor::register_io(uint16_t port, IoReadFn read, IoWriteFn write,
                                void* user) {
    assert(n_io_ports < MAX_IO_PORTS);
    io_ports[n_io_ports++] = { port, read, write, user };
}

uint32_t Executor::io_read(uint16_t port, unsigned size) {
    for (int i = 0; i < n_io_ports; ++i) {
        if (io_ports[i].port == port && io_ports[i].read)
            return io_ports[i].read(port, size, io_ports[i].user);
    }
    fprintf(stderr, "[io] unhandled read  port=%04X size=%u\n", port, size);
    return 0xFFFFFFFFu;
}

void Executor::io_write(uint16_t port, uint32_t val, unsigned size) {
    for (int i = 0; i < n_io_ports; ++i) {
        if (io_ports[i].port == port && io_ports[i].write) {
            io_ports[i].write(port, val, size, io_ports[i].user);
            return;
        }
    }
    fprintf(stderr, "[io] unhandled write port=%04X val=%08X size=%u\n",
            port, val, size);
}

// ---------------------------------------------------------------------------
// Interrupt / exception delivery through the IDT.
//
// Reads the gate descriptor from the guest IDT, pushes an interrupt frame
// (EFLAGS, CS, EIP, and optionally an error code) onto the guest stack,
// clears IF for interrupt gates, and sets ctx.eip to the handler.
//
// Gate descriptor layout (32-bit protected mode, 8 bytes):
//   [0..1]  offset bits 0-15
//   [2..3]  segment selector
//   [4]     reserved (0)
//   [5]     type & attributes: P(1) DPL(2) 0(1) gate_type(4)
//           gate_type: 0xE = 32-bit interrupt gate (clears IF)
//                      0xF = 32-bit trap gate (leaves IF unchanged)
//   [6..7]  offset bits 16-31
// ---------------------------------------------------------------------------

void Executor::deliver_interrupt(uint8_t vector, uint32_t return_eip,
                                  bool has_error, uint32_t error_code) {
    // Diagnostic: count hardware IRQ deliveries
    static int irq_deliver_count = 0;
    if (vector >= 0x30 && irq_deliver_count < 5) {
        irq_deliver_count++;
        fprintf(stderr, "[diag] IRQ deliver: vector=0x%02X return_eip=0x%08X IF=%d (count=%d)\n",
                vector, return_eip, ctx.virtual_if ? 1 : 0, irq_deliver_count);
    }
    // Specifically track vector 0x33 (GPU IRQ 3)
    static int gpu_irq_count = 0;
    if (vector == 0x33 && gpu_irq_count < 3) {
        gpu_irq_count++;
        fprintf(stderr, "[diag] GPU vector 0x33 delivered! return_eip=0x%08X (count=%d)\n",
                return_eip, gpu_irq_count);
    }

    uint32_t idt_offset = (uint32_t)vector * 8;
    if (idt_offset + 7 > ctx.idtr_limit) {
        fprintf(stderr, "[exec] IDT vector %u exceeds limit (%u) at EIP=0x%08X\n",
                vector, ctx.idtr_limit, return_eip);
        ctx.halted = true;
        return;
    }

    uint32_t desc_addr = ctx.idtr_base + idt_offset;
    uint32_t desc_pa = desc_addr;
    if (paging_enabled()) {
        desc_pa = translate_va(desc_addr, false);
        if (desc_pa == ~0u) {
            // Diagnostic: dump PDE for the failing VA.
            uint32_t pdi = (desc_addr >> 22) & 0x3FF;
            uint32_t pde_pa = (ctx.cr3 & 0xFFFFF000u) + pdi * 4;
            uint32_t pde = 0;
            if (pde_pa + 4 <= GUEST_RAM_SIZE)
                memcpy(&pde, ram + pde_pa, 4);
            fprintf(stderr, "[exec] IDT descriptor VA %08X page fault (vector=%u CR2=%08X CR3=%08X PDE[%03X]=%08X)\n",
                    desc_addr, vector, ctx.cr2, ctx.cr3, pdi, pde);
            ctx.halted = true;
            return;
        }
    }
    if (desc_pa + 8 > GUEST_RAM_SIZE) {
        fprintf(stderr, "[exec] IDT descriptor PA %08X out of range\n", desc_pa);
        ctx.halted = true;
        return;
    }

    // Parse gate descriptor.
    uint16_t offset_lo, selector, offset_hi;
    uint8_t  type_attr;
    memcpy(&offset_lo, ram + desc_pa + 0, 2);
    memcpy(&selector,  ram + desc_pa + 2, 2);
    memcpy(&type_attr, ram + desc_pa + 5, 1);
    memcpy(&offset_hi, ram + desc_pa + 6, 2);

    if (!(type_attr & 0x80)) {
        fprintf(stderr, "[exec] IDT vector %u not present (type_attr=0x%02X desc_pa=0x%08X return_eip=0x%08X current_eip=0x%08X)\n",
                vector, type_attr, desc_pa, return_eip, ctx.eip);
        // Dump 8 bytes of the IDT entry
        uint8_t idt_bytes[8];
        memcpy(idt_bytes, ram + desc_pa, 8);
        fprintf(stderr, "[exec] IDT[%u] bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                vector, idt_bytes[0], idt_bytes[1], idt_bytes[2], idt_bytes[3],
                idt_bytes[4], idt_bytes[5], idt_bytes[6], idt_bytes[7]);
        ctx.halted = true;
        return;
    }

    uint32_t handler = ((uint32_t)offset_hi << 16) | offset_lo;

    // Push interrupt frame: EFLAGS, CS, EIP (and optionally error code).
    // Merge virtual_if into the saved EFLAGS so IRETD can restore it.
    uint32_t saved_eflags = (ctx.eflags & ~0x200u)
                          | (ctx.virtual_if ? 0x200u : 0u)
                          | 0x02u;  // bit 1 always set
    uint32_t esp = ctx.gp[GP_ESP];

    // Stack bounds check: if ESP is too low to push the interrupt frame
    // (12-16 bytes), halt to prevent infinite fault loops.
    // When paging is enabled, ESP is a VA — only check for near-zero wrap.
    uint32_t frame_size = has_error ? 16 : 12;
    if (esp < frame_size || (!paging_enabled() && esp > GUEST_RAM_SIZE)) {
        fprintf(stderr, "[exec] stack overflow delivering vector %u (ESP=0x%08X)\n",
                vector, esp);
        ctx.halted = true;
        return;
    }

    auto push32 = [&](uint32_t val) {
        esp -= 4;
        uint32_t pa = esp;
        if (paging_enabled()) {
            pa = translate_va(esp, true);
            if (pa == ~0u) return; // page fault during push
        }
        if (pa < GUEST_RAM_SIZE) memcpy(ram + pa, &val, 4);
    };

    push32(saved_eflags);
    push32((uint32_t)selector);
    push32(return_eip);

    if (has_error) {
        push32(error_code);
    }

    ctx.gp[GP_ESP] = esp;

    // Clear IF for interrupt gates (type nibble == 0xE); trap gates (0xF) leave IF.
    if ((type_attr & 0x0F) == 0x0E) {
        ctx.virtual_if = false;
    }

    ctx.eip = handler;
}

// ---------------------------------------------------------------------------
// VA â†’ PA page-table walk (32-bit non-PAE, 4 KB pages).
//
// Page directory: 1024 Ã— 4-byte entries at PA = CR3 & ~0xFFF.
//   PDE[31:12] = page table PA base
//   PDE[7]     = PS (page size): 1 = 4 MB page (direct map), 0 = 4 KB page table
//   PDE[0]     = P (present)
//
// Page table (if PS=0): 1024 Ã— 4-byte entries.
//   PTE[31:12] = physical page base
//   PTE[1]     = R/W (0 = read-only)
//   PTE[0]     = P (present)
//
// Returns PA on success, ~0u on fault.
// On fault: sets CR2 = faulting VA.  The caller should deliver #PF (vector 14)
// with an appropriate error code.
// ---------------------------------------------------------------------------

uint32_t Executor::translate_va(uint32_t va, bool is_write) {
    // Software TLB lookup.
    uint32_t vpn = va >> 12;
    uint32_t tlb_idx = vpn & SoftTlb::MASK;
    auto& te = is_write ? tlb.write_tlb[tlb_idx] : tlb.read_tlb[tlb_idx];
    if (te.tag == vpn)
        return te.pa_page | (va & 0xFFF);

    uint32_t pdir_pa = alias_pa(ctx.cr3 & GUEST_PAGE_MASK);
    uint32_t pdi     = (va >> 22) & 0x3FF;
    uint32_t pti     = (va >> 12) & 0x3FF;

    // Read PDE
    uint32_t pde_pa = pdir_pa + pdi * 4;
    if (pde_pa + 4 > GUEST_RAM_SIZE) goto fault_np;
    uint32_t pde;
    memcpy(&pde, ram + pde_pa, 4);

    if (!(pde & PTE_PRESENT)) goto fault_np;

    // 4 MB page (PS=1)?
    if (pde & PDE_PS) {
        uint32_t pa = alias_pa((pde & PDE_4MB_BASE) | (va & PDE_4MB_OFF));
        if (is_write && !(pde & PTE_RW)) goto fault_prot;
        if (!(pde & PTE_ACCESSED) || (is_write && !(pde & PTE_DIRTY))) {
            pde |= PTE_ACCESSED;
            if (is_write) pde |= PTE_DIRTY;
            memcpy(ram + pde_pa, &pde, 4);
        }
        // TLB fill (also populate read TLB on write hits for future reads).
        te.tag = vpn;  te.pa_page = pa & GUEST_PAGE_MASK;
        if (is_write) { tlb.read_tlb[tlb_idx].tag = vpn; tlb.read_tlb[tlb_idx].pa_page = pa & GUEST_PAGE_MASK; }
        return pa;
    }

    // 4 KB page table
    {
        uint32_t pt_pa = alias_pa((pde & GUEST_PAGE_MASK)) + pti * 4;
        if (pt_pa + 4 > GUEST_RAM_SIZE) goto fault_np;
        uint32_t pte;
        memcpy(&pte, ram + pt_pa, 4);

        if (!(pte & PTE_PRESENT)) goto fault_np;
        if (is_write && !(pte & PTE_RW)) goto fault_prot;

        uint32_t pa = alias_pa((pte & GUEST_PAGE_MASK) | (va & 0xFFF));

        bool need_pde_update = !(pde & PTE_ACCESSED);
        bool need_pte_update = !(pte & PTE_ACCESSED) || (is_write && !(pte & PTE_DIRTY));
        if (need_pde_update) {
            pde |= PTE_ACCESSED;
            memcpy(ram + pde_pa, &pde, 4);
        }
        if (need_pte_update) {
            pte |= PTE_ACCESSED;
            if (is_write) pte |= PTE_DIRTY;
            memcpy(ram + pt_pa, &pte, 4);
        }

        // TLB fill.
        te.tag = vpn;  te.pa_page = pa & GUEST_PAGE_MASK;
        if (is_write) { tlb.read_tlb[tlb_idx].tag = vpn; tlb.read_tlb[tlb_idx].pa_page = pa & GUEST_PAGE_MASK; }
        return pa;
    }

fault_prot:
    ctx.cr2 = va;
    ctx.pf_error_code = 1u | (is_write ? 2u : 0u);  // P=1 (present), W/R, U/S=0
    return ~0u;

fault_np:
    ctx.cr2 = va;
    ctx.pf_error_code = (is_write ? 2u : 0u);  // P=0 (not present), W/R, U/S=0
    return ~0u;
}

// ---------------------------------------------------------------------------
// Block memory read: copy 'size' bytes from guest VA to host buffer.
// Handles paging translation and page boundary crossings.
// Returns false if any page translation faults.
// ---------------------------------------------------------------------------

bool Executor::read_guest_block(uint32_t va, uint32_t size, void* buf) {
    auto* dst = static_cast<uint8_t*>(buf);
    while (size > 0) {
        uint32_t pa;
        if (paging_enabled()) {
            pa = translate_va(va, /*is_write=*/false);
            if (pa == ~0u) return false;
        } else {
            pa = va;
        }
        if (pa >= GUEST_RAM_SIZE) return false;

        uint32_t page_remain = GUEST_PAGE_SIZE - (pa & (GUEST_PAGE_SIZE - 1));
        uint32_t chunk = (size < page_remain) ? size : page_remain;
        memcpy(dst, ram + pa, chunk);
        dst  += chunk;
        va   += chunk;
        size -= chunk;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Block memory write: copy 'size' bytes from host buffer to guest VA.
// Handles paging translation and page boundary crossings.
// Returns false if any page translation faults.
// ---------------------------------------------------------------------------

bool Executor::write_guest_block(uint32_t va, uint32_t size, const void* buf) {
    auto* src = static_cast<const uint8_t*>(buf);
    while (size > 0) {
        uint32_t pa;
        if (paging_enabled()) {
            pa = translate_va(va, /*is_write=*/true);
            if (pa == ~0u) return false;
        } else {
            pa = va;
        }
        if (pa >= GUEST_RAM_SIZE) return false;

        uint32_t page_remain = GUEST_PAGE_SIZE - (pa & (GUEST_PAGE_SIZE - 1));
        uint32_t chunk = (size < page_remain) ? size : page_remain;
        memcpy(ram + pa, src, chunk);
        src  += chunk;
        va   += chunk;
        size -= chunk;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Real-mode boot stub interpreter.
//
// The x86 reset vector (0xFFFFFFF0) executes in 16-bit real mode.  The Xbox
// BIOS prologue is a small sequence: JMP, LGDT, LIDT, MOV CR0 (PE=1), far JMP
// to 32-bit protected mode.  Rather than implementing a full 16-bit decoder,
// we interpret just enough instructions to reach the far JMP, then switch to
// the JIT for 32-bit protected mode execution.
//
// Supports: JMP rel8, LGDT/LIDT, MOV EAX,CR0, OR AL,imm8, MOV CR0,EAX,
//           JMP ptr16:32, NOP, CLI, CLD.
// ---------------------------------------------------------------------------
bool Executor::interpret_real_mode_boot() {
    // Real mode after reset: CS base = 0xFFFF0000, flat physical addressing.
    uint32_t cs_base = 0xFFFF0000u;
    uint32_t eip = 0xFFF0u;  // CS:IP = F000:FFF0 â†’ PA 0xFFFFFFF0

    auto fetch_byte = [&](uint32_t pa) -> uint8_t {
        if (pa < GUEST_RAM_SIZE) return ram[pa];
        if (ctx.mmio) return (uint8_t)ctx.mmio->read(pa, 1);
        return 0xCC;
    };
    auto fetch_word = [&](uint32_t pa) -> uint16_t {
        uint8_t lo = fetch_byte(pa), hi = fetch_byte(pa + 1);
        return (uint16_t)((hi << 8) | lo);
    };
    auto fetch_dword = [&](uint32_t pa) -> uint32_t {
        uint16_t lo = fetch_word(pa), hi = fetch_word(pa + 2);
        return ((uint32_t)hi << 16) | lo;
    };

    for (int step = 0; step < 100; ++step) {
        uint32_t pa = cs_base + eip;
        uint8_t b0 = fetch_byte(pa);

        // Check for prefixes.
        bool has_66 = false;
        bool has_seg = false;
        uint32_t seg_base = cs_base;
        int prefix_len = 0;

        while (b0 == 0x66 || b0 == 0x2E || b0 == 0x90 || b0 == 0xFC || b0 == 0xFA) {
            if (b0 == 0x66) has_66 = true;
            else if (b0 == 0x2E) { has_seg = true; seg_base = cs_base; }
            else if (b0 == 0x90) { eip++; pa = cs_base + eip; b0 = fetch_byte(pa); continue; } // NOP
            else if (b0 == 0xFC) { /* CLD — ignored, DF already 0 */ }
            else if (b0 == 0xFA) { ctx.virtual_if = false; } // CLI
            prefix_len++;
            b0 = fetch_byte(pa + prefix_len);
        }
        pa += prefix_len;
        eip += prefix_len;

        // JMP rel8
        if (b0 == 0xEB) {
            int8_t rel = (int8_t)fetch_byte(pa + 1);
            eip += 2 + rel;
            continue;
        }

        // 0F group (LGDT, LIDT, MOV CR)
        if (b0 == 0x0F) {
            uint8_t b1 = fetch_byte(pa + 1);

            // LGDT/LIDT: 0F 01 /2 or 0F 01 /3, mod=00, rm=6 â†’ [disp16]
            if (b1 == 0x01) {
                uint8_t modrm = fetch_byte(pa + 2);
                uint8_t reg = (modrm >> 3) & 7;
                // mod=00, rm=6 â†’ [disp16] in 16-bit addressing
                uint16_t disp = fetch_word(pa + 3);
                uint32_t desc_pa = (has_seg ? seg_base : cs_base) + disp;

                // Load 6-byte pseudo-descriptor: 2-byte limit, 4-byte base
                // (with 66 prefix, base is 32-bit; without, only 24-bit)
                uint16_t limit = fetch_word(desc_pa);
                uint32_t base;
                if (has_66)
                    base = fetch_dword(desc_pa + 2);
                else
                    base = fetch_word(desc_pa + 2) | ((uint32_t)fetch_byte(desc_pa + 4) << 16);

                if (reg == 2) { // LGDT
                    ctx.gdtr_base = base;
                    ctx.gdtr_limit = limit;
                } else if (reg == 3) { // LIDT
                    ctx.idtr_base = base;
                    ctx.idtr_limit = limit;
                }
                eip += 5; // 0F 01 modrm disp16
                continue;
            }

            // MOV r32, CR0: 0F 20 C0 (ModRM = 11 000 000 â†’ CR0 â†’ EAX)
            if (b1 == 0x20) {
                uint8_t modrm = fetch_byte(pa + 2);
                uint8_t rm = modrm & 7;
                ctx.gp[rm] = ctx.cr0;
                eip += 3;
                continue;
            }

            // MOV CR0, r32: 0F 22 C0
            if (b1 == 0x22) {
                uint8_t modrm = fetch_byte(pa + 2);
                uint8_t rm = modrm & 7;
                ctx.cr0 = ctx.gp[rm];
                eip += 3;
                // If PE is now set, next instruction should be a far JMP.
                continue;
            }
            // Unknown 0F instruction in boot stub.
            fprintf(stderr, "[boot16] unhandled 0F %02X at PA %08X\n", b1, pa);
            return false;
        }

        // OR AL, imm8
        if (b0 == 0x0C) {
            uint8_t imm = fetch_byte(pa + 1);
            ctx.gp[GP_EAX] = (ctx.gp[GP_EAX] & ~0xFF) | ((ctx.gp[GP_EAX] & 0xFF) | imm);
            eip += 2;
            continue;
        }

        // Far JMP: EA offset selector (16-bit: offset16 sel16; with 66: offset32 sel16)
        if (b0 == 0xEA) {
            uint32_t target;
            uint16_t sel;
            if (has_66) {
                target = fetch_dword(pa + 1);
                sel = fetch_word(pa + 5);
            } else {
                target = fetch_word(pa + 1);
                sel = fetch_word(pa + 3);
            }
            // Switch to protected mode: set EIP, we're done with real mode.
            ctx.eip = target;
            ctx.cr0 |= 1; // ensure PE=1
            printf("[boot16] Far JMP to %04X:%08X — entering protected mode\n", sel, target);
            return true;
        }

        // XOR EAX, EAX (33 C0) — might appear in some boot stubs
        if (b0 == 0x33) {
            uint8_t modrm = fetch_byte(pa + 1);
            if (modrm == 0xC0) {
                ctx.gp[GP_EAX] = 0;
                eip += 2;
                continue;
            }
        }

        fprintf(stderr, "[boot16] unhandled opcode %02X at PA %08X (eip=%04X)\n",
                b0, pa, eip);
        return false;
    }

    fprintf(stderr, "[boot16] exceeded step limit\n");
    return false;
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void Executor::run(uint32_t entry_eip, uint64_t max_steps) {
    ctx.eip    = entry_eip;
    ctx.halted = false;

    uint64_t steps = 0;
    Trace* prev_trace = nullptr;

    // Debug ring buffer: last 64 trace entries for diagnosing crashes.
    static constexpr int TRACE_RING_SIZE = 64;
    struct TraceEntry { uint32_t eip; uint32_t esp; uint32_t ebp; };
    TraceEntry trace_ring[TRACE_RING_SIZE] = {};
    int trace_ring_idx = 0;

    // Spin-loop detection: if the same trace is executed many times in a row,
    // scan for CMP immediate and fast-forward the counter register.
    uint32_t spin_eip    = 0;
    uint32_t spin_count  = 0;
    uint32_t stale_eip   = 0;   // tracks repeated spin triggers at same addr
    uint32_t stale_count = 0;
    static constexpr uint32_t SPIN_THRESHOLD = 16;

    while (!ctx.halted) {
        if (max_steps && steps >= max_steps) {
            break;
        }
        // Deliver pending hardware IRQs at trace boundaries.
        bool has_irq = irq_check ? irq_check(irq_user) : (pending_irq != 0);
        if (has_irq && ctx.virtual_if) {
            uint8_t vector;
            if (irq_ack) {
                vector = irq_ack(irq_user);
            } else {
                // Legacy: simple bitmap, vectors 0x20+.
                unsigned irq = 0;
                uint32_t bits = pending_irq;
                while (!(bits & 1)) { bits >>= 1; ++irq; }
                pending_irq &= ~(1u << irq);
                vector = uint8_t(0x20 + irq);
            }

            deliver_interrupt(vector, ctx.eip);
            prev_trace = nullptr; // chain broken by interrupt
        }

        uint32_t eip = ctx.eip;

        // Record trace entry in ring buffer for diagnostics.
        trace_ring[trace_ring_idx] = { eip, ctx.gp[GP_ESP], ctx.gp[GP_EBP] };
        trace_ring_idx = (trace_ring_idx + 1) % TRACE_RING_SIZE;

        // In HLE mode, if EIP is outside guest RAM and not in the MMIO
        // device region (0xF0000000+), it's a bad jump — halt with diagnostic.
        if (hle_handler && eip >= GUEST_RAM_SIZE && eip < 0xF0000000u) {
            fprintf(stderr, "[exec] bad jump to EIP=0x%08X (outside guest RAM) ESP=0x%08X\n", eip, ctx.gp[GP_ESP]);
            // Dump recent trace history
            fprintf(stderr, "[exec] recent trace history (oldest first):\n");
            for (int i = 0; i < TRACE_RING_SIZE; ++i) {
                int idx = (trace_ring_idx + i) % TRACE_RING_SIZE;
                if (trace_ring[idx].eip == 0 && trace_ring[idx].esp == 0) continue;
                fprintf(stderr, "  EIP=0x%08X ESP=0x%08X EBP=0x%08X\n",
                        trace_ring[idx].eip, trace_ring[idx].esp, trace_ring[idx].ebp);
            }
            // Dump stack for context
            uint32_t esp = ctx.gp[GP_ESP];
            fprintf(stderr, "[exec] stack @ESP=0x%08X:", esp);
            for (int i = 0; i < 8; ++i) {
                uint32_t val = 0;
                if (esp + i * 4 + 4 <= GUEST_RAM_SIZE)
                    memcpy(&val, ram + esp + i * 4, 4);
                fprintf(stderr, " %08X", val);
            }
            fprintf(stderr, "\n");
            ctx.halted = true;
            break;
        }

        // When paging is enabled, translate EIP (VA) to PA for code fetch.
        // The trace cache keys on the guest VA (ctx.eip).
        uint32_t code_pa = eip;
        if (paging_enabled()) {
            code_pa = translate_va(eip, /*is_write=*/false);
            if (code_pa == ~0u) {
                // #PF on instruction fetch: deliver page fault (vector 14).
                // Error code: P and W/R bits set by translate_va, plus
                // bit 4 (instruction fetch).
                deliver_interrupt(14, eip, /*has_error=*/true,
                                  ctx.pf_error_code | 0x10u);
                prev_trace = nullptr;
                ++steps;  // count toward budget to prevent infinite PF loops
                continue;
            }
        }

        // Lookup trace in cache.
        Trace* t = tcache.lookup(eip);

        if (t && !t->valid) t = nullptr;

        // Build if missing.
        if (!t) {
            // Prepare code bytes for the trace builder.
            const uint8_t* code_ptr = nullptr;
            uint32_t code_len = 0;
            uint8_t rom_buf[4096];  // temp buffer for non-RAM code fetch

            if (code_pa < GUEST_RAM_SIZE) {
                // Code is in RAM — read directly.
                code_ptr = ram + code_pa;
                if (!paging_enabled()) {
                    code_len = GUEST_RAM_SIZE - code_pa;
                } else {
                    uint32_t page_remain = 0x1000 - (code_pa & 0xFFF);
                    if (page_remain >= 15) {
                        code_len = page_remain;
                    } else {
                        memcpy(rom_buf, ram + code_pa, page_remain);
                        uint32_t next_va = (eip | 0xFFF) + 1;
                        uint32_t next_pa = translate_va(next_va, false);
                        if (next_pa != ~0u && next_pa < GUEST_RAM_SIZE) {
                            uint32_t extra = 15 - page_remain;
                            memcpy(rom_buf + page_remain, ram + next_pa, extra);
                            code_ptr = rom_buf;
                            code_len = page_remain + extra;
                        } else {
                            code_len = page_remain;
                        }
                    }
                }
            } else if (ctx.mmio) {
                // Code is in MMIO space (e.g. flash ROM) — fetch via MMIO
                // reads into a temp buffer (one page max).
                uint32_t page_off = code_pa & 0xFFF;
                uint32_t fetch_len = 4096 - page_off;
                for (uint32_t i = 0; i < fetch_len; i += 4) {
                    uint32_t pa = code_pa + i;
                    uint32_t remain = fetch_len - i;
                    unsigned sz = (remain >= 4) ? 4 : remain;
                    uint32_t val = ctx.mmio->read(pa, sz);
                    memcpy(rom_buf + i, &val, sz);
                }
                code_ptr = rom_buf;
                code_len = fetch_len;
            }

            t = builder.build(eip, code_ptr, code_len, ram,
                              cc, arena, &ctx);
            if (!t) {
                fprintf(stderr, "[exec] build failed at EIP=%08X (PA=%08X) — halting\n", eip, code_pa);
                break;
            }
            tcache.insert(t);

            // Protect code page for SMC detection (RAM pages only).
            if (code_pa < GUEST_RAM_SIZE)
                protect_code_page(code_pa);
        }

        // Block linking: eagerly link this trace's exits to existing targets,
        // and try to link the previous trace's exits to this trace.
        try_link_trace(t);
        if (prev_trace && prev_trace->valid)
            try_link_trace(prev_trace);

        // Execute the trace.
        ctx.eip         = eip;
        ctx.stop_reason = STOP_NONE;
        dispatch_trace(&ctx, t->host_code);

        // Advance EIP to what the trace exit stub wrote.
        ctx.eip = ctx.next_eip;
        ++steps;

        // Periodic EIP sample to detect stalls
        if ((steps & 0xFFFFF) == 0) {  // every ~1M steps
            static int eip_sample_count = 0;
            if (eip_sample_count < 5) {
                ++eip_sample_count;
                uint32_t sample_pa = ctx.eip;
                if (paging_enabled()) sample_pa = translate_va(ctx.eip, false);
                fprintf(stderr, "[diag] EIP sample at step %llu: 0x%08X bytes:",
                        (unsigned long long)steps, ctx.eip);
                if (sample_pa != ~0u && sample_pa + 16 <= GUEST_RAM_SIZE) {
                    for (int i = 0; i < 16; ++i)
                        fprintf(stderr, " %02X", ram[sample_pa + i]);
                }
                fprintf(stderr, "\n");
                // Also dump the called function at 0x00023F47 (once)
                if (eip_sample_count == 1) {
                    uint32_t fn_va = 0x00023F47;
                    uint32_t fn_pa = fn_va;
                    if (paging_enabled()) fn_pa = translate_va(fn_va, false);
                    if (fn_pa != ~0u && fn_pa + 48 <= GUEST_RAM_SIZE) {
                        fprintf(stderr, "[diag] Fn@0x%08X bytes:", fn_va);
                        for (int i = 0; i < 48; ++i)
                            fprintf(stderr, " %02X", ram[fn_pa + i]);
                        fprintf(stderr, "\n");
                    }
                    // Dump loop context (before EIP 0x0001E417)
                    uint32_t loop_va = 0x0001E3F0;
                    uint32_t loop_pa = loop_va;
                    if (paging_enabled()) loop_pa = translate_va(loop_va, false);
                    if (loop_pa != ~0u && loop_pa + 128 <= GUEST_RAM_SIZE) {
                        fprintf(stderr, "[diag] Loop@0x%08X bytes:", loop_va);
                        for (int i = 0; i < 128; ++i)
                            fprintf(stderr, " %02X", ram[loop_pa + i]);
                        fprintf(stderr, "\n");
                    }
                    // Print EBP-relative values to understand loop state
                    uint32_t ebp = ctx.gp[GP_EBP];
                    auto safe_read = [&](uint32_t va) -> uint32_t {
                        uint32_t pa = paging_enabled() ? translate_va(va, false) : va;
                        if (pa == ~0u || pa + 4 > GUEST_RAM_SIZE) return 0xDEAD;
                        uint32_t v; memcpy(&v, ram + pa, 4); return v;
                    };
                    fprintf(stderr, "[diag] EBP=0x%08X [ebp-14h]=%u [ebp-8]=0x%08X [ebp-4]=0x%08X\n",
                            ebp, safe_read(ebp - 0x14), safe_read(ebp - 0x8), safe_read(ebp - 0x4));
                    // What's at VA 0? (the function reads [ecx] where ecx=[ebp-8])
                    fprintf(stderr, "[diag] [VA 0]=0x%08X [VA 4]=0x%08X ECX=0x%08X\n",
                            safe_read(0), safe_read(4), ctx.gp[GP_ECX]);
                }
            }
        }
        prev_trace = t;

        // Spin-loop detection: if the trace loops back to itself many times,
        // it's a busy-wait or memclear loop. Break out by scanning the trace
        // for the CMP immediate and setting the register to that value.
        if (ctx.eip == eip) {
            if (spin_eip == eip) {
                ++spin_count;
                if (spin_count >= SPIN_THRESHOLD) {
                    // Heuristic: scan the code at EIP for loop-exit patterns
                    // and fast-forward the counter register to exit the loop.
                    // Two-pass scan: CMP patterns first (multi-byte, safe),
                    // then DEC/INC single-byte patterns only at offset 0.
                    bool handled = false;
                    if (code_pa + 32 <= GUEST_RAM_SIZE) {
                        // Pass 1: look for CMP r32, imm or DEC [mem] patterns
                        for (uint32_t off = 0; off < 24 && !handled; ++off) {
                            uint8_t b = ram[code_pa + off];
                            // CMP EAX, imm32 (opcode 3D)
                            if (b == 0x3D && off + 5 <= 28) {
                                uint32_t imm;
                                memcpy(&imm, ram + code_pa + off + 1, 4);
                                ctx.gp[GP_EAX] = imm;
                                handled = true;
                            }
                            // 81 F8..FF = CMP r32, imm32 (/7 = CMP in ModR/M)
                            else if (b == 0x81 && off + 6 <= 28) {
                                uint8_t modrm = ram[code_pa + off + 1];
                                if ((modrm & 0xF8) == 0xF8) { // /7 reg-direct
                                    uint32_t imm;
                                    memcpy(&imm, ram + code_pa + off + 2, 4);
                                    unsigned reg = modrm & 7;
                                    ctx.gp[reg] = imm;
                                    handled = true;
                                }
                            }
                            // 83 F8..FF = CMP r32, imm8
                            else if (b == 0x83 && off + 3 <= 28) {
                                uint8_t modrm = ram[code_pa + off + 1];
                                if ((modrm & 0xF8) == 0xF8) {
                                    uint32_t imm = (uint32_t)(int32_t)(int8_t)ram[code_pa + off + 2];
                                    unsigned reg = modrm & 7;
                                    ctx.gp[reg] = imm;
                                    handled = true;
                                }
                                // 83 E8..EF = SUB r32, imm8 (countdown loop)
                                else if ((modrm & 0xF8) == 0xE8) {
                                    unsigned reg = modrm & 7;
                                    if (ctx.gp[reg] > SPIN_THRESHOLD * 2) break;
                                    // Don't fast-forward loops that contain memory
                                    // stores (0x88=MOV r/m8,r8  0x89=MOV r/m32,r32).
                                    // Those are data-copy loops (memcpy/memmove), not
                                    // spin-waits.
                                    bool has_store = false;
                                    for (uint32_t s = 0; s < off; ++s) {
                                        uint8_t sb = ram[code_pa + s];
                                        if (sb == 0x88 || sb == 0x89) {
                                            has_store = true; break;
                                        }
                                    }
                                    if (has_store) break;
                                    ctx.gp[reg] = 1; // after SUB 1: 0→ZF=1, JNZ falls through
                                    handled = true;
                                }
                            }
                            // FF /1 = DEC [EBP+disp8]  (FF 4D xx)
                            else if (b == 0xFF && off + 3 <= 28 && ram[code_pa + off + 1] == 0x4D) {
                                int8_t disp = (int8_t)ram[code_pa + off + 2];
                                uint32_t addr = ctx.gp[GP_EBP] + (int32_t)disp;
                                if (addr + 4 <= GUEST_RAM_SIZE) {
                                    uint32_t one = 1;
                                    memcpy(ram + addr, &one, 4);
                                }
                                handled = true;
                            }
                        }
                        // Pass 2: DEC r32 at offset 0 = pure delay loop (no stores)
                        if (!handled) {
                            uint8_t b0 = ram[code_pa];
                            if (b0 >= 0x48 && b0 <= 0x4F) {
                                unsigned reg = b0 - 0x48;
                                ctx.gp[reg] = 1;
                                handled = true;
                            }
                        }
                        // Pass 3: MOV reg,[ESP+disp8]; DEC reg; MOV [ESP+disp8],reg; JNZ
                        // OR:    MOV reg,[ESP]; DEC reg; MOV [ESP],reg; JNZ
                        // This is a countdown delay loop. Fast-forward by
                        // zeroing the counter register so DEC sets ZF=0 and
                        // JNZ falls through.
                        if (!handled && code_pa + 12 <= GUEST_RAM_SIZE) {
                            uint8_t b0 = ram[code_pa];
                            // 8B 44 24 dd = MOV EAX,[ESP+disp8]  (or other reg)
                            if (b0 == 0x8B) {
                                uint8_t modrm = ram[code_pa + 1];
                                // mod=01, r/m=100 (SIB follows), with disp8
                                if ((modrm & 0xC7) == 0x44) {
                                    uint8_t sib = ram[code_pa + 2];
                                    if (sib == 0x24) { // SIB = base=ESP, no index
                                        uint8_t disp = ram[code_pa + 3];
                                        unsigned reg = (modrm >> 3) & 7;
                                        // Check DEC reg follows at offset 4
                                        uint8_t b4 = ram[code_pa + 4];
                                        if (b4 >= 0x48 && b4 <= 0x4F &&
                                            (unsigned)(b4 - 0x48) == reg) {
                                            // Set reg to 1 so after DEC: 1→0, ZF=1, JNZ falls through
                                            if (ctx.gp[reg] > SPIN_THRESHOLD * 2) break;
                                            ctx.gp[reg] = 1;
                                            // Also write 1 to the stack counter so the stored
                                            // value matches (in case code re-reads it)
                                            uint32_t sp = ctx.gp[GP_ESP];
                                            uint32_t addr = sp + disp;
                                            uint32_t one = 1;
                                            write_guest_block(addr, 4, &one);
                                            handled = true;
                                        }
                                    }
                                }
                                // mod=00, r/m=100 (SIB follows), no displacement
                                // 8B 04 24 = MOV reg,[ESP]
                                else if ((modrm & 0xC7) == 0x04) {
                                    uint8_t sib = ram[code_pa + 2];
                                    if (sib == 0x24) { // SIB = base=ESP, no index
                                        unsigned reg = (modrm >> 3) & 7;
                                        // Check DEC reg follows at offset 3
                                        uint8_t b3 = ram[code_pa + 3];
                                        if (b3 >= 0x48 && b3 <= 0x4F &&
                                            (unsigned)(b3 - 0x48) == reg) {
                                            ctx.gp[reg] = 1;
                                            // Write 1 to [ESP] via paging
                                            uint32_t sp = ctx.gp[GP_ESP];
                                            uint32_t one = 1;
                                            bool ok = write_guest_block(sp, 4, &one);
                                            static int diag_p3 = 0;
                                            if (diag_p3 < 3) {
                                                diag_p3++;
                                                fprintf(stderr, "[diag] Pass3-nodisp: EIP=0x%08X reg=%u sp=0x%08X write_ok=%d\n",
                                                        eip, reg, sp, ok ? 1 : 0);
                                            }
                                            handled = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (!handled) {
                        // Unrecognized loop pattern.
                        // In HLE/XBE mode (hle_handler set), yield to run_step
                        // so the cooperative scheduler can advance time.
                        // In BIOS/LLE mode (no hle_handler), do NOT halt —
                        // the loop is legitimate code (RC4, decompression, etc.)
                        // and there's no scheduler to resume it.
                        // Exception: loops containing memory stores are real work
                        // (push buffer writing, memcpy, etc.) — don't yield.
                        if (hle_handler) {
                            bool has_store = false;
                            if (code_pa + 32 <= GUEST_RAM_SIZE) {
                                for (uint32_t s = 0; s < 28; ++s) {
                                    uint8_t sb = ram[code_pa + s];
                                    if (sb == 0x88 || sb == 0x89 || sb == 0xA2 || sb == 0xA3 ||
                                        sb == 0xC6 || sb == 0xC7 || sb == 0xAA || sb == 0xAB) {
                                        has_store = true; break;
                                    }
                                }
                            }
                            if (!has_store) {
                                static int nospin_diag = 0;
                                if (nospin_diag < 5) {
                                    nospin_diag++;
                                    fprintf(stderr, "[diag] Spin yield (no-store) at EIP=0x%08X steps=%u\n", eip, steps);
                                }
                                ctx.halted = true;
                            }
                        }
                        // else: let the loop continue naturally
                    }
                    // Track "stale" spins: if the same address keeps triggering
                    // spin detection, yield to let HW devices advance.
                    // Only in HLE mode — BIOS loops are legitimate.
                    if (hle_handler) {
                        if (stale_eip == eip) {
                            ++stale_count;
                            if (stale_count >= 64) {
                                static int stale_diag = 0;
                                if (stale_diag < 3) {
                                    stale_diag++;
                                    fprintf(stderr, "[diag] Stale spin at EIP=0x%08X (count=%u)\n", eip, stale_count);
                                }
                                ctx.halted = true;
                                stale_count = 0;
                            }
                        } else {
                            stale_eip = eip;
                            stale_count = 1;
                        }
                    }
                    spin_count = 0;
                }
            } else {
                spin_eip = eip;
                spin_count = 1;
            }
        } else {
            spin_count = 0;

            // Slow-spin detection: track backward-edge targets across CALL
            // boundaries.  Loops containing function calls reset spin_count
            // on forward edges but the backward-edge target repeats.
            // After enough repetitions, fire tick so interrupts can preempt.
            // NOTE: Disabled for now — the high-frequency tick firing adds
            // overhead to legitimate long init loops (PGRAPH channel setup).
            // VBlank IRQs fire naturally through tick_period mechanism.
            #if 0
            if (ctx.eip < eip && hle_handler) {
                static uint32_t slow_target = 0;
                static uint32_t slow_count = 0;
                if (ctx.eip == slow_target) {
                    if (++slow_count >= 64) {
                        // Fire ticks to let VBlank/timer IRQs preempt the spin
                        if (tick_fn) {
                            for (int t = 0; t < 100; ++t)
                                tick_fn(tick_user);
                        }
                        slow_count = 0;
                    }
                } else {
                    slow_target = ctx.eip;
                    slow_count = 1;
                }
            }
            #endif

            // Multi-trace cycle detection: catch 2-4 trace ping-pong loops
            // that the single-trace detector misses (e.g. BST cycles in kernel).
            // When detected, fire extra device ticks so timer IRQs can preempt
            // the stuck code via the normal interrupt delivery mechanism.
            {
                static uint32_t prev_eip2 = 0;
                static uint32_t cycle2_count = 0;
                if (ctx.eip == prev_eip2 && ctx.eip != eip) {
                    if (++cycle2_count >= 100) {
                        cycle2_count = 0;
                        // Fire device ticks to advance time while CPU is stuck.
                        if (tick_fn) {
                            for (int t = 0; t < 100; ++t)
                                tick_fn(tick_user);
                        }
                    }
                } else {
                    cycle2_count = 0;
                }
                prev_eip2 = eip;
            }
        }

        // Periodic device tick (e.g. PIT timer).
        if (tick_period && ++tick_counter >= tick_period) {
            tick_counter = 0;
            tick_fn(tick_user);
        }

        // Privileged instruction stop: decode and handle in the run loop.
        if (ctx.stop_reason == STOP_PRIVILEGED) {
            prev_trace = nullptr; // chain broken
            handle_privileged();
            ++steps;  // count privileged ops toward budget

            // HLT: spin-tick until an external interrupt arrives.
            // On real x86, HLT suspends the CPU until the next interrupt.
            // Advance EIP past the HLT opcode (1 byte) so execution resumes
            // at the following instruction after the ISR returns via IRET.
            if (ctx.halted) {
                ctx.eip += 1;  // HLT is always 1 byte (0xF4)
                ctx.halted = false;
                // Spin: call tick_fn to advance device timers until an IRQ fires.
                while (!(irq_check && irq_check(irq_user))) {
                    if (max_steps && steps >= max_steps) { ctx.halted = true; break; }
                    if (tick_period) {
                        if (++tick_counter >= tick_period) {
                            tick_counter = 0;
                            tick_fn(tick_user);
                        }
                    }
                    ++steps;
                }
            }
            continue;
        }

        // Page fault during JIT memory access: deliver #PF (vector 14).
        // translate_va_jit already set CR2 to the faulting VA.
        if (ctx.stop_reason == STOP_PAGE_FAULT) {
            prev_trace = nullptr; // chain broken
            ++steps;  // count PFs toward budget
            static int pf_log_count = 0;
            if (pf_log_count < 50) {
                ++pf_log_count;
                uint32_t cr2 = ctx.cr2;
                uint32_t pdi = (cr2 >> 22) & 0x3FF;
                uint32_t pde_pa = alias_pa(ctx.cr3 & 0xFFFFF000u) + pdi * 4;
                uint32_t pde = 0;
                if (pde_pa + 4 <= GUEST_RAM_SIZE)
                    memcpy(&pde, ram + pde_pa, 4);
                fprintf(stderr, "[exec] #PF at EIP=%08X CR2=%08X PDE[%03X]=%08X err=%02X fs_base=%08X paging=%d\n",
                        ctx.eip, cr2, pdi, pde, ctx.pf_error_code, ctx.fs_base,
                        (ctx.cr0 & 0x80000000u) ? 1 : 0);
                fprintf(stderr, "[exec]   EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n",
                        ctx.gp[0], ctx.gp[3], ctx.gp[1], ctx.gp[2]);
                fprintf(stderr, "[exec]   ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n",
                        ctx.gp[4], ctx.gp[5], ctx.gp[6], ctx.gp[7]);
                // Dump code bytes at faulting EIP
                {
                    uint32_t code_eip = ctx.eip;
                    uint32_t code_pa = code_eip;
                    if (paging_enabled()) code_pa = translate_va(code_eip, false);
                    if (code_pa != ~0u && code_pa + 16 <= GUEST_RAM_SIZE) {
                        fprintf(stderr, "[exec]   code @EIP=%08X:", code_eip);
                        for (int ci = 0; ci < 16; ++ci)
                            fprintf(stderr, " %02X", ram[code_pa + ci]);
                        fprintf(stderr, "\n");
                    }
                }
                // Also dump the IDT entry for vector 14
                uint32_t idt_desc = ctx.idtr_base + 14 * 8;
                uint32_t idt_pa = idt_desc;
                if (paging_enabled()) idt_pa = translate_va(idt_desc, false);
                if (idt_pa != ~0u && idt_pa + 8 <= GUEST_RAM_SIZE) {
                    uint8_t idt_bytes[8];
                    memcpy(idt_bytes, ram + idt_pa, 8);
                    uint32_t handler = ((uint32_t)idt_bytes[7] << 24) | ((uint32_t)idt_bytes[6] << 16)
                                     | ((uint32_t)idt_bytes[1] << 8) | idt_bytes[0];
                    fprintf(stderr, "[exec]   IDT[14] @ PA=%08X: %02X%02X %02X%02X %02X%02X %02X%02X → handler=%08X\n",
                            idt_pa, idt_bytes[0], idt_bytes[1], idt_bytes[2], idt_bytes[3],
                            idt_bytes[4], idt_bytes[5], idt_bytes[6], idt_bytes[7], handler);
                    fprintf(stderr, "[exec]   IDTR: base=%08X limit=%04X\n",
                            ctx.idtr_base, ctx.idtr_limit);
                }
                // Dump last 16 trace ring entries
                fprintf(stderr, "[exec]   Trace history (oldest first):\n");
                for (int i = 0; i < TRACE_RING_SIZE; ++i) {
                    int idx = (trace_ring_idx + i) % TRACE_RING_SIZE;
                    if (trace_ring[idx].eip == 0 && trace_ring[idx].esp == 0) continue;
                    fprintf(stderr, "[exec]     EIP=%08X ESP=%08X EBP=%08X\n",
                            trace_ring[idx].eip, trace_ring[idx].esp, trace_ring[idx].ebp);
                }
            }
            // Use error code computed by translate_va_jit (P, W/R, U/S bits).
            deliver_interrupt(14, ctx.eip, /*has_error=*/true, ctx.pf_error_code);
            continue;
        }

        // Divide-by-zero (#DE, vector 0): VEH caught host EXCEPTION_INT_DIVIDE_BY_ZERO.
        if (ctx.stop_reason == STOP_DIVIDE_ERROR) {
            prev_trace = nullptr;
            deliver_interrupt(0, ctx.eip);
            ++steps;
            continue;
        }

        // Invalid opcode (#UD, vector 6): trace builder couldn't translate.
        if (ctx.stop_reason == STOP_INVALID_OPCODE) {
            prev_trace = nullptr;
            // In HLE mode with no IDT, halt — don't try to deliver through IDT.
            if (hle_handler && ctx.idtr_limit == 0) {
                uint32_t eip = ctx.eip;
                uint32_t esp = ctx.gp[GP_ESP];
                fprintf(stderr, "[exec] #UD at EIP=0x%08X EBP=0x%08X bytes:", eip, ctx.gp[GP_EBP]);
                for (int i = 0; i < 16 && eip + i < GUEST_RAM_SIZE; ++i)
                    fprintf(stderr, " %02X", ram[eip + i]);
                fprintf(stderr, "\n[exec] stack @ESP=0x%08X:", esp);
                for (int i = 0; i < 8; ++i) {
                    uint32_t val = 0;
                    if (esp + i * 4 + 4 <= GUEST_RAM_SIZE)
                        memcpy(&val, ram + esp + i * 4, 4);
                    fprintf(stderr, " %08X", val);
                }
                fprintf(stderr, "\n[exec] recent trace history:\n");
                for (int i = 0; i < TRACE_RING_SIZE; ++i) {
                    int idx = (trace_ring_idx + i) % TRACE_RING_SIZE;
                    if (trace_ring[idx].eip == 0 && trace_ring[idx].esp == 0) continue;
                    fprintf(stderr, "  EIP=0x%08X ESP=0x%08X EBP=0x%08X\n",
                            trace_ring[idx].eip, trace_ring[idx].esp, trace_ring[idx].ebp);
                }
                ctx.halted = true;
                break;
            }
            deliver_interrupt(6, ctx.eip);
            continue;
        }

        // Check for HALT condition (EIP == 0xFFFFFFFF).
        if (ctx.eip == 0xFFFF'FFFFu) {
            fprintf(stderr, "[exec] EIP=%08X -- halting\n", ctx.eip);
            // Dump trace ring
            fprintf(stderr, "[exec] Trace history:\n");
            for (int i = 0; i < TRACE_RING_SIZE; ++i) {
                int idx = (trace_ring_idx + i) % TRACE_RING_SIZE;
                if (trace_ring[idx].eip == 0 && trace_ring[idx].esp == 0) continue;
                fprintf(stderr, "  EIP=%08X ESP=%08X EBP=%08X\n",
                        trace_ring[idx].eip, trace_ring[idx].esp, trace_ring[idx].ebp);
            }
            ctx.halted = true;
            break;
        }
    }
    // Diagnostic: report steps completed on exit
    {
        static int run_exit_count = 0;
        if (run_exit_count < 20) {
            ++run_exit_count;
            fprintf(stderr, "[diag] run() exit #%d: steps=%llu/%llu halted=%d EIP=0x%08X\n",
                    run_exit_count, (unsigned long long)steps, (unsigned long long)max_steps,
                    ctx.halted ? 1 : 0, ctx.eip);
        }
    }
}
