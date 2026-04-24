#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// A compiled trace: a run of guest instructions from guest_eip up to (but not
// including) the first branch/call/ret, emitted into the code cache.
//
// Link slots: each trace can have up to 2 exit edges (e.g. Jcc taken +
// fallthrough).  A link slot records the JMP rel32 patch site and the target
// guest EIP.  When the target trace is compiled, the JMP is patched to go
// directly to the target's host_code, bypassing the run loop.
struct Trace {
    uint32_t  guest_eip   = 0;
    uint8_t*  host_code   = nullptr;   // pointer into CodeCache slab
    uint32_t  page_ver    = 0;         // page_versions[guest_eip>>12] at build time
    bool      valid       = false;

    // Block linking: up to 2 exit edges.
    static constexpr int MAX_LINKS = 2;
    struct LinkSlot {
        uint8_t* jmp_rel32 = nullptr;  // address of the rel32 field in the JMP
        uint32_t target_eip = 0;       // guest EIP this edge targets
        bool     linked     = false;   // true if patched to a real target
    };
    LinkSlot links[MAX_LINKS] = {};
    int      num_links = 0;

    void add_link(uint8_t* patch_site, uint32_t target) {
        if (num_links < MAX_LINKS)
            links[num_links++] = { patch_site, target, false };
    }
};

// Flat open-addressed hash table mapping guest_eip → Trace*.
// Capacity must be a power of two.
struct TraceCache {
    static constexpr size_t CAPACITY = 1u << 16; // 65536 slots
    static constexpr uint32_t EMPTY   = 0xFFFF'FFFF;
    static constexpr uint32_t DELETED = 0xFFFF'FFFE; // tombstone

    struct Slot {
        uint32_t key  = EMPTY;
        Trace*   trace = nullptr;
    };

    Slot slots[CAPACITY] = {};

    TraceCache() { clear(); }

    void clear() {
        for (auto& s : slots) { s.key = EMPTY; s.trace = nullptr; }
    }

    // Insert (overwrites existing entry for the same eip).
    void insert(Trace* t) {
        uint32_t h = hash(t->guest_eip);
        while (slots[h].key != EMPTY && slots[h].key != DELETED &&
               slots[h].key != t->guest_eip)
            h = (h + 1) & (CAPACITY - 1);
        slots[h] = { t->guest_eip, t };
    }

    // Returns nullptr on miss.
    Trace* lookup(uint32_t eip) const {
        uint32_t h = hash(eip);
        while (slots[h].key != EMPTY) {
            if (slots[h].key == eip) return slots[h].trace;
            h = (h + 1) & (CAPACITY - 1);
        }
        return nullptr;
    }

    void invalidate(uint32_t eip) {
        uint32_t h = hash(eip);
        while (slots[h].key != EMPTY) {
            if (slots[h].key == eip) {
                if (slots[h].trace) slots[h].trace->valid = false;
                slots[h].key   = DELETED;
                slots[h].trace = nullptr;
                return;
            }
            h = (h + 1) & (CAPACITY - 1);
        }
    }

    static uint32_t hash(uint32_t k) {
        k ^= k >> 16;
        k *= 0x45d9f3b;
        k ^= k >> 16;
        return k & (CAPACITY - 1);
    }
};
