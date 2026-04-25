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
    uint32_t  code_pa     = 0;         // guest physical address of code (for rebuild)
    uint8_t*  host_code   = nullptr;   // pointer into CodeCache slab
    size_t    host_size   = 0;         // emitted host code size in bytes
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

    // Memory-op site table: maps host code offset → guest EIP for VEH lookup.
    // Populated during trace build at each memory-op emit site.
    static constexpr int MAX_MEM_SITES = 64;
    struct MemOpSite {
        uint32_t host_offset;   // offset from host_code where the mem op starts
        uint32_t guest_eip;     // guest instruction that emitted this memory op
    };
    MemOpSite mem_sites[MAX_MEM_SITES] = {};
    int       num_mem_sites = 0;

    void add_mem_site(uint32_t host_off, uint32_t geip) {
        if (num_mem_sites < MAX_MEM_SITES)
            mem_sites[num_mem_sites++] = { host_off, geip };
    }

    // Lookup guest EIP by host code address. Returns 0 on miss.
    uint32_t lookup_guest_eip(const uint8_t* host_addr) const {
        uint32_t off = (uint32_t)(host_addr - host_code);
        for (int i = 0; i < num_mem_sites; ++i)
            if (mem_sites[i].host_offset == off)
                return mem_sites[i].guest_eip;
        return 0;
    }

    // Lookup host code offset by guest EIP. Returns ~0u on miss.
    uint32_t lookup_host_offset(uint32_t eip) const {
        for (int i = 0; i < num_mem_sites; ++i)
            if (mem_sites[i].guest_eip == eip)
                return mem_sites[i].host_offset;
        return ~0u;
    }
};

// Two-level direct-mapped trace cache: page_map[eip >> 12][offset >> 1].
// Level 1: fixed array indexed by (guest_page & L1_MASK).
// Level 2: per-page array of 2048 Trace* slots, allocated on demand.
// Full guest_eip is verified on lookup to handle L1 aliasing.
struct TraceCache {
    static constexpr size_t L1_BITS = 15;                 // 32768 pages (128 MB)
    static constexpr size_t L1_SIZE = 1u << L1_BITS;
    static constexpr size_t L1_MASK = L1_SIZE - 1;
    static constexpr size_t L2_SIZE = 2048;               // 4096 >> 1

    struct PageTable {
        Trace* slots[L2_SIZE] = {};
    };

    PageTable* page_map[L1_SIZE] = {};

    TraceCache() = default;
    ~TraceCache() { destroy(); }

    // Free all page tables (destructor only — after this the cache is unusable).
    void destroy() {
        for (size_t i = 0; i < L1_SIZE; ++i) {
            delete page_map[i];
            page_map[i] = nullptr;
        }
    }

    // Mark every cached trace invalid and null all slots.
    // Page tables are kept allocated for reuse.
    void clear() {
        for (size_t i = 0; i < L1_SIZE; ++i) {
            if (!page_map[i]) continue;
            for (size_t j = 0; j < L2_SIZE; ++j) {
                Trace* t = page_map[i]->slots[j];
                if (t) { t->valid = false; page_map[i]->slots[j] = nullptr; }
            }
        }
    }

    void insert(Trace* t) {
        size_t l1 = (t->guest_eip >> 12) & L1_MASK;
        size_t l2 = (t->guest_eip & 0xFFF) >> 1;
        if (!page_map[l1]) page_map[l1] = new PageTable{};
        page_map[l1]->slots[l2] = t;
    }

    Trace* lookup(uint32_t eip) const {
        size_t l1 = (eip >> 12) & L1_MASK;
        if (!page_map[l1]) return nullptr;
        Trace* t = page_map[l1]->slots[(eip & 0xFFF) >> 1];
        return (t && t->guest_eip == eip) ? t : nullptr;
    }

    void invalidate(uint32_t eip) {
        size_t l1 = (eip >> 12) & L1_MASK;
        if (!page_map[l1]) return;
        size_t l2 = (eip & 0xFFF) >> 1;
        Trace* t = page_map[l1]->slots[l2];
        if (t && t->guest_eip == eip) {
            t->valid = false;
            page_map[l1]->slots[l2] = nullptr;
        }
    }
};
