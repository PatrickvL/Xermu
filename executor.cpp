#include "executor.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>

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
//   R15 = ram_size       (callee-saved)
//
// Guest GP registers live in host EAX–EDI while a trace runs.
// ESP is NOT mapped to host RSP; it stays in ctx->gp[GP_ESP] and is
// accessed by the inline sequences emitted for PUSH/POP/CALL/RET.
//
// Windows x64 note:
//   Swap RDI↔RCX and RSI↔RDX in the prolog; also push/pop RDI and RSI
//   (they are callee-saved on Windows but not on SysV).
// ---------------------------------------------------------------------------

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
        "mov %%rcx, %%rdi\n\t"   // normalise: ctx → RDI
        "mov %%rdx, %%rsi\n\t"   // code → RSI
#endif
        // ---- Set up pinned executor registers ----------------------------
        "mov %%rdi, %%r13\n\t"              // R13 = ctx
        "movq 48(%%r13), %%r12\n\t"         // R12 = ctx->fastmem_base
        "movl 56(%%r13), %%r15d\n\t"        // R15D = ctx->ram_size
        // Stash host_code (RSI) in R14 before we clobber RSI with guest ESI.
        "mov %%rsi, %%r14\n\t"

        // ---- Load guest GP registers into host registers -----------------
        // (ESP intentionally skipped — stays in ctx->gp[4])
        "movl  0(%%r13), %%eax\n\t"
        "movl  4(%%r13), %%ecx\n\t"
        "movl  8(%%r13), %%edx\n\t"
        "movl 12(%%r13), %%ebx\n\t"
        "movl 20(%%r13), %%ebp\n\t"
        "movl 24(%%r13), %%esi\n\t"
        "movl 28(%%r13), %%edi\n\t"

        // ---- Dispatch into trace -----------------------------------------
        // The trace ends with RET, which returns here.
        "call *%%r14\n\t"

        // ---- Save guest GP registers back --------------------------------
        "movl %%eax,  0(%%r13)\n\t"
        "movl %%ecx,  4(%%r13)\n\t"
        "movl %%edx,  8(%%r13)\n\t"
        "movl %%ebx, 12(%%r13)\n\t"
        "movl %%ebp, 20(%%r13)\n\t"
        "movl %%esi, 24(%%r13)\n\t"
        "movl %%edi, 28(%%r13)\n\t"

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

// ---------------------------------------------------------------------------
// XboxExecutor
// ---------------------------------------------------------------------------

bool XboxExecutor::init(MmioMap* mmio) {
    ram = static_cast<uint8_t*>(platform::alloc_ram(GUEST_RAM_SIZE));
    if (!ram) return false;
    memset(ram, 0xCC, GUEST_RAM_SIZE); // INT3 fill (debug aid)

    if (!cc.init()) return false;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fastmem_base = (uint64_t)(uintptr_t)ram;
    ctx.ram_size     = GUEST_RAM_SIZE;
    ctx.mmio         = mmio;
    ctx.cr0          = 0x00000011;  // PE=1, ET=1 (protected mode, no paging)
    ctx.eflags       = 0x00000002;  // reserved bit always set
    ctx.virtual_if   = true;

    return true;
}

void XboxExecutor::destroy() {
    cc.destroy();
    if (ram) { platform::free_ram(ram, GUEST_RAM_SIZE); ram = nullptr; }
}

void XboxExecutor::load_guest(uint32_t pa, const void* src, size_t size) {
    assert(pa + size <= GUEST_RAM_SIZE);
    memcpy(ram + pa, src, size);
}

// ---------------------------------------------------------------------------
// Privileged instruction handler (stub — expand per opcode as needed).
// ---------------------------------------------------------------------------

void XboxExecutor::handle_privileged() {
    fprintf(stderr, "[exec] privileged insn at EIP=%08X\n", ctx.eip);
    // Real implementation: decode ctx.eip, handle RDMSR/WRMSR/IN/OUT/etc.
    ctx.virtual_if = false; // halt on unhandled privileged insn
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void XboxExecutor::run(uint32_t entry_eip, uint64_t max_steps) {
    ctx.eip = entry_eip;

    uint64_t steps = 0;

    while (ctx.virtual_if) {
        if (max_steps && steps >= max_steps) break;

        uint32_t eip = ctx.eip;

        // Lookup trace in cache.
        Trace* t = tcache.lookup(eip);

        // Validate: check page version for SMC.
        if (t && t->valid) {
            if (pv.get(eip) != t->page_ver) {
                tcache.invalidate(eip);
                t = nullptr;
            }
        } else {
            t = nullptr;
        }

        // Build if missing.
        if (!t) {
            t = builder.build(eip, ram, GUEST_RAM_SIZE,
                              cc, arena, pv, &ctx);
            if (!t) {
                fprintf(stderr, "[exec] build failed at EIP=%08X — halting\n", eip);
                break;
            }
            tcache.insert(t);
        }

        // Execute the trace.
        ctx.eip = eip;          // snapshot before call (epilog writes next_eip)
        dispatch_trace(&ctx, t->host_code);

        // Advance EIP to what the trace exit stub wrote.
        ctx.eip = ctx.next_eip;

        // Check for HALT condition (EIP == 0 or EIP out of RAM used as sentinel).
        if (ctx.eip == 0xFFFF'FFFFu || ctx.eip >= GUEST_RAM_SIZE) {
            fprintf(stderr, "[exec] EIP=%08X out of range — halting\n", ctx.eip);
            break;
        }

        ++steps;
    }
}
