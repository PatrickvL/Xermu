#pragma once
// 1 MB flash ROM stub (BIOS).
#include "address_map.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

struct FlashState {
    uint8_t data[FLASH_SIZE];
    FlashState() { memset(data, 0xFF, FLASH_SIZE); }
};

static uint32_t flash_read(uint32_t pa, unsigned size, void* user) {
    auto* f = static_cast<FlashState*>(user);
    uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
    uint32_t val = 0;
    memcpy(&val, f->data + off, size);
    return val;
}

static void flash_write(uint32_t, uint32_t, unsigned, void*) {}

} // namespace xbox
