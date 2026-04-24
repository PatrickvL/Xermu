#pragma once
// SMBus controller stub — EEPROM, SMC, video encoders.
#include <cstdint>
#include <cstring>

namespace xbox {

struct SmbusState {
    uint8_t status  = 0;
    uint8_t control = 0;
    uint8_t address = 0;
    uint8_t command = 0;
    uint8_t data    = 0;
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

static uint32_t smbus_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00: return s->status;
    case 0x02: return s->control;
    case 0x04: return s->address;
    case 0x06: return s->data;
    case 0x08: return s->command;
    default:   return 0;
    }
}

static void smbus_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00:
        s->status &= ~(uint8_t)val;
        return;
    case 0x02: {
        s->control = (uint8_t)val;
        s->status |= 0x10;
        uint8_t dev_addr = s->address >> 1;
        bool is_read = (s->address & 1) != 0;

        if (dev_addr == 0x54) {
            if (is_read && s->command < sizeof(s->eeprom))
                s->data = s->eeprom[s->command];
            else if (!is_read && s->command < sizeof(s->eeprom))
                s->eeprom[s->command] = s->data;
        } else if (dev_addr == 0x10) {
            if (is_read) {
                switch (s->command) {
                case 0x01: s->data = 0xD0; break;
                case 0x03: s->data = 0x60; break;
                case 0x09: s->data = 25;   break;
                case 0x0A: s->data = 35;   break;
                case 0x0F: s->data = 0x05; break;
                case 0x11: s->data = 0x40; break;
                default:   s->data = 0;    break;
                }
            }
        } else if (dev_addr == 0x45) {
            if (is_read) s->data = 0;
        } else if (dev_addr == 0x6A) {
            if (is_read) s->data = 0;
        }
        return;
    }
    case 0x04: s->address = (uint8_t)val; return;
    case 0x06: s->data = (uint8_t)val;    return;
    case 0x08: s->command = (uint8_t)val;  return;
    }
}

} // namespace xbox
