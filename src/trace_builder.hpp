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
    // Returns the Trace on success, nullptr on error or unsupported instruction.
    // `ram` must point to the start of guest physical RAM (same as fastmem_base).
    Trace* build(uint32_t           guest_eip,
                 const uint8_t*     ram,
                 uint32_t           ram_size,
                 CodeCache&         cc,
                 TraceArena&        arena,
                 const PageVersions& pv,
                 GuestContext*      ctx);   // ctx needed for MMIO handler ptrs

private:
    ZydisDecoder        decoder_;

    // Maps Zydis conditional-jump mnemonic to the 1-byte short-Jcc opcode.
    static uint8_t jcc_short_opcode(ZydisMnemonic m);

    // Returns true if the instruction is a trace terminator (branch/call/ret).
    static bool is_terminator(ZydisMnemonic m);

    // Returns true if the instruction touches memory (has a MEM operand).
    static bool has_mem_operand(const ZydisDecodedOperand* ops, uint8_t count,
                                 int& mem_op_idx);

    // Returns true for privileged instructions we must trap.
    static bool is_privileged(ZydisMnemonic m);

    // Emit a full inline memory-access dispatch for one instruction.
    // Handles both load and store, all common sizes.
    bool emit_mem_dispatch(Emitter&                    e,
                           const ZydisDecodedInstruction& insn,
                           const ZydisDecodedOperand*  ops,
                           int                          mem_idx,
                           GuestContext*                ctx,
                           bool                         save_flags);

    // Emit PUSH/POP sequences (they modify ESP inline).
    bool emit_push(Emitter& e, const ZydisDecodedInstruction& insn,
                   const ZydisDecodedOperand* ops, GuestContext* ctx,
                   bool save_flags);
    bool emit_pop(Emitter& e, const ZydisDecodedInstruction& insn,
                  const ZydisDecodedOperand* ops, GuestContext* ctx,
                  bool save_flags);

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
