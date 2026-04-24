#include "executor.hpp"
#include <Zydis/Zydis.h>
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
        "mov %%rcx, %%rdi\n\t"   // normalise: ctx → RDI
        "mov %%rdx, %%rsi\n\t"   // code → RSI
#endif
        // ---- Set up pinned executor registers ----------------------------
        "mov %%rdi, %%r13\n\t"              // R13 = ctx
        "movq 48(%%r13), %%r12\n\t"         // R12 = ctx->fastmem_base
        "movl 56(%%r13), %%r15d\n\t"        // R15D = ctx->ram_size
        // Stash host_code (RSI) in R14 before we clobber RSI with guest ESI.
        "mov %%rsi, %%r14\n\t"

        // ---- Save host FPU/SSE state, load guest FPU/SSE state ----------
        "lea 640(%%r13), %%rax\n\t"         // RAX = &ctx->host_fpu
        "fxsave (%%rax)\n\t"
        "lea 128(%%r13), %%rax\n\t"         // RAX = &ctx->guest_fpu
        "fxrstor (%%rax)\n\t"

        // ---- Load guest GP registers into host registers -----------------
        // (ESP intentionally skipped — stays in ctx->gp[4])
        "movl  0(%%r13), %%eax\n\t"
        "movl  4(%%r13), %%ecx\n\t"
        "movl  8(%%r13), %%edx\n\t"
        "movl 12(%%r13), %%ebx\n\t"
        "movl 20(%%r13), %%ebp\n\t"
        "movl 24(%%r13), %%esi\n\t"
        "movl 28(%%r13), %%edi\n\t"

        // Restore guest EFLAGS into host RFLAGS.
        // R14 still holds host_code, so use the stack.
        "push %%r14\n\t"                      // save host_code
        "movl 36(%%r13), %%r14d\n\t"          // R14D = ctx->eflags
        "push %%r14\n\t"
        "popfq\n\t"                            // load guest EFLAGS
        "pop %%r14\n\t"                        // restore host_code

        // ---- Dispatch into trace -----------------------------------------
        // The trace ends with RET, which returns here.
        "call *%%r14\n\t"

        // ---- Save guest EFLAGS from host RFLAGS -------------------------
        "pushfq\n\t"
        "pop %%r14\n\t"                        // R14 = guest EFLAGS
        "movl %%r14d, 36(%%r13)\n\t"          // ctx->eflags

        // ---- Save guest GP registers back --------------------------------
        "movl %%eax,  0(%%r13)\n\t"
        "movl %%ecx,  4(%%r13)\n\t"
        "movl %%edx,  8(%%r13)\n\t"
        "movl %%ebx, 12(%%r13)\n\t"
        "movl %%ebp, 20(%%r13)\n\t"
        "movl %%esi, 24(%%r13)\n\t"
        "movl %%edi, 28(%%r13)\n\t"

        // ---- Save guest FPU/SSE state, restore host FPU/SSE state -------
        "lea 128(%%r13), %%rax\n\t"         // RAX = &ctx->guest_fpu
        "fxsave (%%rax)\n\t"
        "lea 640(%%r13), %%rax\n\t"         // RAX = &ctx->host_fpu
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
    ctx.halted       = false;
    ctx.page_versions = (uint64_t)(uintptr_t)pv.ver;  // SMC write-side
    tlb.flush();

    // Initialize guest FPU state to clean defaults.
    // FCW = 0x037F: all x87 exceptions masked, double precision, round-nearest
    uint16_t fcw = 0x037F;
    memcpy(ctx.guest_fpu + 0, &fcw, 2);
    // MXCSR = 0x1F80: all SSE exceptions masked, round-nearest
    uint32_t mxcsr = 0x1F80;
    memcpy(ctx.guest_fpu + 24, &mxcsr, 4);

    return true;
}

void Executor::destroy() {
    cc.destroy();
    if (ram) { platform::free_ram(ram, GUEST_RAM_SIZE); ram = nullptr; }
}

void Executor::load_guest(uint32_t pa, const void* src, size_t size) {
    assert(pa + size <= GUEST_RAM_SIZE);
    memcpy(ram + pa, src, size);
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

        // Verify the target trace's page is still current.
        uint32_t target_pa = lk.target_eip;
        if (paging_enabled()) {
            target_pa = translate_va(lk.target_eip, /*is_write=*/false);
            if (target_pa == ~0u) continue;
        }
        if (pv.get(target_pa) != target->page_ver) continue;

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
                // Reset JMP rel32 to 0 → fallthrough to the cold exit stub.
                int32_t zero = 0;
                memcpy(lk.jmp_rel32, &zero, 4);
                lk.linked = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Privileged instruction handler — decode and dispatch HLT, IN, OUT, etc.
// Called from run loop when a trace exits with STOP_PRIVILEGED.
// ctx.eip == address of the privileged instruction.
// ---------------------------------------------------------------------------

void Executor::handle_privileged() {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32,
                     ZYDIS_STACK_WIDTH_32);

    ZydisDecodedInstruction insn;
    ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];

    if (ctx.eip >= GUEST_RAM_SIZE) {
        fprintf(stderr, "[exec] privileged EIP=%08X out of range\n", ctx.eip);
        ctx.halted = true;
        return;
    }

    const uint8_t* pc = ram + ctx.eip;
    ZyanUSize avail = GUEST_RAM_SIZE - ctx.eip;
    if (avail > 15) avail = 15;

    ZyanStatus st = ZydisDecoderDecodeFull(&decoder, pc, avail, &insn, ops);
    if (!ZYAN_SUCCESS(st)) {
        fprintf(stderr, "[exec] decode failed at privileged EIP=%08X\n", ctx.eip);
        ctx.halted = true;
        return;
    }

    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_HLT:
        ctx.halted = true;
        return;

    case ZYDIS_MNEMONIC_CLI:
        ctx.virtual_if = false;
        ctx.eip += insn.length;
        return;

    case ZYDIS_MNEMONIC_STI:
        ctx.virtual_if = true;
        ctx.eip += insn.length;
        return;

    case ZYDIS_MNEMONIC_CPUID: {
        uint32_t leaf = ctx.gp[GP_EAX];
        // Pentium III Coppermine: Family 6, Model 8, Stepping 3
        switch (leaf) {
        case 0:
            ctx.gp[GP_EAX] = 2;               // max standard leaf
            ctx.gp[GP_EBX] = 0x756E6547;       // "Genu"
            ctx.gp[GP_EDX] = 0x49656E69;       // "ineI"
            ctx.gp[GP_ECX] = 0x6C65746E;       // "ntel"
            break;
        case 1:
            ctx.gp[GP_EAX] = 0x00000683;       // family=6 model=8 stepping=3
            ctx.gp[GP_EBX] = 0x00000000;
            ctx.gp[GP_ECX] = 0x00000000;
            // EDX: FPU, DE, PSE, TSC, MSR, PAE, MCE, CX8, SEP, MTRR,
            //       PGE, MCA, CMOV, PAT, PSE-36, MMX, FXSR, SSE
            ctx.gp[GP_EDX] = 0x0383F9FF;
            break;
        default:
            ctx.gp[GP_EAX] = 0;
            ctx.gp[GP_EBX] = 0;
            ctx.gp[GP_ECX] = 0;
            ctx.gp[GP_EDX] = 0;
            break;
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_RDTSC: {
        // Return a monotonic TSC value based on host rdtsc.
        uint64_t tsc;
#if defined(_MSC_VER)
        tsc = __rdtsc();
#else
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        tsc = ((uint64_t)hi << 32) | lo;
#endif
        ctx.gp[GP_EAX] = (uint32_t)tsc;
        ctx.gp[GP_EDX] = (uint32_t)(tsc >> 32);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_RDMSR: {
        // MSR index in ECX, result in EDX:EAX
        uint32_t msr = ctx.gp[GP_ECX];
        uint64_t val = 0;
        switch (msr) {
        case 0x174: val = ctx.sysenter_cs;  break; // IA32_SYSENTER_CS
        case 0x175: val = ctx.sysenter_esp; break; // IA32_SYSENTER_ESP
        case 0x176: val = ctx.sysenter_eip; break; // IA32_SYSENTER_EIP
        case 0x10:  // IA32_TIME_STAMP_COUNTER
#if defined(_MSC_VER)
            val = __rdtsc();
#else
            { uint32_t lo, hi;
              __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
              val = ((uint64_t)hi << 32) | lo; }
#endif
            break;
        default:
            // Stub: return 0, log for debugging
            fprintf(stderr, "[exec] RDMSR ECX=%08X → 0\n", msr);
            break;
        }
        ctx.gp[GP_EAX] = (uint32_t)val;
        ctx.gp[GP_EDX] = (uint32_t)(val >> 32);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_WRMSR: {
        uint32_t msr = ctx.gp[GP_ECX];
        uint64_t val = ((uint64_t)ctx.gp[GP_EDX] << 32) | ctx.gp[GP_EAX];
        switch (msr) {
        case 0x174: ctx.sysenter_cs  = (uint32_t)val; break;
        case 0x175: ctx.sysenter_esp = (uint32_t)val; break;
        case 0x176: ctx.sysenter_eip = (uint32_t)val; break;
        default:
            // Stub: log and ignore (MTRR, misc MSRs)
            fprintf(stderr, "[exec] WRMSR ECX=%08X val=%016llX\n", msr,
                    (unsigned long long)val);
            break;
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LGDT: {
        // LGDT [mem] — load GDT base+limit from 6-byte memory operand
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            // Base register handling (simplified)
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 6 <= GUEST_RAM_SIZE) {
                ctx.gdtr_limit = *reinterpret_cast<uint16_t*>(ram + ea);
                ctx.gdtr_base  = *reinterpret_cast<uint32_t*>(ram + ea + 2);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LIDT: {
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 6 <= GUEST_RAM_SIZE) {
                ctx.idtr_limit = *reinterpret_cast<uint16_t*>(ram + ea);
                ctx.idtr_base  = *reinterpret_cast<uint32_t*>(ram + ea + 2);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_MOV: {
        // MOV CRn, r32  or  MOV r32, CRn
        // ops[0] = destination, ops[1] = source
        auto cr_to_ptr = [&](ZydisRegister r) -> uint32_t* {
            switch (r) {
            case ZYDIS_REGISTER_CR0: return &ctx.cr0;
            case ZYDIS_REGISTER_CR2: return &ctx.cr2;
            case ZYDIS_REGISTER_CR3: return &ctx.cr3;
            case ZYDIS_REGISTER_CR4: return &ctx.cr4;
            default: return nullptr;
            }
        };
        auto gp_index = [](ZydisRegister r) -> int {
            uint8_t enc;
            return reg32_enc(r, enc) ? (int)enc : -1;
        };

        // MOV CRn, r32 (write to CR)
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint32_t* cr = cr_to_ptr(ops[0].reg.value);
            int gp = gp_index(ops[1].reg.value);
            if (cr && gp >= 0) {
                *cr = ctx.gp[gp];
                // Flush TLB on CR3 write; invalidate all traces on CR0 write
                // (paging mode change makes all emitted code stale).
                if (ops[0].reg.value == ZYDIS_REGISTER_CR3)
                    tlb.flush();
                if (ops[0].reg.value == ZYDIS_REGISTER_CR0) {
                    tlb.flush();
                    tcache.clear();
                    cc.reset();
                    arena.reset();
                }
                ctx.eip += insn.length;
                return;
            }
            // MOV r32, CRn (read from CR)
            cr = cr_to_ptr(ops[1].reg.value);
            gp = gp_index(ops[0].reg.value);
            if (cr && gp >= 0) {
                ctx.gp[gp] = *cr;
                ctx.eip += insn.length;
                return;
            }
        }
        fprintf(stderr, "[exec] unhandled MOV CR/DR at EIP=%08X\n", ctx.eip);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_INVLPG: {
        // INVLPG m — invalidate TLB entry for the page containing EA.
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            tlb.flush_va(ea);
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_WBINVD:
    case ZYDIS_MNEMONIC_INVD:
    case ZYDIS_MNEMONIC_CLTS:
    case ZYDIS_MNEMONIC_LMSW:
        // Stub: ignore cache/task state instructions
        ctx.eip += insn.length;
        return;

    case ZYDIS_MNEMONIC_SYSENTER: {
        // SYSENTER: fast ring-3 → ring-0 transition.
        // CS = SYSENTER_CS_MSR, SS = SYSENTER_CS_MSR + 8
        // EIP = SYSENTER_EIP_MSR, ESP = SYSENTER_ESP_MSR
        // IF cleared, VM cleared.
        if (ctx.sysenter_cs == 0) {
            // #GP(0) if SYSENTER_CS_MSR is 0
            deliver_interrupt(13, ctx.eip, true, 0);
            return;
        }
        ctx.virtual_if = false;
        ctx.gp[GP_ESP] = ctx.sysenter_esp;
        ctx.eip = ctx.sysenter_eip;
        return;
    }

    case ZYDIS_MNEMONIC_SYSEXIT: {
        // SYSEXIT: fast ring-0 → ring-3 return.
        // CS = SYSENTER_CS_MSR + 16, SS = SYSENTER_CS_MSR + 24
        // EIP = EDX, ESP = ECX
        if (ctx.sysenter_cs == 0) {
            deliver_interrupt(13, ctx.eip, true, 0);
            return;
        }
        ctx.eip = ctx.gp[GP_EDX];
        ctx.gp[GP_ESP] = ctx.gp[GP_ECX];
        return;
    }

    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INT1:
        // Debug traps: deliver through IDT.
        // INT3 is vector 3, return address = instruction AFTER the INT3.
        // INT1 is vector 1.
        deliver_interrupt(insn.mnemonic == ZYDIS_MNEMONIC_INT3 ? 3 : 1,
                          ctx.eip + insn.length);
        return;

    case ZYDIS_MNEMONIC_INT: {
        // Software interrupt: INT imm8
        uint8_t vector = (uint8_t)ops[0].imm.value.u;
        // HLE intercept: if this vector matches the HLE trap vector,
        // call the handler instead of delivering through the IDT.
        if (hle_handler && vector == hle_vector) {
            uint32_t ordinal = ctx.gp[GP_EAX];
            ctx.eip += insn.length; // advance past INT
            if (hle_handler(*this, ordinal, hle_user))
                return;
            // Handler returned false — fall through to IDT delivery.
            ctx.eip -= insn.length; // undo advance
        }
        deliver_interrupt(vector, ctx.eip + insn.length);
        return;
    }

    case ZYDIS_MNEMONIC_INTO:
        // INTO: interrupt on overflow — deliver vector 4 if OF is set.
        if (ctx.eflags & 0x800u) {
            deliver_interrupt(4, ctx.eip + insn.length);
        } else {
            ctx.eip += insn.length;
        }
        return;

    case ZYDIS_MNEMONIC_OUT: {
        // OUT imm8, AL:   ops[0]=imm, ops[1]=reg(AL/AX/EAX)
        // OUT DX, AL:     ops[0]=DX,  ops[1]=reg(AL/AX/EAX)
        uint16_t port;
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            port = (uint16_t)ops[0].imm.value.u;
        else
            port = (uint16_t)(ctx.gp[GP_EDX] & 0xFFFF);

        unsigned size = ops[1].size / 8;
        uint32_t value = ctx.gp[GP_EAX];
        if (size == 1) value &= 0xFF;
        else if (size == 2) value &= 0xFFFF;

        io_write(port, value, size);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_IN: {
        // IN AL, imm8:   ops[0]=reg(AL/AX/EAX), ops[1]=imm
        // IN AL, DX:     ops[0]=reg(AL/AX/EAX), ops[1]=DX
        uint16_t port;
        if (ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            port = (uint16_t)ops[1].imm.value.u;
        else
            port = (uint16_t)(ctx.gp[GP_EDX] & 0xFFFF);

        unsigned size = ops[0].size / 8;
        uint32_t value = io_read(port, size);

        if (size == 1)
            ctx.gp[GP_EAX] = (ctx.gp[GP_EAX] & ~0xFFu) | (value & 0xFF);
        else if (size == 2)
            ctx.gp[GP_EAX] = (ctx.gp[GP_EAX] & ~0xFFFFu) | (value & 0xFFFF);
        else
            ctx.gp[GP_EAX] = value;

        ctx.eip += insn.length;
        return;
    }

    default:
        fprintf(stderr, "[exec] unhandled privileged mnem=%d at EIP=%08X\n",
                insn.mnemonic, ctx.eip);
        ctx.halted = true;
        return;
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
    return 0xFFFFFFFF;
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
    uint32_t idt_offset = (uint32_t)vector * 8;
    if (idt_offset + 7 > ctx.idtr_limit) {
        fprintf(stderr, "[exec] IDT vector %u exceeds limit (%u)\n",
                vector, ctx.idtr_limit);
        ctx.halted = true;
        return;
    }

    uint32_t desc_addr = ctx.idtr_base + idt_offset;
    uint32_t desc_pa = desc_addr;
    if (paging_enabled()) {
        desc_pa = translate_va(desc_addr, false);
        if (desc_pa == ~0u) {
            fprintf(stderr, "[exec] IDT descriptor VA %08X page fault\n", desc_addr);
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
        fprintf(stderr, "[exec] IDT vector %u not present\n", vector);
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
// VA → PA page-table walk (32-bit non-PAE, 4 KB pages).
//
// Page directory: 1024 × 4-byte entries at PA = CR3 & ~0xFFF.
//   PDE[31:12] = page table PA base
//   PDE[7]     = PS (page size): 1 = 4 MB page (direct map), 0 = 4 KB page table
//   PDE[0]     = P (present)
//
// Page table (if PS=0): 1024 × 4-byte entries.
//   PTE[31:12] = physical page base
//   PTE[1]     = R/W (0 = read-only)
//   PTE[0]     = P (present)
//
// Returns PA on success, ~0u on fault.
// On fault: sets CR2 = faulting VA.  The caller should deliver #PF (vector 14)
// with an appropriate error code.
// ---------------------------------------------------------------------------

uint32_t Executor::translate_va(uint32_t va, bool is_write) {
    uint32_t pdir_pa = ctx.cr3 & 0xFFFFF000u;
    uint32_t pdi     = (va >> 22) & 0x3FF;
    uint32_t pti     = (va >> 12) & 0x3FF;

    // Read PDE
    uint32_t pde_pa = pdir_pa + pdi * 4;
    if (pde_pa + 4 > GUEST_RAM_SIZE) goto fault;
    uint32_t pde;
    memcpy(&pde, ram + pde_pa, 4);

    if (!(pde & 1)) goto fault; // not present

    // 4 MB page (PS=1)?
    if (pde & 0x80) {
        // PA = PDE[31:22] << 22 | VA[21:0]
        uint32_t pa = (pde & 0xFFC00000u) | (va & 0x003FFFFFu);
        // Check write permission
        if (is_write && !(pde & 2)) goto fault;
        // Set accessed + dirty bits
        if (!(pde & 0x20) || (is_write && !(pde & 0x40))) {
            pde |= 0x20;  // accessed
            if (is_write) pde |= 0x40;  // dirty
            memcpy(ram + pde_pa, &pde, 4);
        }
        return pa;
    }

    // 4 KB page table
    {
        uint32_t pt_pa = (pde & 0xFFFFF000u) + pti * 4;
        if (pt_pa + 4 > GUEST_RAM_SIZE) goto fault;
        uint32_t pte;
        memcpy(&pte, ram + pt_pa, 4);

        if (!(pte & 1)) goto fault; // not present

        if (is_write && !(pte & 2)) goto fault; // read-only

        uint32_t pa = (pte & 0xFFFFF000u) | (va & 0xFFF);

        // Set accessed + dirty bits
        bool need_pde_update = !(pde & 0x20);
        bool need_pte_update = !(pte & 0x20) || (is_write && !(pte & 0x40));
        if (need_pde_update) {
            pde |= 0x20;
            memcpy(ram + pde_pa, &pde, 4);
        }
        if (need_pte_update) {
            pte |= 0x20;
            if (is_write) pte |= 0x40;
            memcpy(ram + pt_pa, &pte, 4);
        }

        return pa;
    }

fault:
    ctx.cr2 = va;
    return ~0u;
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void Executor::run(uint32_t entry_eip, uint64_t max_steps) {
    ctx.eip    = entry_eip;
    ctx.halted = false;

    uint64_t steps = 0;
    Trace* prev_trace = nullptr;

    while (!ctx.halted) {
        if (max_steps && steps >= max_steps) break;

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

        // When paging is enabled, translate EIP (VA) to PA for code fetch.
        // The trace cache keys on the guest VA (ctx.eip).
        uint32_t code_pa = eip;
        if (paging_enabled()) {
            code_pa = translate_va(eip, /*is_write=*/false);
            if (code_pa == ~0u) {
                // #PF on instruction fetch: deliver page fault (vector 14).
                // Error code: bit 0=0 (not present), bit 2=0 (supervisor),
                // bit 4=1 (instruction fetch).
                deliver_interrupt(14, eip, /*has_error=*/true, /*error_code=*/0x10);
                prev_trace = nullptr;
                continue;
            }
        }

        // Lookup trace in cache.
        Trace* t = tcache.lookup(eip);

        // Validate: check page version for SMC.
        if (t && t->valid) {
            if (pv.get(code_pa) != t->page_ver) {
                unlink_trace(t);      // remove incoming links before invalidation
                tcache.invalidate(eip);
                t = nullptr;
                prev_trace = nullptr; // chain broken by invalidation
            }
        } else {
            t = nullptr;
        }

        // Build if missing.
        if (!t) {
            t = builder.build(code_pa, ram, GUEST_RAM_SIZE,
                              cc, arena, pv, &ctx);
            if (!t) {
                fprintf(stderr, "[exec] build failed at EIP=%08X (PA=%08X) — halting\n", eip, code_pa);
                break;
            }
            // Override guest_eip to store the VA (for cache lookup).
            t->guest_eip = eip;
            t->page_ver  = pv.get(code_pa);
            tcache.insert(t);
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
        prev_trace = t;

        // Periodic device tick (e.g. PIT timer).
        if (tick_period && ++tick_counter >= tick_period) {
            tick_counter = 0;
            tick_fn(tick_user);
        }

        // Privileged instruction stop: decode and handle in the run loop.
        if (ctx.stop_reason == STOP_PRIVILEGED) {
            prev_trace = nullptr; // chain broken
            handle_privileged();
            continue;
        }

        // Page fault during JIT memory access: deliver #PF (vector 14).
        // translate_va_jit already set CR2 to the faulting VA.
        if (ctx.stop_reason == STOP_PAGE_FAULT) {
            prev_trace = nullptr; // chain broken
            // Error code: bit 0 = 0 (not present), bit 2 = 0 (supervisor).
            // Full error code computation would check P, W/R, U/S bits;
            // for now use 0 (not-present supervisor read).
            deliver_interrupt(14, ctx.eip, /*has_error=*/true, /*error_code=*/0);
            continue;
        }

        // Check for HALT condition (EIP == 0 or EIP out of RAM used as sentinel).
        if (ctx.eip == 0xFFFF'FFFFu || ctx.eip >= GUEST_RAM_SIZE) {
            fprintf(stderr, "[exec] EIP=%08X out of range — halting\n", ctx.eip);
            ctx.halted = true;
            break;
        }
    }
}
