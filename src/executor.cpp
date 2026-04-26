#include "executor.hpp"
#include "fault_handler.hpp"
#include <Zydis/Zydis.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// VEH handler for 4 GB fastmem faults (Windows only).
// ---------------------------------------------------------------------------
#ifdef _WIN32
LONG CALLBACK fastmem_veh_handler(EXCEPTION_POINTERS* ep) {
    if (!g_active_executor) return EXCEPTION_CONTINUE_SEARCH;

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

    // Only handle access violations below this point.
    if (code != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    auto fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];

    // Step 1: Is the fault within the fastmem window?
    auto base = (uintptr_t)exec->ctx.fastmem_base;
    if (fault_addr < base || fault_addr >= base + 0x100000000ULL)
        return EXCEPTION_CONTINUE_SEARCH;

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
    if (!exec->cc.contains(rip))
        return EXCEPTION_CONTINUE_SEARCH;

    // Step 3: Find the trace containing RIP (linear scan — VEH is rare).
    Trace* faulting_trace = nullptr;
    {
        auto& tcache = exec->tcache;
        for (size_t p = 0; p < TraceCache::L1_SIZE; ++p) {
            auto* page = tcache.page_map[p];
            if (!page) continue;
            for (size_t s = 0; s < TraceCache::L2_SIZE; ++s) {
                Trace* t = page->slots[s];
                if (!t || !t->valid || !t->host_code) continue;
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

        // --- Arithmetic ALU: ADD/SUB [mem], reg/imm  or  reg, [mem] ---
        if (mnem == ZYDIS_MNEMONIC_ADD || mnem == ZYDIS_MNEMONIC_SUB) {
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
            uint32_t result;
            uint32_t fl;
            if (mnem == ZYDIS_MNEMONIC_ADD) {
                result = a + b;
                fl = compute_add_flags(a, b, op_size);
            } else {
                result = a - b;
                fl = compute_sub_flags(a, b, op_size);
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

        // Unhandled non-patchable mnemonic — fall through to crash.
        fprintf(stderr, "[veh] MMIO fault at guest EIP %08X — unhandled non-patchable op (mnemonic=%d)\n",
                site->guest_eip, (int)mnem);
        return EXCEPTION_CONTINUE_SEARCH;
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
        // Fallback: 320 MB aliased window.
        fastmem_window = platform::alloc_fastmem_window(FASTMEM_WINDOW_SIZE,
                                                         GUEST_RAM_SIZE);
    }
    if (fastmem_window.base) {
        ram = static_cast<uint8_t*>(fastmem_window.base);
    } else {
        // Fallback: plain allocation (mirror goes through MMIO slow path).
        ram = static_cast<uint8_t*>(platform::alloc_ram(GUEST_RAM_SIZE));
    }
    if (!ram) return false;
    memset(ram, 0xCC, GUEST_RAM_SIZE); // INT3 fill (debug aid)

    if (!cc.init()) return false;

    // Pre-generate shared MMIO slow-path helpers into the helper page.
    generate_mmio_helpers(cc.helper_page, HELPER_PAGE_BYTES, mmio_helpers);

    memset(&ctx, 0, sizeof(ctx));
    ctx.fastmem_base = (uint64_t)(uintptr_t)ram;
    ctx.mmio         = mmio;
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
                memcpy(&ctx.gdtr_limit, ram + ea, 2);
                memcpy(&ctx.gdtr_base,  ram + ea + 2, 4);
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
                memcpy(&ctx.idtr_limit, ram + ea, 2);
                memcpy(&ctx.idtr_base,  ram + ea + 2, 4);
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
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 2 <= GUEST_RAM_SIZE) {
                memcpy(&sel, ram + ea, 2);
            }
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
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 2 <= GUEST_RAM_SIZE) {
                memcpy(&sel, ram + ea, 2);
            }
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
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 2 <= GUEST_RAM_SIZE) {
                uint16_t v = ctx.ldtr_sel;
                memcpy(ram + ea, &v, 2);
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
            uint32_t ea = 0;
            if (ops[0].mem.disp.has_displacement)
                ea = (uint32_t)ops[0].mem.disp.value;
            if (ops[0].mem.base != ZYDIS_REGISTER_NONE) {
                uint8_t enc;
                if (reg32_enc(ops[0].mem.base, enc))
                    ea += ctx.gp[enc];
            }
            if (ea + 2 <= GUEST_RAM_SIZE) {
                uint16_t v = ctx.tr_sel;
                memcpy(ram + ea, &v, 2);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_SGDT: {
        // SGDT [mem] — store GDT base+limit to 6-byte memory operand
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
                memcpy(ram + ea, &ctx.gdtr_limit, 2);
                memcpy(ram + ea + 2, &ctx.gdtr_base, 4);
            }
        }
        ctx.eip += insn.length;
        return;
    }

    case ZYDIS_MNEMONIC_SIDT: {
        // SIDT [mem] — store IDT base+limit to 6-byte memory operand
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
                memcpy(ram + ea, &ctx.idtr_limit, 2);
                memcpy(ram + ea + 2, &ctx.idtr_base, 4);
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
    uint32_t pdir_pa = ctx.cr3 & GUEST_PAGE_MASK;
    uint32_t pdi     = (va >> 22) & 0x3FF;
    uint32_t pti     = (va >> 12) & 0x3FF;

    // Read PDE
    uint32_t pde_pa = pdir_pa + pdi * 4;
    if (pde_pa + 4 > GUEST_RAM_SIZE) goto fault;
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
        if (pt_pa + 4 > GUEST_RAM_SIZE) goto fault;
        uint32_t pte;
        memcpy(&pte, ram + pt_pa, 4);

        if (!(pte & PTE_PRESENT)) goto fault;
        if (is_write && !(pte & PTE_RW)) goto fault;

        uint32_t pa = (pte & GUEST_PAGE_MASK) | (va & 0xFFF);

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

        if (t && !t->valid) t = nullptr;

        // Build if missing.
        if (!t) {
            t = builder.build(code_pa, ram,
                              cc, arena, &ctx);
            if (!t) {
                fprintf(stderr, "[exec] build failed at EIP=%08X (PA=%08X) — halting\n", eip, code_pa);
                break;
            }
            // Override guest_eip to store the VA (for cache lookup).
            t->guest_eip = eip;
            tcache.insert(t);

            // Protect code page for SMC detection — write faults invalidate traces.
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

        // Divide-by-zero (#DE, vector 0): VEH caught host EXCEPTION_INT_DIVIDE_BY_ZERO.
        if (ctx.stop_reason == STOP_DIVIDE_ERROR) {
            prev_trace = nullptr;
            deliver_interrupt(0, ctx.eip);
            continue;
        }

        // Check for HALT condition (EIP == 0xFFFFFFFF or out of RAM).
        if (ctx.eip == 0xFFFF'FFFFu || ctx.eip >= GUEST_RAM_SIZE) {
            fprintf(stderr, "[exec] EIP=%08X out of range — halting\n", ctx.eip);
            ctx.halted = true;
            break;
        }
    }
}
