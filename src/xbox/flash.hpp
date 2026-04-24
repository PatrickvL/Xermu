#pragma once
// Flash ROM + MCPX hidden boot ROM.
//
// Flash (1 MB at 0xF0000000, aliased at 0xFF000000): holds the Xbox BIOS
// image (2BL + kernel init + certificate chain).  256 KB images are mirrored
// 4× within the 1 MB space.
//
// MCPX ROM (512 bytes at physical 0xFFFFFE00): secret boot ROM baked into the
// MCPX southbridge die.  Executes at the x86 reset vector (0xFFFFFFF0),
// decrypts the 2BL from flash, then jumps to it.  For HLE mode this is
// unused; for LLE mode a dump can be loaded.

#include "address_map.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace xbox {

struct FlashState {
    uint8_t data[FLASH_SIZE];

    // MCPX hidden boot ROM (512 bytes, mapped at PA 0xFFFFFE00).
    static constexpr uint32_t MCPX_ROM_SIZE = 512;
    uint8_t  mcpx_rom[MCPX_ROM_SIZE];
    bool     mcpx_loaded = false;

    FlashState() {
        memset(data, 0xFF, FLASH_SIZE);
        memset(mcpx_rom, 0xFF, MCPX_ROM_SIZE);
    }

    // Load a BIOS image from a file into the flash region.
    // Supports 256 KB (mirrored 4×) or 1 MB images.
    // Returns true on success.
    bool load_bios(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (size == 256 * 1024) {
            // 256 KB image: mirror 4× to fill 1 MB
            if (fread(data, 1, (size_t)size, f) != (size_t)size)
                { fclose(f); return false; }
            memcpy(data + 0x40000, data, 0x40000);
            memcpy(data + 0x80000, data, 0x40000);
            memcpy(data + 0xC0000, data, 0x40000);
        } else if (size == (long)FLASH_SIZE) {
            // 1 MB image: load directly
            if (fread(data, 1, FLASH_SIZE, f) != FLASH_SIZE)
                { fclose(f); return false; }
        } else {
            fclose(f);
            return false;  // unsupported size
        }
        fclose(f);
        return true;
    }

    // Load a 512-byte MCPX ROM dump.
    bool load_mcpx(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size != MCPX_ROM_SIZE) { fclose(f); return false; }
        if (fread(mcpx_rom, 1, MCPX_ROM_SIZE, f) != MCPX_ROM_SIZE)
            { fclose(f); return false; }
        fclose(f);
        mcpx_loaded = true;
        return true;
    }
};

static uint32_t flash_read(uint32_t pa, unsigned size, void* user) {
    auto* f = static_cast<FlashState*>(user);

    // MCPX hidden ROM: 0xFFFFFE00–0xFFFFFFFF (512 bytes at top of address space)
    if (f->mcpx_loaded && pa >= 0xFFFFFE00u) {
        uint32_t off = pa - 0xFFFFFE00u;
        if (off + size <= FlashState::MCPX_ROM_SIZE) {
            uint32_t val = 0;
            memcpy(&val, f->mcpx_rom + off, size);
            return val;
        }
    }

    // Both FLASH_BASE (0xF0000000) and BIOS_BASE (0xFF000000) map here.
    // Wrap into 1 MB range.
    uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
    uint32_t val = 0;
    memcpy(&val, f->data + off, size);
    return val;
}

static void flash_write(uint32_t, uint32_t, unsigned, void*) {}

} // namespace xbox
