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
// Generate a slow-path MMIO dispatch stub for the given patchable mem site.
//
// The stub is called via CALL rel32 from the 5-byte NOP sled that precedes
// the fastmem instruction.  On entry:
//   [RSP]  = return address = address of fastmem instruction in host code
//   R13    = GuestContext*
//   R14D   = physical address of the memory operation
//   host GP registers hold current guest register values
//
// The stub saves GP regs, calls the appropriate MMIO helper, restores GP
// regs, advances the return address past the fastmem instruction by
// site.skip_len bytes, then returns.
//
// Returns the number of bytes written into buf[0..cap), or 0 on failure.
// ---------------------------------------------------------------------------
size_t generate_slow_path_stub(uint8_t* buf, size_t cap,
                                const Trace::MemOpSite& site);
