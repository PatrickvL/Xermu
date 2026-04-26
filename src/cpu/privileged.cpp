// ---------------------------------------------------------------------------
// privileged.cpp — Privileged instruction handler for the JIT executor.
//
// Decodes and emulates ring-0 instructions that the JIT cannot execute
// natively: HLT, CLI, STI, CPUID, RDTSC, RDMSR, WRMSR, LGDT, LIDT,
// MOV CR/DR/SEG, IN, OUT, INS/OUTS, INT, SYSENTER/SYSEXIT, etc.
//
// Also contains load_segment_base() and the compute_ea() helper.
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include <Zydis/Zydis.h>
#include <cstdio>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// Helper: compute effective address from a Zydis memory operand.
// ---------------------------------------------------------------------------
static uint32_t compute_ea(const ZydisDecodedOperand& op, const GuestContext& ctx) {
    uint32_t ea = 0;
    if (op.mem.disp.has_displacement)
        ea = (uint32_t)(int32_t)op.mem.disp.value;
    if (op.mem.base != ZYDIS_REGISTER_NONE) {
        uint8_t enc;
        if (reg32_enc(op.mem.base, enc))
            ea += ctx.gp[enc];
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        uint8_t enc;
        if (reg32_enc(op.mem.index, enc))
            ea += ctx.gp[enc] * op.mem.scale;
    }
    return ea;
}

// ---------------------------------------------------------------------------
// Helper: load segment base from GDT descriptor given a selector.
// Returns 0 for null selector or if GDT is out of guest RAM.
// ---------------------------------------------------------------------------
uint32_t Executor::load_segment_base(uint16_t sel) {
    if (sel == 0) return 0;  // null selector → base 0
    uint16_t index = sel >> 3;
    uint32_t desc_off = ctx.gdtr_base + (uint32_t)index * 8;

    // Read 8-byte GDT descriptor
    uint8_t desc[8];
    uint32_t pa = desc_off;
    if (paging_enabled()) {
        pa = translate_va(desc_off, false);
        if (pa == ~0u) return 0;
    }
    if (pa + 8 <= GUEST_RAM_SIZE) {
        memcpy(desc, ram + pa, 8);
    } else if (ctx.mmio) {
        for (int i = 0; i < 8; ++i)
            desc[i] = (uint8_t)ctx.mmio->read(pa + i, 1);
    } else {
        return 0;
    }

    // Extract base from descriptor bytes:
    // base[15:0]  = desc[2..3]
    // base[23:16] = desc[4]
    // base[31:24] = desc[7]
    uint32_t base = (uint32_t)desc[2] | ((uint32_t)desc[3] << 8) |
                    ((uint32_t)desc[4] << 16) | ((uint32_t)desc[7] << 24);
    return base;
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

    // Fetch instruction bytes (from RAM or via MMIO for flash ROM).
    uint8_t ibuf[15];
    const uint8_t* pc;
    ZyanUSize avail;

    uint32_t code_pa = ctx.eip;
    if (paging_enabled()) {
        code_pa = translate_va(ctx.eip, false);
        if (code_pa == ~0u) {
            fprintf(stderr, "[exec] #PF translating privileged EIP=%08X\n", ctx.eip);
            ctx.halted = true;
            return;
        }
    }

    if (code_pa < GUEST_RAM_SIZE) {
        pc = ram + code_pa;
        avail = GUEST_RAM_SIZE - code_pa;
        if (avail > 15) avail = 15;
    } else if (ctx.mmio) {
        // Fetch up to 15 bytes via MMIO (e.g. flash ROM)
        avail = 15;
        for (ZyanUSize i = 0; i < avail; ++i)
            ibuf[i] = (uint8_t)ctx.mmio->read(code_pa + (uint32_t)i, 1);
        pc = ibuf;
    } else {
        fprintf(stderr, "[exec] privileged EIP=%08X (PA=%08X) out of range\n",
                ctx.eip, code_pa);
        ctx.halted = true;
        return;
    }

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
        case MSR_SYSENTER_CS:  val = ctx.sysenter_cs;  break;
        case MSR_SYSENTER_ESP: val = ctx.sysenter_esp; break;
        case MSR_SYSENTER_EIP: val = ctx.sysenter_eip; break;
        case MSR_TSC:
#if defined(_MSC_VER)
            val = __rdtsc();
#else
            { uint32_t lo, hi;
              __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
              val = ((uint64_t)hi << 32) | lo; }
#endif
            break;
        // MTRR MSRs
        case MSR_MTRR_DEF_TYPE:     val = ctx.mtrr_def_type; break;
        case MSR_MTRRCAP:           val = 0x0000000000000508ULL; break; // 8 VR, FIX, no WC
        case MSR_MTRR_FIX64K:       val = ctx.mtrr_fix64k; break;
        case MSR_MTRR_FIX16K_80000: val = ctx.mtrr_fix16k[0]; break;
        case MSR_MTRR_FIX16K_A0000: val = ctx.mtrr_fix16k[1]; break;
        default:
            if (msr >= MSR_MTRR_PHYSBASE0 && msr <= 0x20F) {
                int idx = (msr - MSR_MTRR_PHYSBASE0) / 2;
                val = (msr & 1) ? ctx.mtrr_physmask[idx] : ctx.mtrr_physbase[idx];
            } else if (msr >= MSR_MTRR_FIX4K_BASE && msr <= MSR_MTRR_FIX4K_END) {
                val = ctx.mtrr_fix4k[msr - MSR_MTRR_FIX4K_BASE];
            } else {
                fprintf(stderr, "[exec] RDMSR ECX=%08X → 0\n", msr);
            }
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
        case MSR_SYSENTER_CS:  ctx.sysenter_cs  = (uint32_t)val; break;
        case MSR_SYSENTER_ESP: ctx.sysenter_esp = (uint32_t)val; break;
        case MSR_SYSENTER_EIP: ctx.sysenter_eip = (uint32_t)val; break;
        // MTRR MSRs
        case MSR_MTRR_DEF_TYPE:     ctx.mtrr_def_type = val; break;
        case MSR_MTRR_FIX64K:       ctx.mtrr_fix64k = val; break;
        case MSR_MTRR_FIX16K_80000: ctx.mtrr_fix16k[0] = val; break;
        case MSR_MTRR_FIX16K_A0000: ctx.mtrr_fix16k[1] = val; break;
        default:
            if (msr >= MSR_MTRR_PHYSBASE0 && msr <= 0x20F) {
                int idx = (msr - MSR_MTRR_PHYSBASE0) / 2;
                if (msr & 1) ctx.mtrr_physmask[idx] = val;
                else         ctx.mtrr_physbase[idx] = val;
            } else if (msr >= MSR_MTRR_FIX4K_BASE && msr <= MSR_MTRR_FIX4K_END) {
                ctx.mtrr_fix4k[msr - MSR_MTRR_FIX4K_BASE] = val;
            } else {
                fprintf(stderr, "[exec] WRMSR ECX=%08X val=%016llX\n", msr,
                        (unsigned long long)val);
            }
            break;
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LGDT: {
        // LGDT [mem] — load GDT base+limit from 6-byte memory operand
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, false);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 0); return; }
            }
            if (pa + 6 <= GUEST_RAM_SIZE) {
                memcpy(&ctx.gdtr_limit, ram + pa, 2);
                memcpy(&ctx.gdtr_base,  ram + pa + 2, 4);
            } else if (ctx.mmio) {
                ctx.gdtr_limit = (uint16_t)ctx.mmio->read(pa, 2);
                ctx.gdtr_base  = ctx.mmio->read(pa + 2, 4);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LIDT: {
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, false);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 0); return; }
            }
            if (pa + 6 <= GUEST_RAM_SIZE) {
                memcpy(&ctx.idtr_limit, ram + pa, 2);
                memcpy(&ctx.idtr_base,  ram + pa + 2, 4);
            } else if (ctx.mmio) {
                ctx.idtr_limit = (uint16_t)ctx.mmio->read(pa, 2);
                ctx.idtr_base  = ctx.mmio->read(pa + 2, 4);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LLDT: {
        // LLDT r/m16 — load LDT register selector
        uint16_t sel = 0;
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc;
            if (guest_reg_enc(ops[0].reg.value, enc))
                sel = (uint16_t)ctx.gp[enc];
        } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, false);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 0); return; }
            }
            if (pa + 2 <= GUEST_RAM_SIZE)
                memcpy(&sel, ram + pa, 2);
            else if (ctx.mmio)
                sel = (uint16_t)ctx.mmio->read(pa, 2);
        }
        ctx.ldtr_sel = sel;
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_LTR: {
        // LTR r/m16 — load task register selector
        uint16_t sel = 0;
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc;
            if (guest_reg_enc(ops[0].reg.value, enc))
                sel = (uint16_t)ctx.gp[enc];
        } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, false);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 0); return; }
            }
            if (pa + 2 <= GUEST_RAM_SIZE)
                memcpy(&sel, ram + pa, 2);
            else if (ctx.mmio)
                sel = (uint16_t)ctx.mmio->read(pa, 2);
        }
        ctx.tr_sel = sel;
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_SLDT: {
        // SLDT r/m16 — store LDT register selector
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc;
            if (guest_reg_enc(ops[0].reg.value, enc))
                ctx.gp[enc] = ctx.ldtr_sel;
        } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, true);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 2); return; }
            }
            if (pa + 2 <= GUEST_RAM_SIZE) {
                uint16_t v = ctx.ldtr_sel;
                memcpy(ram + pa, &v, 2);
            } else if (ctx.mmio) {
                ctx.mmio->write(pa, ctx.ldtr_sel, 2);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_STR: {
        // STR r/m16 — store task register selector
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            uint8_t enc;
            if (guest_reg_enc(ops[0].reg.value, enc))
                ctx.gp[enc] = ctx.tr_sel;
        } else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, true);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 2); return; }
            }
            if (pa + 2 <= GUEST_RAM_SIZE) {
                uint16_t v = ctx.tr_sel;
                memcpy(ram + pa, &v, 2);
            } else if (ctx.mmio) {
                ctx.mmio->write(pa, ctx.tr_sel, 2);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_SGDT: {
        // SGDT [mem] — store GDT base+limit to 6-byte memory operand
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, true);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 2); return; }
            }
            if (pa + 6 <= GUEST_RAM_SIZE) {
                memcpy(ram + pa, &ctx.gdtr_limit, 2);
                memcpy(ram + pa + 2, &ctx.gdtr_base, 4);
            } else if (ctx.mmio) {
                ctx.mmio->write(pa, ctx.gdtr_limit, 2);
                ctx.mmio->write(pa + 2, ctx.gdtr_base, 4);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_SIDT: {
        // SIDT [mem] — store IDT base+limit to 6-byte memory operand
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, true);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 2); return; }
            }
            if (pa + 6 <= GUEST_RAM_SIZE) {
                memcpy(ram + pa, &ctx.idtr_limit, 2);
                memcpy(ram + pa + 2, &ctx.idtr_base, 4);
            } else if (ctx.mmio) {
                ctx.mmio->write(pa, ctx.idtr_limit, 2);
                ctx.mmio->write(pa + 2, ctx.idtr_base, 4);
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

        // MOV DRn, r32 / MOV r32, DRn — debug registers.
        auto dr_index = [](ZydisRegister r) -> int {
            switch (r) {
            case ZYDIS_REGISTER_DR0: return 0;
            case ZYDIS_REGISTER_DR1: return 1;
            case ZYDIS_REGISTER_DR2: return 2;
            case ZYDIS_REGISTER_DR3: return 3;
            case ZYDIS_REGISTER_DR4: return 4; // alias for DR6 when CR4.DE=0
            case ZYDIS_REGISTER_DR5: return 5; // alias for DR7 when CR4.DE=0
            case ZYDIS_REGISTER_DR6: return 6;
            case ZYDIS_REGISTER_DR7: return 7;
            default: return -1;
            }
        };

        // MOV DRn, r32
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            int di = dr_index(ops[0].reg.value);
            int gp = gp_index(ops[1].reg.value);
            if (di >= 0 && gp >= 0) {
                // DR4/DR5 alias to DR6/DR7 when CR4.DE (bit 3) is clear
                if (di == 4 && !(ctx.cr4 & 8)) di = 6;
                if (di == 5 && !(ctx.cr4 & 8)) di = 7;
                ctx.dr[di] = ctx.gp[gp];
                ctx.eip += insn.length;
                return;
            }
            // MOV r32, DRn
            di = dr_index(ops[1].reg.value);
            gp = gp_index(ops[0].reg.value);
            if (di >= 0 && gp >= 0) {
                if (di == 4 && !(ctx.cr4 & 8)) di = 6;
                if (di == 5 && !(ctx.cr4 & 8)) di = 7;
                ctx.gp[gp] = ctx.dr[di];
                ctx.eip += insn.length;
                return;
            }
        }

        // MOV sreg, r/m16 — load segment register.
        // Xbox flat model: selector value stored, base stays 0 (except FS/GS).
        auto seg_sel_ptr = [&](ZydisRegister r) -> uint16_t* {
            switch (r) {
            case ZYDIS_REGISTER_ES: return &ctx.es_sel;
            case ZYDIS_REGISTER_CS: return &ctx.cs_sel;
            case ZYDIS_REGISTER_SS: return &ctx.ss_sel;
            case ZYDIS_REGISTER_DS: return &ctx.ds_sel;
            case ZYDIS_REGISTER_FS: return &ctx.fs_sel;
            case ZYDIS_REGISTER_GS: return &ctx.gs_sel;
            default: return nullptr;
            }
        };

        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            // MOV sreg, r16
            uint16_t* sp = seg_sel_ptr(ops[0].reg.value);
            if (sp) {
                uint16_t val = 0;
                if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    int gi = gp_index(ops[1].reg.value);
                    // Accept both 16-bit and 32-bit forms (Zydis may decode as AX or EAX)
                    if (gi < 0) {
                        // Try 16-bit register → same encoding
                        uint8_t enc;
                        if (reg32_enc((ZydisRegister)(ops[1].reg.value +
                            (ZYDIS_REGISTER_EAX - ZYDIS_REGISTER_AX)), enc))
                            gi = (int)enc;
                    }
                    if (gi >= 0) val = (uint16_t)ctx.gp[gi];
                } else if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                    // MOV sreg, [mem] — read 16 bits from guest memory
                    uint32_t ea = compute_ea(ops[1], ctx);
                    uint32_t pa = ea;
                    if (paging_enabled()) {
                        pa = translate_va(ea, false);
                        if (pa == ~0u) {
                            deliver_interrupt(14, ctx.eip, true, 0);
                            return;
                        }
                    }
                    if (pa < GUEST_RAM_SIZE)
                        memcpy(&val, ram + pa, 2);
                    else if (ctx.mmio)
                        val = (uint16_t)ctx.mmio->read(pa, 2);
                }
                *sp = val;
                // Update segment base for FS/GS from GDT.
                if (ops[0].reg.value == ZYDIS_REGISTER_FS ||
                    ops[0].reg.value == ZYDIS_REGISTER_GS) {
                    uint32_t base = load_segment_base(val);
                    if (ops[0].reg.value == ZYDIS_REGISTER_FS)
                        ctx.fs_base = base;
                    else
                        ctx.gs_base = base;
                }
                ctx.eip += insn.length;
                return;
            }

            // MOV r16, sreg — read segment register
            int gi = gp_index(ops[0].reg.value);
            if (gi < 0) {
                uint8_t enc;
                if (reg32_enc((ZydisRegister)(ops[0].reg.value +
                    (ZYDIS_REGISTER_EAX - ZYDIS_REGISTER_AX)), enc))
                    gi = (int)enc;
            }
            if (gi >= 0) {
                uint16_t* sp = seg_sel_ptr(ops[1].reg.value);
                if (sp) {
                    ctx.gp[gi] = (ctx.gp[gi] & 0xFFFF0000u) | *sp;
                    ctx.eip += insn.length;
                    return;
                }
            }
        }

        fprintf(stderr, "[exec] unhandled MOV CR/DR/SEG at EIP=%08X\n", ctx.eip);
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_INVLPG: {
        // INVLPG m — invalidate TLB entry for the page containing EA.
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            uint32_t ea = compute_ea(ops[0], ctx);
            tlb.flush_va(ea);
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_RDPMC: {
        // RDPMC: read performance counter ECX into EDX:EAX.
        // Xbox kernel initialises PMCs but doesn't depend on values.
        ctx.gp[GP_EAX] = 0;
        ctx.gp[GP_EDX] = 0;
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
        // In HLE/XBE mode with no IDT set up, treat INT3 as "thread exit":
        // halt so the test_runner can dispatch pending threads.
        if (hle_handler && ctx.idtr_limit == 0) {
            ctx.eip += insn.length;
            ctx.halted = true;
            return;
        }
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

    // String I/O: INSB/INSW/INSD (with optional REP prefix)
    case ZYDIS_MNEMONIC_INSB:
    case ZYDIS_MNEMONIC_INSW:
    case ZYDIS_MNEMONIC_INSD: {
        uint16_t port = (uint16_t)ctx.gp[GP_EDX];
        unsigned sz = (insn.mnemonic == ZYDIS_MNEMONIC_INSB) ? 1 :
                      (insn.mnemonic == ZYDIS_MNEMONIC_INSW) ? 2 : 4;
        bool has_rep = (insn.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
        uint32_t count = has_rep ? ctx.gp[GP_ECX] : 1;
        bool df = (ctx.eflags & 0x400) != 0; // direction flag
        int step = df ? -(int)sz : (int)sz;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t val = io_read(port, sz);
            uint32_t ea = ctx.gp[GP_EDI];
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, true);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 2); return; }
            }
            if (pa < GUEST_RAM_SIZE) memcpy(ram + pa, &val, sz);
            else if (ctx.mmio) ctx.mmio->write(pa, val, sz);
            ctx.gp[GP_EDI] += step;
        }
        if (has_rep) ctx.gp[GP_ECX] = 0;
        ctx.eip += insn.length;
        return;
    }

    // String I/O: OUTSB/OUTSW/OUTSD (with optional REP prefix)
    case ZYDIS_MNEMONIC_OUTSB:
    case ZYDIS_MNEMONIC_OUTSW:
    case ZYDIS_MNEMONIC_OUTSD: {
        uint16_t port = (uint16_t)ctx.gp[GP_EDX];
        unsigned sz = (insn.mnemonic == ZYDIS_MNEMONIC_OUTSB) ? 1 :
                      (insn.mnemonic == ZYDIS_MNEMONIC_OUTSW) ? 2 : 4;
        bool has_rep = (insn.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
        uint32_t count = has_rep ? ctx.gp[GP_ECX] : 1;
        bool df = (ctx.eflags & 0x400) != 0;
        int step = df ? -(int)sz : (int)sz;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t ea = ctx.gp[GP_ESI];
            uint32_t pa = ea;
            if (paging_enabled()) {
                pa = translate_va(ea, false);
                if (pa == ~0u) { deliver_interrupt(14, ctx.eip, true, 0); return; }
            }
            uint32_t val = 0;
            if (pa < GUEST_RAM_SIZE) memcpy(&val, ram + pa, sz);
            else if (ctx.mmio) val = ctx.mmio->read(pa, sz);
            io_write(port, val, sz);
            ctx.gp[GP_ESI] += step;
        }
        if (has_rep) ctx.gp[GP_ECX] = 0;
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
