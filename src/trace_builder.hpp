#pragma once
#include "context.hpp"
#include "code_cache.hpp"
#include "trace.hpp"
#include "emitter.hpp"
#include <Zydis/Zydis.h>

// Scratch arena for Trace objects themselves (not their emitted code).
struct TraceArena {
    static constexpr size_t CAP = 65536;
    Trace  pool[CAP];
    size_t used = 0;
    Trace* alloc() { return (used < CAP) ? &pool[used++] : nullptr; }
    void   reset() { used = 0; }
};

// ---------------------------------------------------------------------------
// Software TLB for VA→PA translation when paging (CR0.PG) is enabled.
// Direct-mapped, indexed by VPN & MASK. Separate read/write arrays so that
// write permission is checked only on stores.
// ---------------------------------------------------------------------------
struct SoftTlb {
    static constexpr int BITS = 8;
    static constexpr int SIZE = 1 << BITS;   // 256 entries
    static constexpr uint32_t MASK = SIZE - 1;
    static constexpr uint32_t INVALID_TAG = 0xFFFFFFFF;

    struct Entry {
        uint32_t tag;       // VPN (VA >> 12), INVALID_TAG = empty
        uint32_t pa_page;   // physical page base (PA & ~0xFFF)
    };

    Entry read_tlb[SIZE];
    Entry write_tlb[SIZE];

    void flush() {
        for (auto& e : read_tlb)  e.tag = INVALID_TAG;
        for (auto& e : write_tlb) e.tag = INVALID_TAG;
    }
    void flush_va(uint32_t va) {
        uint32_t vpn = va >> 12;
        uint32_t idx = vpn & MASK;
        if (read_tlb[idx].tag == vpn)  read_tlb[idx].tag = INVALID_TAG;
        if (write_tlb[idx].tag == vpn) write_tlb[idx].tag = INVALID_TAG;
    }
};

struct TraceBuilder {
    TraceBuilder();

    // Build and emit a trace starting at guest_eip.
    Trace* build(uint32_t           guest_eip,
                 const uint8_t*     ram,
                 CodeCache&         cc,
                 TraceArena&        arena,
                 GuestContext*      ctx);

    // has_mem_operand is used by emit handlers — needs to be accessible
    static bool has_mem_operand(const ZydisDecodedOperand* ops, uint8_t count,
                                 int& mem_op_idx);

private:
    ZydisDecoder        decoder_;

    // Maps Zydis conditional-jump mnemonic to the 1-byte short-Jcc opcode.
    static uint8_t jcc_short_opcode(ZydisMnemonic m);

    // Emit a conditional-branch trace exit.
    void emit_cond_exit(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, uint32_t pc_after);

    // Emit an unconditional-JMP trace exit.
    void emit_jmp_exit(Emitter& e, const ZydisDecodedInstruction& insn,
                       const ZydisDecodedOperand* ops, uint32_t pc_after,
                       GuestContext* ctx);

    // Emit a CALL trace exit (pushes return addr, jumps to target).
    void emit_call_exit(Emitter& e, const ZydisDecodedInstruction& insn,
                        const ZydisDecodedOperand* ops, uint32_t pc_after,
                        GuestContext* ctx);

    // Emit a RET trace exit (reads return addr from guest stack).
    void emit_ret_exit(Emitter& e, GuestContext* ctx);
};

// ---------------------------------------------------------------------------
// Pre-generated MMIO slow-path helper page.
//
// 48 shared helpers (24 read + 24 write) indexed by [reg_enc][size_idx],
// plus 3 write_imm tails indexed by [size_idx].  All fit in one 4 KB page
// allocated alongside the JIT code cache (guaranteeing CALL rel32 reach).
//
// Each helper:  PUSHFQ → save_all_gp → setup_args → call mmio_fn →
//               load_all_gp → POPFQ → RET.
//
// The VEH decodes the faulting fastmem instruction to determine (direction,
// reg, size), looks up the helper address, and patches CALL rel32 in place.
//
// For write-imm sites: a tiny per-site thunk (MOV R15D,imm + JMP tail) is
// generated from the thunk slab; the shared write_imm tail reads R15D.
// ---------------------------------------------------------------------------
struct MmioHelpers {
    uint8_t* read_helpers[8][3]  = {};  // [reg_enc][size_idx]  size_idx: 0=1B, 1=2B, 2=4B
    uint8_t* write_helpers[8][3] = {};  // [reg_enc][size_idx]
    uint8_t* write_imm_tails[3]  = {};  // [size_idx]  (thunks JMP here)

    uint8_t* lookup_read(uint8_t reg_enc, uint8_t size_bytes) const {
        int si = (size_bytes <= 1) ? 0 : (size_bytes == 2) ? 1 : 2;
        return read_helpers[reg_enc & 7][si];
    }
    uint8_t* lookup_write(uint8_t reg_enc, uint8_t size_bytes) const {
        int si = (size_bytes <= 1) ? 0 : (size_bytes == 2) ? 1 : 2;
        return write_helpers[reg_enc & 7][si];
    }
    uint8_t* lookup_write_imm(uint8_t size_bytes) const {
        int si = (size_bytes <= 1) ? 0 : (size_bytes == 2) ? 1 : 2;
        return write_imm_tails[si];
    }
};

// Emit all shared MMIO helpers into the code-cache helper page.
// Must be called once after CodeCache::init().
void generate_mmio_helpers(uint8_t* page, size_t page_cap, MmioHelpers& out);
