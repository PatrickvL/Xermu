#pragma once
// NV2A tiling alias — RAM mirror at 0x0C000000..0x13FFFFFF.
#include "address_map.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

static uint32_t ram_mirror_read(uint32_t pa, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    uint32_t val = 0;
    memcpy(&val, ram + off, size);
    return val;
}

static void ram_mirror_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    memcpy(ram + off, &val, size);
}

} // namespace xbox
