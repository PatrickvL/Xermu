#pragma once
#include "platform.hpp"
#include <cstring>
#include <cassert>
#include <cstdint>

static constexpr size_t CODE_CACHE_BYTES = 32u * 1024u * 1024u; // 32 MB

struct CodeCache {
    uint8_t* base  = nullptr;
    size_t   cap   = 0;
    size_t   used  = 0;

    bool init(size_t capacity = CODE_CACHE_BYTES) {
        base = static_cast<uint8_t*>(platform::alloc_exec(capacity));
        if (!base) return false;
        cap  = capacity;
        used = 0;
        return true;
    }

    void destroy() {
        if (base) { platform::free_exec(base, cap); base = nullptr; }
    }

    // Carve out n bytes, 16-byte aligned. Returns null on overflow.
    uint8_t* alloc(size_t n) {
        size_t a = (used + 15u) & ~15ull;
        if (a + n > cap) return nullptr;
        uint8_t* p = base + a;
        used = a + n;
        return p;
    }

    void reset() { used = 0; }
};
