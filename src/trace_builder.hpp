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

// Per-page SMC version counter (legacy — retained for compatibility during migration).
struct PageVersions {
    static constexpr size_t PAGES = 1u << 20; // 4 GB / 4 KB
    uint32_t ver[PAGES] = {};
    void bump(uint32_t pa) { ver[pa >> 12]++; }
    uint32_t get(uint32_t pa) const { return ver[pa >> 12]; }
};

// ---------------------------------------------------------------------------
// Per-code-page fault bitmap: 1 bit per byte offset (512 bytes per page).
// When a memory-access instruction faults (VEH), its guest EIP is marked.
// On trace rebuild, marked instructions emit the slow-path CALL instead
// of the optimistic bare fastmem op.
//
// Bitmaps are allocated on demand and released when the code page is
// written to (invalidating all traces on that page).
// ---------------------------------------------------------------------------
struct FaultBitmaps {
    static constexpr size_t PAGES = 1u << 20; // 4 GB / 4 KB = 1M pages
    uint8_t* maps[PAGES] = {};

    bool test(uint32_t guest_pa) const {
        uint32_t page = guest_pa >> 12;
        uint32_t off  = guest_pa & 0xFFF;
        auto* m = maps[page];
        return m && (m[off >> 3] & (1u << (off & 7)));
    }

    void set(uint32_t guest_pa) {
        uint32_t page = guest_pa >> 12;
        uint32_t off  = guest_pa & 0xFFF;
        if (!maps[page]) {
            maps[page] = new uint8_t[512]();
        }
        maps[page][off >> 3] |= (1u << (off & 7));
    }

    void clear_page(uint32_t pa) {
        uint32_t page = pa >> 12;
        delete[] maps[page];
        maps[page] = nullptr;
    }

    ~FaultBitmaps() {
        for (size_t i = 0; i < PAGES; ++i)
            delete[] maps[i];
    }
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
                 uint32_t           ram_size,
                 CodeCache&         cc,
                 TraceArena&        arena,
                 const PageVersions& pv,
                 GuestContext*      ctx,
                 const FaultBitmaps* fb = nullptr);

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
