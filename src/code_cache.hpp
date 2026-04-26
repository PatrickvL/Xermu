#pragma once
#include "platform.hpp"
#include <cstring>
#include <cassert>
#include <cstdint>

static constexpr size_t CODE_CACHE_BYTES = 32u * 1024u * 1024u; // 32 MB for JIT code
static constexpr size_t STUB_SLAB_BYTES  = 1u  * 1024u * 1024u; // 1 MB for MMIO slow-path stubs

struct CodeCache {
    uint8_t* base      = nullptr;
    size_t   cap       = 0;
    size_t   used      = 0;

    // Stub slab: occupies the top of the same RWX allocation, after the JIT
    // code area.  Proximity guarantees CALL rel32 reaches from any JIT trace.
    uint8_t* stub_base = nullptr;
    size_t   stub_cap  = 0;
    size_t   stub_used = 0;

    bool init(size_t jit_capacity  = CODE_CACHE_BYTES,
              size_t stub_capacity = STUB_SLAB_BYTES) {
        const size_t total = jit_capacity + stub_capacity;
        base = static_cast<uint8_t*>(platform::alloc_exec(total));
        if (!base) return false;
        cap       = jit_capacity;
        used      = 0;
        stub_base = base + jit_capacity;
        stub_cap  = stub_capacity;
        stub_used = 0;
        return true;
    }

    void destroy() {
        if (base) {
            platform::free_exec(base, cap + stub_cap);
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

    // Carve out n bytes in the stub slab, 16-byte aligned. Returns null on overflow.
    uint8_t* alloc_stub(size_t n) {
        size_t a = (stub_used + 15u) & ~15ull;
        if (a + n > stub_cap) return nullptr;
        uint8_t* p = stub_base + a;
        stub_used = a + n;
        return p;
    }

    void reset() { used = 0; stub_used = 0; }

    // Check if a host address is within the code cache or stub slab.
    bool contains(const uint8_t* addr) const {
        return addr >= base && addr < base + cap + stub_cap;
    }
};
