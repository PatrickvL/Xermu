#pragma once
// Xbox physical address map constants.
#include <cstdint>

namespace xbox {

static constexpr uint32_t RAM_SIZE_RETAIL   = 64u * 1024u * 1024u;   // 64 MB
static constexpr uint32_t RAM_SIZE_DEVKIT   = 128u * 1024u * 1024u;  // 128 MB

static constexpr uint32_t RAM_MIRROR_BASE   = 0x0C000000u;
static constexpr uint32_t RAM_MIRROR_SIZE   = 0x08000000u;  // 128 MB

static constexpr uint32_t FLASH_BASE        = 0xF0000000u;
static constexpr uint32_t FLASH_SIZE        = 0x00100000u;  // 1 MB

static constexpr uint32_t NV2A_BASE         = 0xFD000000u;
static constexpr uint32_t NV2A_SIZE         = 0x01000000u;  // 16 MB

static constexpr uint32_t APU_BASE          = 0xFE800000u;
static constexpr uint32_t APU_SIZE          = 0x00400000u;  // 4 MB

static constexpr uint32_t IOAPIC_BASE       = 0xFEC00000u;
static constexpr uint32_t IOAPIC_SIZE       = 0x00001000u;  // 4 KB

static constexpr uint32_t USB0_BASE         = 0xFED00000u;
static constexpr uint32_t USB0_SIZE         = 0x00001000u;  // 4 KB
static constexpr uint32_t USB1_BASE         = 0xFED08000u;
static constexpr uint32_t USB1_SIZE         = 0x00001000u;  // 4 KB

static constexpr uint32_t BIOS_BASE         = 0xFF000000u;
static constexpr uint32_t BIOS_SIZE         = 0x01000000u;  // 16 MB (1 MB flash mirrored)

} // namespace xbox
