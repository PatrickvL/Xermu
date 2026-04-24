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
        // Stub: return 0 for all MSRs, log for debugging
        fprintf(stderr, "[exec] RDMSR ECX=%08X → 0\n", msr);
        ctx.gp[GP_EAX] = (uint32_t)val;
        ctx.gp[GP_EDX] = (uint32_t)(val >> 32);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_WRMSR: {
        uint32_t msr = ctx.gp[GP_ECX];
        uint64_t val = ((uint64_t)ctx.gp[GP_EDX] << 32) | ctx.gp[GP_EAX];
        // Stub: log and ignore
        fprintf(stderr, "[exec] WRMSR ECX=%08X val=%016llX\n", msr,
                (unsigned long long)val);
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

    case ZYDIS_MNEMONIC_INVLPG:
        // Stub: no paging yet, just advance EIP
        ctx.eip += insn.length;
        return;

    case ZYDIS_MNEMONIC_WBINVD:
    case ZYDIS_MNEMONIC_INVD:
    case ZYDIS_MNEMONIC_CLTS:
    case ZYDIS_MNEMONIC_LMSW:
        // Stub: ignore cache/task state instructions
        ctx.eip += insn.length;
        return;

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

    uint32_t desc_pa = ctx.idtr_base + idt_offset;
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

    esp -= 4; if (esp < GUEST_RAM_SIZE) memcpy(ram + esp, &saved_eflags, 4);
    uint32_t cs_val = (uint32_t)selector;
    esp -= 4; if (esp < GUEST_RAM_SIZE) memcpy(ram + esp, &cs_val, 4);
    esp -= 4; if (esp < GUEST_RAM_SIZE) memcpy(ram + esp, &return_eip, 4);

    if (has_error) {
        esp -= 4; if (esp < GUEST_RAM_SIZE) memcpy(ram + esp, &error_code, 4);
    }

    ctx.gp[GP_ESP] = esp;

    // Clear IF for interrupt gates (type nibble == 0xE); trap gates (0xF) leave IF.
    if ((type_attr & 0x0F) == 0x0E) {
        ctx.virtual_if = false;
    }

    ctx.eip = handler;
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void Executor::run(uint32_t entry_eip, uint64_t max_steps) {
    ctx.eip    = entry_eip;
    ctx.halted = false;

    uint64_t steps = 0;

    while (!ctx.halted) {
        if (max_steps && steps >= max_steps) break;

        // Deliver pending hardware IRQs at trace boundaries.
        if (pending_irq && ctx.virtual_if) {
            // Find lowest-numbered pending IRQ line.
            unsigned irq = 0;
            uint32_t bits = pending_irq;
            while (!(bits & 1)) { bits >>= 1; ++irq; }
            pending_irq &= ~(1u << irq);
            // Map IRQ to IDT vector: PIC-style IRQ 0-15 → vectors 0x20-0x2F.
            deliver_interrupt(uint8_t(0x20 + irq), ctx.eip);
        }

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
        ctx.eip         = eip;
        ctx.stop_reason = STOP_NONE;
        dispatch_trace(&ctx, t->host_code);

        // Advance EIP to what the trace exit stub wrote.
        ctx.eip = ctx.next_eip;
        ++steps;

        // Privileged instruction stop: decode and handle in the run loop.
        if (ctx.stop_reason == STOP_PRIVILEGED) {
            handle_privileged();
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
