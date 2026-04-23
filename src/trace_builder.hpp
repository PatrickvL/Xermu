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

// Per-page SMC version counter.
struct PageVersions {
    static constexpr size_t PAGES = 1u << 20; // 4 GB / 4 KB
    uint32_t ver[PAGES] = {};
    void bump(uint32_t pa) { ver[pa >> 12]++; }
    uint32_t get(uint32_t pa) const { return ver[pa >> 12]; }
};

struct TraceBuilder {
    TraceBuilder();

    // Build and emit a trace starting at guest_eip.
    Trace* build(uint32_t           guest_eip,
                 const uint8_t*     ram,
                 uint32_t           ram_size,
                 CodeCache&         cc,
                 TraceArena&        arena,
                 const PageVersions& pv,
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
