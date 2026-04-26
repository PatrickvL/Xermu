// ---------------------------------------------------------------------------
// veh_handler.cpp — VEH handler for 4 GB fastmem faults (Windows only).
//
// Handles:
//   1. #DE (divide-by-zero) from native DIV/IDIV in JIT code
//   2. Access violations in the fastmem window (MMIO dispatch)
//      a. SMC write-protect detection
//      b. Non-patchable MMIO software emulation (ALU/TEST/CMP/etc.)
//      c. Patchable fastmem → CALL rel32 hot-patching
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include "fault_handler.hpp"
#include <Zydis/Zydis.h>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
LONG CALLBACK fastmem_veh_handler(EXCEPTION_POINTERS* ep) {
    if (!g_active_executor) {
        fprintf(stderr, "[veh] handler called but g_active_executor is null, code=0x%08lX\n",
                ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto  code = ep->ExceptionRecord->ExceptionCode;
    auto* ctx_regs = ep->ContextRecord;
    auto* exec = g_active_executor;

    // -----------------------------------------------------------------------
    // #DE (divide-by-zero): DIV/IDIV executed natively on host faulted.
    // Map back to guest EIP, set STOP_DIVIDE_ERROR, redirect to exit stub.
    // -----------------------------------------------------------------------
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        auto rip = (uint8_t*)(uintptr_t)ctx_regs->Rip;
        if (!exec->cc.contains(rip))
            return EXCEPTION_CONTINUE_SEARCH;

        Trace* t = nullptr;
        {
            auto& tcache = exec->tcache;
            for (size_t p = 0; p < TraceCache::L1_SIZE; ++p) {
                auto* page = tcache.page_map[p];
                if (!page) continue;
                for (size_t s = 0; s < TraceCache::L2_SIZE; ++s) {
                    Trace* tr = page->slots[s];
                    // Include invalid traces: host code is still in-cache and
                    // may have been executing when the #DE occurred.
                    if (!tr || !tr->host_code) continue;
                    if (rip >= tr->host_code && rip < tr->host_code + tr->host_size) {
                        t = tr;
                        break;
                    }
                }
                if (t) break;
            }
        }
        if (!t) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        uint32_t rip_off = (uint32_t)(rip - t->host_code);
        uint32_t guest_eip = 0;
        bool found = false;
        for (int i = 0; i < t->num_mem_sites; ++i) {
            if (rip_off >= t->mem_sites[i].host_offset &&
                rip_off <  t->mem_sites[i].host_offset + 8) {
                guest_eip = t->mem_sites[i].guest_eip;
                found = true;
                break;
            }
        }
        if (!found) return EXCEPTION_CONTINUE_SEARCH;

        // Write stop_reason and next_eip into GuestContext via R13.
        auto* gctx = reinterpret_cast<GuestContext*>(ctx_regs->R13);
        gctx->next_eip    = guest_eip;
        gctx->stop_reason  = STOP_DIVIDE_ERROR;

        // Redirect to exception_exit stub (save_all_gp + RET).
        ctx_regs->Rip = (DWORD64)(uintptr_t)exec->mmio_helpers.exception_exit;
        ctx_regs->Rip = (DWORD64)(uintptr_t)exec->mmio_helpers.exception_exit;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // -----------------------------------------------------------------------
    // EXCEPTION_ILLEGAL_INSTRUCTION: JIT code may have emitted something the
    // host CPU can't execute, or the guest has a UD2.  Diagnose + halt.
    // -----------------------------------------------------------------------
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
        auto rip = (uint8_t*)(uintptr_t)ctx_regs->Rip;
        if (exec->cc.contains(rip)) {
            fprintf(stderr, "[veh] ILLEGAL_INSTRUCTION in JIT code at RIP=%p bytes:",
                    (void*)rip);
            for (int i = 0; i < 16; ++i)
                fprintf(stderr, " %02X", rip[i]);
            fprintf(stderr, "\n");
            fprintf(stderr, "[veh]   R12(fastmem)=%p R13(ctx)=%p R14(EA)=0x%llX\n",
                    (void*)(uintptr_t)ctx_regs->R12,
                    (void*)(uintptr_t)ctx_regs->R13,
                    (unsigned long long)ctx_regs->R14);
            fprintf(stderr, "[veh]   RAX=0x%08X ECX=0x%08X EDX=0x%08X EBX=0x%08X\n",
                    (uint32_t)ctx_regs->Rax, (uint32_t)ctx_regs->Rcx,
                    (uint32_t)ctx_regs->Rdx, (uint32_t)ctx_regs->Rbx);
            // Find the trace to get guest EIP
            auto& tcache = exec->tcache;
            for (size_t p = 0; p < TraceCache::L1_SIZE; ++p) {
                auto* page = tcache.page_map[p];
                if (!page) continue;
                for (size_t s = 0; s < TraceCache::L2_SIZE; ++s) {
                    Trace* t = page->slots[s];
                    if (!t || !t->host_code) continue;
                    if (rip >= t->host_code && rip < t->host_code + t->host_size) {
                        fprintf(stderr, "[veh]   trace guest_eip=0x%08X host_off=%u\n",
                                t->guest_eip, (uint32_t)(rip - t->host_code));
                        break;
                    }
                }
            }
            // Set halt + redirect to exception_exit
            auto* gctx = reinterpret_cast<GuestContext*>(ctx_regs->R13);
            gctx->halted = true;
            gctx->stop_reason = STOP_INVALID_OPCODE;
            ctx_regs->Rip = (DWORD64)(uintptr_t)exec->mmio_helpers.exception_exit;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        fprintf(stderr, "[veh] ILLEGAL_INSTRUCTION outside code cache at RIP=%p\n",
                (void*)rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Only handle access violations below this point.
    if (code != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    auto fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];

    // Step 1: Is the fault within the fastmem window?
    auto base = (uintptr_t)exec->ctx.fastmem_base;
    if (fault_addr < base || fault_addr >= base + 0x100000000ULL) {
        fprintf(stderr, "[veh] AV outside fastmem: fault=0x%llX base=0x%llX RIP=%p access=%lld\n",
                (unsigned long long)fault_addr, (unsigned long long)base,
                (void*)(uintptr_t)ctx_regs->Rip,
                (long long)ep->ExceptionRecord->ExceptionInformation[0]);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Step 1b: SMC detection — write to a protected code page in RAM.
    // ExceptionInformation[0]: 0=read, 1=write, 8=DEP
    auto access_type = ep->ExceptionRecord->ExceptionInformation[0];
    uint32_t guest_pa = (uint32_t)(fault_addr - base);
    if (access_type == 1 && guest_pa < GUEST_RAM_SIZE &&
        exec->is_code_page(guest_pa)) {
        exec->invalidate_code_page(guest_pa);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Step 2: Is the faulting RIP within our code cache?
    auto rip = (uint8_t*)(uintptr_t)ctx_regs->Rip;
    if (!exec->cc.contains(rip)) {
        fprintf(stderr, "[veh] AV in fastmem but RIP=%p outside code cache, PA=0x%08X access=%lld\n",
                (void*)rip, guest_pa, (long long)access_type);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Step 3: Find the trace containing RIP (linear scan — VEH is rare).
    // Include invalid traces: host code is still in-cache and may have been
    // executing when the fault occurred (e.g. after SMC invalidation).
    Trace* faulting_trace = nullptr;
    {
        auto& tcache = exec->tcache;
        for (size_t p = 0; p < TraceCache::L1_SIZE; ++p) {
            auto* page = tcache.page_map[p];
            if (!page) continue;
            for (size_t s = 0; s < TraceCache::L2_SIZE; ++s) {
                Trace* t = page->slots[s];
                if (!t || !t->host_code) continue;
                if (rip >= t->host_code && rip < t->host_code + t->host_size) {
                    faulting_trace = t;
                    break;
                }
            }
            if (faulting_trace) break;
        }
    }

    if (!faulting_trace) {
        fprintf(stderr, "[veh] FAULT at RIP=%p — no matching trace\n", (void*)rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Step 4: Look up the MemOpSite for this faulting instruction.
    uint32_t rip_off = (uint32_t)(rip - faulting_trace->host_code);
    Trace::MemOpSite* site = nullptr;
    for (int i = 0; i < faulting_trace->num_mem_sites; ++i) {
        if (faulting_trace->mem_sites[i].host_offset == rip_off) {
            site = &faulting_trace->mem_sites[i];
            break;
        }
    }

    if (!site) {
        fprintf(stderr, "[veh] FAULT in trace @%08X — no mem site for RIP %p\n",
                faulting_trace->guest_eip, (void*)rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Non-patchable site (ALU/TEST/SETcc/CMOVcc/FPU/SSE MMIO).
    // Software-emulate: decode host instruction, perform MMIO read/write,
    // update registers + EFLAGS in CONTEXT, advance RIP.
    if (site->patch_len == 0) {
        uint32_t mmio_pa = (uint32_t)ctx_regs->R14;
        auto* mmio = exec->ctx.mmio;

        // Decode the host instruction with a 64-bit Zydis decoder.
        ZydisDecoder dec64;
        ZydisDecoderInit(&dec64, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        ZydisDecodedInstruction hinsn;
        ZydisDecodedOperand hops[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus zst = ZydisDecoderDecodeFull(&dec64, rip, 15, &hinsn, hops);
        if (ZYAN_FAILED(zst))
            return EXCEPTION_CONTINUE_SEARCH;

        // Map host 64-bit register to a pointer into the CONTEXT struct.
        auto reg_ptr64 = [&](ZydisRegister r) -> DWORD64* {
            switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r)) {
            case ZYDIS_REGISTER_RAX: return &ctx_regs->Rax;
            case ZYDIS_REGISTER_RCX: return &ctx_regs->Rcx;
            case ZYDIS_REGISTER_RDX: return &ctx_regs->Rdx;
            case ZYDIS_REGISTER_RBX: return &ctx_regs->Rbx;
            case ZYDIS_REGISTER_RBP: return &ctx_regs->Rbp;
            case ZYDIS_REGISTER_RSI: return &ctx_regs->Rsi;
            case ZYDIS_REGISTER_RDI: return &ctx_regs->Rdi;
            case ZYDIS_REGISTER_R8:  return &ctx_regs->R8;
            default: return nullptr;
            }
        };

        // Read a guest register value (masked to operand size).
        auto read_reg = [&](ZydisRegister r, unsigned sz) -> uint32_t {
            DWORD64* p = reg_ptr64(r);
            if (!p) return 0;
            uint64_t v = *p;
            // For 8-bit high registers (AH/CH/DH/BH), shift right 8.
            if (r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH ||
                r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH)
                v >>= 8;
            return (uint32_t)(v & ((1ULL << (sz * 8)) - 1));
        };

        // Write a value to a guest register.
        auto write_reg = [&](ZydisRegister r, uint32_t val, unsigned sz) {
            DWORD64* p = reg_ptr64(r);
            if (!p) return;
            if (r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH ||
                r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH) {
                *p = (*p & ~0xFF00ULL) | ((uint64_t)(val & 0xFF) << 8);
            } else if (sz == 1) {
                *p = (*p & ~0xFFULL) | (val & 0xFF);
            } else if (sz == 2) {
                *p = (*p & ~0xFFFFULL) | (val & 0xFFFF);
            } else {
                *p = val; // 32-bit write zero-extends to 64 in x64
            }
        };

        // Compute standard flags for result of given size.
        auto compute_logical_flags = [](uint32_t result, unsigned sz) -> uint32_t {
            uint32_t fl = 0;
            uint32_t mask = (sz == 4) ? 0xFFFFFFFF : (1u << (sz * 8)) - 1;
            result &= mask;
            if (result == 0) fl |= 0x40;   // ZF
            if (result & (1u << (sz * 8 - 1))) fl |= 0x80; // SF
            // PF: parity of low byte
            uint8_t pb = (uint8_t)result;
            pb ^= pb >> 4; pb ^= pb >> 2; pb ^= pb >> 1;
            if (!(pb & 1)) fl |= 0x04;     // PF (set if even parity)
            return fl;
        };

        // Compute full arithmetic flags (CF, OF, AF, ZF, SF, PF).
        auto compute_add_flags = [&](uint32_t a, uint32_t b, unsigned sz) -> uint32_t {
            uint64_t sum = (uint64_t)a + b;
            uint32_t result = (uint32_t)sum;
            uint32_t fl = compute_logical_flags(result, sz);
            unsigned bits = sz * 8;
            if (sum >> bits) fl |= 0x01; // CF
            // OF: sign of a == sign of b && sign of result != sign of a
            uint32_t sign_mask = 1u << (bits - 1);
            if (~(a ^ b) & (a ^ result) & sign_mask) fl |= 0x800; // OF
            if ((a ^ b ^ result) & 0x10) fl |= 0x10; // AF
            return fl;
        };

        auto compute_sub_flags = [&](uint32_t a, uint32_t b, unsigned sz) -> uint32_t {
            uint32_t result = a - b;
            uint32_t fl = compute_logical_flags(result, sz);
            unsigned bits = sz * 8;
            if (a < b) fl |= 0x01; // CF (borrow)
            uint32_t sign_mask = 1u << (bits - 1);
            if ((a ^ b) & (a ^ result) & sign_mask) fl |= 0x800; // OF
            if ((a ^ b ^ result) & 0x10) fl |= 0x10; // AF
            return fl;
        };

        // Update EFLAGS: preserve non-arithmetic flags, replace status flags.
        constexpr uint32_t STATUS_MASK = 0x8D5; // CF|PF|AF|ZF|SF|OF
        auto set_status_flags = [&](uint32_t fl) {
            ctx_regs->EFlags = (ctx_regs->EFlags & ~STATUS_MASK) | (fl & STATUS_MASK);
        };

        // Determine operand size.  For non-patchable fastmem the encoding is:
        //   [66?] REX opcode ModRM SIB [disp] [imm]
        // Zydis gives us operand sizes directly.
        unsigned op_size = 4;
        // Find memory operand to determine size.
        for (int i = 0; i < (int)hinsn.operand_count_visible; ++i) {
            if (hops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                op_size = hops[i].size / 8;
                if (op_size == 0) op_size = 4;
                break;
            }
        }

        // Identify the instruction category and emulate.
        auto mnem = hinsn.mnemonic;

        // --- TEST reg/imm, [mem] or TEST [mem], reg/imm ---
        if (mnem == ZYDIS_MNEMONIC_TEST) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            uint32_t other = 0;
            for (int i = 0; i < (int)hinsn.operand_count_visible; ++i) {
                if (hops[i].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    other = read_reg(hops[i].reg.value, op_size);
                else if (hops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                    other = (uint32_t)hops[i].imm.value.u;
            }
            uint32_t result = mem_val & other;
            set_status_flags(compute_logical_flags(result, op_size));
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- CMP [mem], reg/imm  or  CMP reg, [mem] ---
        if (mnem == ZYDIS_MNEMONIC_CMP) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            // CMP op0, op1 → op0 - op1
            uint32_t a, b;
            if (hops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                a = mem_val;
                if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    b = read_reg(hops[1].reg.value, op_size);
                else
                    b = (uint32_t)hops[1].imm.value.u;
            } else {
                a = read_reg(hops[0].reg.value, op_size);
                b = mem_val;
            }
            set_status_flags(compute_sub_flags(a, b, op_size));
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- Logical ALU: AND/OR/XOR [mem], reg/imm  or  reg, [mem] ---
        if (mnem == ZYDIS_MNEMONIC_AND || mnem == ZYDIS_MNEMONIC_OR ||
            mnem == ZYDIS_MNEMONIC_XOR) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            bool mem_is_dst = (hops[0].type == ZYDIS_OPERAND_TYPE_MEMORY);
            uint32_t other;
            if (mem_is_dst) {
                if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    other = read_reg(hops[1].reg.value, op_size);
                else
                    other = (uint32_t)hops[1].imm.value.u;
            } else {
                other = read_reg(hops[0].reg.value, op_size);
            }
            uint32_t result;
            if (mnem == ZYDIS_MNEMONIC_AND) result = mem_val & other;
            else if (mnem == ZYDIS_MNEMONIC_OR) result = mem_val | other;
            else result = mem_val ^ other;

            set_status_flags(compute_logical_flags(result, op_size));
            if (mem_is_dst)
                mmio->write(mmio_pa, result, op_size);
            else
                write_reg(hops[0].reg.value, result, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- Arithmetic ALU: ADD/SUB/ADC/SBB [mem], reg/imm  or  reg, [mem] ---
        if (mnem == ZYDIS_MNEMONIC_ADD || mnem == ZYDIS_MNEMONIC_SUB ||
            mnem == ZYDIS_MNEMONIC_ADC || mnem == ZYDIS_MNEMONIC_SBB) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            bool mem_is_dst = (hops[0].type == ZYDIS_OPERAND_TYPE_MEMORY);
            uint32_t a, b;
            if (mem_is_dst) {
                a = mem_val;
                if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    b = read_reg(hops[1].reg.value, op_size);
                else
                    b = (uint32_t)hops[1].imm.value.u;
            } else {
                a = read_reg(hops[0].reg.value, op_size);
                b = mem_val;
            }
            uint32_t carry = (ctx_regs->EFlags & 0x01u); // current CF
            uint32_t result;
            uint32_t fl;
            if (mnem == ZYDIS_MNEMONIC_ADD) {
                result = a + b;
                fl = compute_add_flags(a, b, op_size);
            } else if (mnem == ZYDIS_MNEMONIC_ADC) {
                uint64_t sum = (uint64_t)a + b + carry;
                result = (uint32_t)sum;
                fl = compute_logical_flags(result, op_size);
                unsigned bits = op_size * 8;
                if (sum >> bits) fl |= 0x01; // CF
                uint32_t sign_mask = 1u << (bits - 1);
                if (~(a ^ b) & (a ^ result) & sign_mask) fl |= 0x800; // OF
                if ((a ^ b ^ result) & 0x10) fl |= 0x10; // AF
            } else if (mnem == ZYDIS_MNEMONIC_SUB) {
                result = a - b;
                fl = compute_sub_flags(a, b, op_size);
            } else { // SBB
                uint64_t diff = (uint64_t)a - b - carry;
                result = (uint32_t)diff;
                fl = compute_logical_flags(result, op_size);
                unsigned bits = op_size * 8;
                if (a < (uint64_t)b + carry) fl |= 0x01; // CF (borrow)
                uint32_t sign_mask = 1u << (bits - 1);
                if ((a ^ b) & (a ^ result) & sign_mask) fl |= 0x800; // OF
                if ((a ^ b ^ result) & 0x10) fl |= 0x10; // AF
            }
            set_status_flags(fl);
            if (mem_is_dst)
                mmio->write(mmio_pa, result, op_size);
            else
                write_reg(hops[0].reg.value, result, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- BT/BTS/BTR/BTC [mem], reg/imm ---
        if (mnem == ZYDIS_MNEMONIC_BT  || mnem == ZYDIS_MNEMONIC_BTS ||
            mnem == ZYDIS_MNEMONIC_BTR || mnem == ZYDIS_MNEMONIC_BTC) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            uint32_t bit;
            if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                bit = read_reg(hops[1].reg.value, op_size);
            else
                bit = (uint32_t)hops[1].imm.value.u;
            bit &= (op_size * 8 - 1);
            uint32_t mask = 1u << bit;
            // CF = selected bit
            uint32_t fl = (ctx_regs->EFlags & ~0x01u) | ((mem_val >> bit) & 1);
            ctx_regs->EFlags = fl;
            if (mnem == ZYDIS_MNEMONIC_BTS)
                mmio->write(mmio_pa, mem_val | mask, op_size);
            else if (mnem == ZYDIS_MNEMONIC_BTR)
                mmio->write(mmio_pa, mem_val & ~mask, op_size);
            else if (mnem == ZYDIS_MNEMONIC_BTC)
                mmio->write(mmio_pa, mem_val ^ mask, op_size);
            // BT: no write-back
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- INC/DEC [mem] ---
        if (mnem == ZYDIS_MNEMONIC_INC || mnem == ZYDIS_MNEMONIC_DEC) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            uint32_t result;
            uint32_t fl;
            if (mnem == ZYDIS_MNEMONIC_INC) {
                result = mem_val + 1;
                fl = compute_add_flags(mem_val, 1, op_size);
            } else {
                result = mem_val - 1;
                fl = compute_sub_flags(mem_val, 1, op_size);
            }
            // INC/DEC preserve CF.
            fl = (fl & ~0x01u) | (ctx_regs->EFlags & 0x01u);
            set_status_flags(fl);
            mmio->write(mmio_pa, result, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- NOT [mem] (no flags affected) ---
        if (mnem == ZYDIS_MNEMONIC_NOT) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            mmio->write(mmio_pa, ~mem_val, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- NEG [mem] ---
        if (mnem == ZYDIS_MNEMONIC_NEG) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            uint32_t result = (uint32_t)(-(int32_t)mem_val);
            uint32_t fl = compute_sub_flags(0, mem_val, op_size);
            set_status_flags(fl);
            mmio->write(mmio_pa, result, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- MOV [mem], reg/imm  or  MOV reg, [mem] (shouldn't normally be
        //     non-patchable, but handle for safety) ---
        if (mnem == ZYDIS_MNEMONIC_MOV) {
            if (hops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                uint32_t val;
                if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                    val = read_reg(hops[1].reg.value, op_size);
                else
                    val = (uint32_t)hops[1].imm.value.u;
                mmio->write(mmio_pa, val, op_size);
            } else {
                uint32_t val = mmio->read(mmio_pa, op_size);
                write_reg(hops[0].reg.value, val, op_size);
            }
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // --- SHL/SHR/SAR [mem], imm/CL ---
        if (mnem == ZYDIS_MNEMONIC_SHL || mnem == ZYDIS_MNEMONIC_SHR ||
            mnem == ZYDIS_MNEMONIC_SAR) {
            uint32_t mem_val = mmio->read(mmio_pa, op_size);
            uint32_t count;
            if (hops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                count = read_reg(hops[1].reg.value, 1);
            else
                count = (uint32_t)hops[1].imm.value.u;
            count &= 0x1F;
            if (count == 0) {
                ctx_regs->Rip += hinsn.length;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            unsigned bits = op_size * 8;
            uint32_t result;
            uint32_t fl = 0;
            if (mnem == ZYDIS_MNEMONIC_SHL) {
                result = mem_val << count;
                fl |= (mem_val >> (bits - count)) & 1; // CF = last bit shifted out
                if (count == 1)
                    fl |= ((result >> (bits - 1)) ^ ((result >> (bits - 1)) & 1)) ? 0 : 0; // OF
            } else if (mnem == ZYDIS_MNEMONIC_SHR) {
                result = mem_val >> count;
                fl |= (mem_val >> (count - 1)) & 1; // CF
            } else { // SAR
                int32_t sval = (int32_t)(mem_val << (32 - bits)) >> (32 - bits); // sign extend
                result = (uint32_t)(sval >> count);
                fl |= (mem_val >> (count - 1)) & 1; // CF
            }
            fl |= compute_logical_flags(result, op_size) & ~0x01u; // ZF/SF/PF, keep CF from above
            set_status_flags(fl);
            mmio->write(mmio_pa, result, op_size);
            ctx_regs->Rip += hinsn.length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Unhandled non-patchable mnemonic — halt gracefully instead of crashing.
        fprintf(stderr, "[veh] MMIO fault at guest EIP %08X — unhandled non-patchable op (mnemonic=%d)\n",
                site->guest_eip, (int)mnem);
        // Set halt flag and stop_reason so the run loop can terminate cleanly.
        // Redirect to exception_exit stub to stop immediately (avoiding
        // cascading faults from subsequent instructions in this trace).
        exec->ctx.halted = true;
        exec->ctx.stop_reason = STOP_INVALID_OPCODE;
        exec->ctx.next_eip = site->guest_eip;
        ctx_regs->Rip = (DWORD64)(uintptr_t)exec->mmio_helpers.exception_exit;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Step 5: Decode the faulting fastmem instruction to derive the helper.
    // The instruction bytes are still intact (we read before patching).
    //
    // Fastmem encodings (all use [R12+R14] via SIB 0x34):
    //   [66] 43 opcode ModRM 34 [imm...]
    //   REX.R=1 (0x47) signals esp_in_r8 (dest = ESP via R8D)
    //
    // Decode: direction (load/store), register, size → helper address.

    const uint8_t* ip = rip;
    uint8_t  decoded_reg  = 0;
    uint8_t  decoded_size = 4;
    bool     decoded_load = true;
    bool     decoded_write_imm = false;

    // Check for 0x66 operand-size prefix (16-bit).
    bool has_66 = (ip[0] == 0x66);
    const uint8_t* p = has_66 ? ip + 1 : ip;

    uint8_t rex   = p[0];           // REX byte (0x43 or 0x47)
    uint8_t opc   = p[1];           // primary opcode
    bool    rex_r = (rex & 0x04);   // REX.R set → ESP-in-R8 for MOVZX/MOVSX

    if (opc == 0x0F) {
        // Two-byte opcode: MOVZX or MOVSX
        uint8_t opc2 = p[2];
        uint8_t modrm = p[3];
        decoded_reg  = (modrm >> 3) & 7;
        decoded_load = true;
        decoded_write_imm = false;
        if (opc2 == 0xB6 || opc2 == 0xBE) decoded_size = 1; // byte src
        else                                decoded_size = 2; // word src
        if (rex_r) decoded_reg = GP_ESP; // R8 dest → ESP
    } else {
        uint8_t modrm = p[2];
        decoded_reg = (modrm >> 3) & 7;

        switch (opc) {
        case 0x8B: decoded_load = true;  decoded_size = has_66 ? 2 : 4; break;
        case 0x89: decoded_load = false; decoded_size = has_66 ? 2 : 4; break;
        case 0x8A: decoded_load = true;  decoded_size = 1; break;
        case 0x88: decoded_load = false; decoded_size = 1; break;
        case 0xC7: decoded_load = false; decoded_write_imm = true;
                   decoded_size = has_66 ? 2 : 4; break;
        case 0xC6: decoded_load = false; decoded_write_imm = true;
                   decoded_size = 1; break;
        default:
            fprintf(stderr, "[veh] unknown fastmem opcode %02X at RIP %p\n",
                    opc, (void*)rip);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // Step 6: Resolve the helper target.
    uint8_t* call_target = nullptr;

    if (decoded_write_imm) {
        // Extract the immediate from the (still-intact) instruction bytes.
        // Layout: [66?] 43 C7/C6 04 34 imm...
        const uint8_t* imm_ptr = p + 4; // after REX opcode ModRM SIB
        uint32_t imm_val = 0;
        memcpy(&imm_val, imm_ptr, decoded_size);

        // Allocate a per-site thunk: MOV R15D,imm32 + JMP write_imm_tail.
        uint8_t* thunk = exec->cc.alloc_thunk(16);
        if (!thunk) {
            fprintf(stderr, "[veh] thunk slab exhausted\n");
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // MOV R15D, imm32: 41 BF imm32 (6 bytes)
        thunk[0] = 0x41; thunk[1] = 0xBF;
        memcpy(thunk + 2, &imm_val, 4);
        // JMP rel32: E9 rel32 (5 bytes)
        uint8_t* write_imm_helper = exec->mmio_helpers.lookup_write_imm(decoded_size);
        thunk[6] = 0xE9;
        int32_t jmp_rel = (int32_t)(write_imm_helper - (thunk + 6 + 5));
        memcpy(thunk + 7, &jmp_rel, 4);

        call_target = thunk;
    } else if (decoded_load) {
        call_target = exec->mmio_helpers.lookup_read(decoded_reg, decoded_size);
    } else {
        call_target = exec->mmio_helpers.lookup_write(decoded_reg, decoded_size);
    }

    if (!call_target) {
        fprintf(stderr, "[veh] no helper for reg=%d size=%d\n",
                decoded_reg, decoded_size);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Step 7: Patch the patchable region with CALL rel32 + trailing NOPs.
    // The CALL's return address (rip + 5) lands in the NOP'd tail, which
    // slides execution to the code after the patchable region.
    uint8_t patch_len = site->patch_len;
    int32_t call_rel = (int32_t)(call_target - (rip + 5));
    rip[0] = 0xE8;
    memcpy(rip + 1, &call_rel, 4);
    for (int i = 5; i < patch_len; ++i)
        rip[i] = 0x90;

    // Step 8: Redirect RIP to the patched CALL so it executes on resume.
    ctx_regs->Rip = (DWORD64)(uintptr_t)rip;

    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif // _WIN32
