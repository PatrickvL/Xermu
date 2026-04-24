#pragma once
// SMBus controller stub — EEPROM, SMC, video encoders.
//
// Flat register array indexed by (port & 0xF), matching the PGRAPH/APU
// pattern.  Named constants in the smbus:: namespace mirror Xbox SMBus
// I/O port register layout.
#include <cstdint>
#include <cstring>

namespace xbox {

// ======================== SMBus Register Offsets ===========================
// Port-relative offsets (port & 0xF).

namespace smbus {
static constexpr uint32_t STATUS  = 0x00;   // interrupt / completion status (W1C)
static constexpr uint32_t CONTROL = 0x02;   // transaction trigger
static constexpr uint32_t ADDRESS = 0x04;   // target device address + R/W bit
static constexpr uint32_t DATA    = 0x06;   // data byte
static constexpr uint32_t COMMAND = 0x08;   // SMBus command byte
} // namespace smbus

// =========================== SMBus State ==================================

struct SmbusState {
    static constexpr uint32_t REG_COUNT = 16;
    uint8_t regs[REG_COUNT] = {};
    uint8_t eeprom[256] = {};

    SmbusState() {
        init_eeprom();
    }

    void init_eeprom() {
        memset(eeprom, 0, sizeof(eeprom));
        eeprom[0x2C] = 0x01;
        const char* serial = "000000000000";
        memcpy(eeprom + 0x34, serial, 12);
        eeprom[0x40] = 0x00; eeprom[0x41] = 0x50; eeprom[0x42] = 0xF2;
        eeprom[0x43] = 0x00; eeprom[0x44] = 0x00; eeprom[0x45] = 0x01;
        eeprom[0x4C] = 0x00; eeprom[0x4D] = 0x01;
        eeprom[0x4E] = 0x80; eeprom[0x4F] = 0x00;
        eeprom[0xE8] = 0x01;
        eeprom[0xFC] = 0x01;
    }
};

// ========================== I/O Handlers ==================================

static uint32_t smbus_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    uint32_t r = port & 0xF;
    if (r < SmbusState::REG_COUNT) return s->regs[r];
    return 0;
}

static void smbus_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    uint32_t r = port & 0xF;
    if (r >= SmbusState::REG_COUNT) return;

    switch (r) {
    case smbus::STATUS:
        s->regs[r] &= ~(uint8_t)val;   // W1C
        return;
    case smbus::CONTROL: {
        s->regs[r] = (uint8_t)val;
        s->regs[smbus::STATUS] |= 0x10; // transaction complete
        uint8_t dev_addr = s->regs[smbus::ADDRESS] >> 1;
        bool is_read = (s->regs[smbus::ADDRESS] & 1) != 0;
        uint8_t cmd = s->regs[smbus::COMMAND];

        if (dev_addr == 0x54) {
            if (is_read && cmd < sizeof(s->eeprom))
                s->regs[smbus::DATA] = s->eeprom[cmd];
            else if (!is_read && cmd < sizeof(s->eeprom))
                s->eeprom[cmd] = s->regs[smbus::DATA];
        } else if (dev_addr == 0x10) {
            if (is_read) {
                switch (cmd) {
                case 0x01: s->regs[smbus::DATA] = 0xD0; break;
                case 0x03: s->regs[smbus::DATA] = 0x60; break;
                case 0x09: s->regs[smbus::DATA] = 25;   break;
                case 0x0A: s->regs[smbus::DATA] = 35;   break;
                case 0x0F: s->regs[smbus::DATA] = 0x05; break;
                case 0x11: s->regs[smbus::DATA] = 0x40; break;
                default:   s->regs[smbus::DATA] = 0;    break;
                }
            }
        } else if (dev_addr == 0x45) {
            if (is_read) s->regs[smbus::DATA] = 0;
        } else if (dev_addr == 0x6A) {
            if (is_read) s->regs[smbus::DATA] = 0;
        }
        return;
    }
    default:
        s->regs[r] = (uint8_t)val;
        return;
    }
}

} // namespace xbox
