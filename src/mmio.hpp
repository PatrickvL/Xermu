#pragma once
#include <cstdint>

using MmioReadFn  = uint32_t (*)(uint32_t pa, unsigned size, void* user);
using MmioWriteFn = void     (*)(uint32_t pa, uint32_t val, unsigned size, void* user);

struct MmioRegion {
    uint32_t    base;
    uint32_t    size;
    MmioReadFn  read;
    MmioWriteFn write;
    void*       user;
};

struct MmioMap {
    static constexpr int MAX = 32;
    static constexpr uint32_t BUS_FLOAT = 0xFFFF'FFFFu; // unmapped read returns all-ones
    MmioRegion regions[MAX];
    int        count = 0;

    void add(uint32_t base, uint32_t size,
             MmioReadFn r, MmioWriteFn w, void* u = nullptr) {
        if (count < MAX)
            regions[count++] = { base, size, r, w, u };
    }

    uint32_t read(uint32_t pa, unsigned size) const {
        for (int i = 0; i < count; ++i) {
            const auto& r = regions[i];
            if (pa >= r.base && (pa - r.base) < r.size)
                return r.read(pa, size, r.user);
        }
        return BUS_FLOAT;
    }

    void write(uint32_t pa, uint32_t val, unsigned size) const {
        for (int i = 0; i < count; ++i) {
            const auto& r = regions[i];
            if (pa >= r.base && (pa - r.base) < r.size) {
                r.write(pa, val, size, r.user);
                return;
            }
        }
    }
};
