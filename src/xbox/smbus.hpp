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

// ======================== SMC Command Bytes ================================
// The SMC (PIC16LC microcontroller) at address 0x10 handles system management.

namespace smc {
static constexpr uint8_t SMC_VERSION      = 0x01;  // read: firmware version
static constexpr uint8_t TRAY_STATE       = 0x03;  // read: DVD tray state
static constexpr uint8_t AV_PACK          = 0x04;  // read: A/V pack type
static constexpr uint8_t FAN_MODE         = 0x05;  // write: fan control mode
static constexpr uint8_t FAN_SPEED        = 0x06;  // read/write: fan speed (0-50)
static constexpr uint8_t LED_OVERRIDE     = 0x07;  // write: LED state
static constexpr uint8_t LED_STATES       = 0x08;  // write: LED blink pattern
static constexpr uint8_t CPU_TEMP         = 0x09;  // read: CPU temperature (°C)
static constexpr uint8_t MB_TEMP          = 0x0A;  // read: motherboard temp (°C)
static constexpr uint8_t TRAY_EJECT       = 0x0C;  // write: eject/close tray
static constexpr uint8_t SCRATCH          = 0x0E;  // read/write: scratch register
static constexpr uint8_t CHALLENGE_0      = 0x1C;  // read: boot challenge byte 0
static constexpr uint8_t CHALLENGE_1      = 0x1D;  // read: boot challenge byte 1
static constexpr uint8_t CHALLENGE_2      = 0x1E;  // read: boot challenge byte 2
static constexpr uint8_t CHALLENGE_3      = 0x1F;  // read: boot challenge byte 3
static constexpr uint8_t RESET_ON_EJECT   = 0x19;  // write: reset-on-eject flag
static constexpr uint8_t INTERRUPT_REASON = 0x11;  // read: interrupt reason
static constexpr uint8_t POWER_OFF        = 0x02;  // write: power off / reset
static constexpr uint8_t AV_TYPE          = 0x0F;  // read: AV type indicator
} // namespace smc

// =========================== SMBus State ==================================

struct SmbusState {
    static constexpr uint32_t REG_COUNT = 16;
    uint8_t regs[REG_COUNT] = {};
    uint8_t eeprom[256] = {};

    // SMC state (PIC16LC at address 0x10)
    uint8_t smc_fan_mode    = 0;
    uint8_t smc_fan_speed   = 20;   // default ~40% speed
    uint8_t smc_led_mode    = 0;
    uint8_t smc_led_states  = 0x01; // default: green
    uint8_t smc_scratch     = 0;
    uint8_t smc_reset_eject = 0;

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
                case smc::SMC_VERSION:      s->regs[smbus::DATA] = 0xD0; break;  // v1.0 retail
                case smc::TRAY_STATE:       s->regs[smbus::DATA] = 0x60; break;  // closed, no media
                case smc::AV_PACK:          s->regs[smbus::DATA] = 0x07; break;  // HDTV pack
                case smc::FAN_SPEED:        s->regs[smbus::DATA] = s->smc_fan_speed; break;
                case smc::CPU_TEMP:         s->regs[smbus::DATA] = 40;   break;  // 40°C
                case smc::MB_TEMP:          s->regs[smbus::DATA] = 35;   break;  // 35°C
                case smc::SCRATCH:          s->regs[smbus::DATA] = s->smc_scratch; break;
                case smc::AV_TYPE:          s->regs[smbus::DATA] = 0x05; break;  // composite
                case smc::INTERRUPT_REASON: s->regs[smbus::DATA] = 0x00; break;  // no pending
                case smc::CHALLENGE_0:      s->regs[smbus::DATA] = 0x00; break;
                case smc::CHALLENGE_1:      s->regs[smbus::DATA] = 0x00; break;
                case smc::CHALLENGE_2:      s->regs[smbus::DATA] = 0x00; break;
                case smc::CHALLENGE_3:      s->regs[smbus::DATA] = 0x00; break;
                default:                    s->regs[smbus::DATA] = 0;    break;
                }
            } else {
                // SMC write commands
                switch (cmd) {
                case smc::FAN_MODE:       s->smc_fan_mode    = s->regs[smbus::DATA]; break;
                case smc::FAN_SPEED:      s->smc_fan_speed   = s->regs[smbus::DATA]; break;
                case smc::LED_OVERRIDE:   s->smc_led_mode    = s->regs[smbus::DATA]; break;
                case smc::LED_STATES:     s->smc_led_states  = s->regs[smbus::DATA]; break;
                case smc::SCRATCH:        s->smc_scratch     = s->regs[smbus::DATA]; break;
                case smc::RESET_ON_EJECT: s->smc_reset_eject = s->regs[smbus::DATA]; break;
                case smc::TRAY_EJECT:     break;  // silently accept
                case smc::POWER_OFF:      break;  // silently accept
                default: break;
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
