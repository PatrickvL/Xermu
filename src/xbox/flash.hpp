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

// ====================== Flash Command Interface ============================
// SST49LF040-compatible CFI (Common Flash Interface) command sequences.
// The Xbox BIOS flash uses command writes to special addresses for
// chip identification, sector erase, and byte program operations.

namespace flash_cmd {
static constexpr uint8_t READ_ARRAY  = 0xFF; // return to normal read mode
static constexpr uint8_t READ_ID     = 0x90; // enter ID read mode
static constexpr uint8_t READ_STATUS = 0x70; // read status register
static constexpr uint8_t CLEAR_STATUS= 0x50; // clear status register
static constexpr uint8_t BYTE_PROGRAM= 0x40; // byte program
static constexpr uint8_t ERASE_SETUP = 0x20; // block erase setup
static constexpr uint8_t ERASE_CONFIRM=0xD0; // block erase confirm

// Status register bits
static constexpr uint8_t SR_READY    = 0x80; // device ready (1=ready)
static constexpr uint8_t SR_ERASE_ERR= 0x20; // erase error
static constexpr uint8_t SR_PROG_ERR = 0x10; // program error
} // namespace flash_cmd

struct FlashState {
    uint8_t data[FLASH_SIZE];

    // MCPX hidden boot ROM (512 bytes, mapped at PA 0xFFFFFE00).
    static constexpr uint32_t MCPX_ROM_SIZE = 512;
    uint8_t  mcpx_rom[MCPX_ROM_SIZE];
    bool     mcpx_loaded = false;

    // Flash command state machine
    uint8_t  mode       = 0;    // 0=read array, 1=ID, 2=status, 3=program, 4=erase
    uint8_t  status_reg = flash_cmd::SR_READY;  // power-on: ready

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

    // Flash command interface modes
    if (f->mode == 2) {
        // Status register read
        return f->status_reg;
    }
    if (f->mode == 1) {
        // ID mode: manufacturer/device at offsets 0,1
        uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
        if ((off & 0xFFFF) == 0) return 0xBF;  // SST manufacturer ID
        if ((off & 0xFFFF) == 1) return 0x52;  // SST49LF040 device ID
        return 0;
    }

    // Normal read: both FLASH_BASE (0xF0000000) and BIOS_BASE (0xFF000000) map here.
    uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
    uint32_t val = 0;
    memcpy(&val, f->data + off, size);
    return val;
}

static void flash_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* f = static_cast<FlashState*>(user);
    uint8_t cmd = (uint8_t)val;

    // If in program mode, the next write is the data byte
    if (f->mode == 3) {
        uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
        if (off < FLASH_SIZE)
            f->data[off] &= cmd;  // flash can only clear bits
        f->status_reg = flash_cmd::SR_READY;
        f->mode = 0;
        return;
    }

    // If in erase-setup mode, expect confirm
    if (f->mode == 4) {
        if (cmd == flash_cmd::ERASE_CONFIRM) {
            uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
            uint32_t sector_base = off & ~0xFFFu;
            uint32_t sector_end = sector_base + 0x1000;
            if (sector_end <= FLASH_SIZE)
                memset(f->data + sector_base, 0xFF, 0x1000);
            f->status_reg = flash_cmd::SR_READY;
        }
        f->mode = 0;
        return;
    }

    // Command dispatch (mode 0, 1, or 2)
    switch (cmd) {
    case flash_cmd::READ_ARRAY:   f->mode = 0; return;
    case flash_cmd::READ_ID:      f->mode = 1; return;
    case flash_cmd::READ_STATUS:  f->mode = 2; return;
    case flash_cmd::CLEAR_STATUS:
        f->status_reg = flash_cmd::SR_READY;
        f->mode = 0;
        return;
    case flash_cmd::BYTE_PROGRAM: f->mode = 3; return;
    case flash_cmd::ERASE_SETUP:  f->mode = 4; return;
    default: return;
    }
}

} // namespace xbox
