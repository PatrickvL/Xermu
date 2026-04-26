#pragma once
#include "platform.hpp"
#include <cstring>
#include <cassert>
#include <cstdint>

static constexpr size_t CODE_CACHE_BYTES   = 32u * 1024u * 1024u; // 32 MB for JIT code
static constexpr size_t HELPER_PAGE_BYTES  = 4096u;               // pre-generated MMIO helpers
static constexpr size_t THUNK_SLAB_BYTES   = 64u * 1024u;         // write-imm per-site thunks

struct CodeCache {
    uint8_t* base       = nullptr;
    size_t   cap        = 0;
    size_t   used       = 0;

    // Helper page: pre-generated shared MMIO slow-path stubs (48 read/write +
    // 3 write_imm tails).  Sits right after the JIT code area.
    uint8_t* helper_page = nullptr;

    // Thunk slab: small per-site thunks for write-imm (MOV R15D,imm + JMP).
    // Sits after the helper page in the same RWX allocation.
    uint8_t* thunk_base = nullptr;
    size_t   thunk_cap  = 0;
    size_t   thunk_used = 0;

    bool init(size_t jit_capacity   = CODE_CACHE_BYTES,
              size_t helper_bytes   = HELPER_PAGE_BYTES,
              size_t thunk_capacity = THUNK_SLAB_BYTES) {
        const size_t total = jit_capacity + helper_bytes + thunk_capacity;
        base = static_cast<uint8_t*>(platform::alloc_exec(total));
        if (!base) return false;
        cap         = jit_capacity;
        used        = 0;
        helper_page = base + jit_capacity;
        thunk_base  = helper_page + helper_bytes;
        thunk_cap   = thunk_capacity;
        thunk_used  = 0;
        return true;
    }

    void destroy() {
        if (base) {
            platform::free_exec(base, cap + HELPER_PAGE_BYTES + thunk_cap);
            base = nullptr;
        }
    }

    // Carve out n bytes of JIT code space, 16-byte aligned. Returns null on overflow.
    uint8_t* alloc(size_t n) {
        size_t a = (used + 15u) & ~15ull;
        if (a + n > cap) return nullptr;
        uint8_t* p = base + a;
        used = a + n;
        return p;
    }

    // Carve out n bytes in the thunk slab, 16-byte aligned. Returns null on overflow.
    uint8_t* alloc_thunk(size_t n) {
        size_t a = (thunk_used + 15u) & ~15ull;
        if (a + n > thunk_cap) return nullptr;
        uint8_t* p = thunk_base + a;
        thunk_used = a + n;
        return p;
    }

    void reset() { used = 0; thunk_used = 0; }

    // Check if a host address is within the code cache, helper page, or thunk slab.
    bool contains(const uint8_t* addr) const {
        return addr >= base && addr < base + cap + HELPER_PAGE_BYTES + thunk_cap;
    }
};
