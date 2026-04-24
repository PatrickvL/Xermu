#pragma once
// IDE (ATA) dual-channel controller — HDD + DVD stubs.
#include <cstdint>
#include <cstring>

namespace xbox {

struct IdeChannel {
    uint8_t  error      = 0x01;
    uint8_t  features   = 0;
    uint8_t  sect_count = 0x01;
    uint8_t  lba_low    = 0x01;
    uint8_t  lba_mid    = 0x00;
    uint8_t  lba_high   = 0x00;
    uint8_t  device     = 0x00;
    uint8_t  status     = 0x50;
    uint8_t  control    = 0x00;
    bool     present    = false;
    uint8_t  identify[512] = {};
};

struct IdeState {
    IdeChannel primary;
    IdeChannel secondary;

    IdeState() {
        primary.present   = true;
        primary.status    = 0x50;
        init_hdd_identify(primary);
        secondary.present = true;
        secondary.status  = 0x50;
        init_dvd_identify(secondary);
    }

    static void init_hdd_identify(IdeChannel& ch) {
        memset(ch.identify, 0, 512);
        auto* w = reinterpret_cast<uint16_t*>(ch.identify);
        w[0]  = 0x0040;
        w[1]  = 16383;
        w[3]  = 16;
        w[6]  = 63;
        set_ata_string(w + 27, "XBOX HDD", 40);
        set_ata_string(w + 10, "0123456789", 20);
        set_ata_string(w + 23, "1.00", 8);
        w[47] = 0x8010;
        w[49] = 0x0200;
        w[53] = 0x0006;
        w[60] = 0x5C10;
        w[61] = 0x0097;
        w[80] = 0x007E;
        w[83] = 0x4000;
        w[88] = 0x003F;
    }

    static void init_dvd_identify(IdeChannel& ch) {
        memset(ch.identify, 0, 512);
        auto* w = reinterpret_cast<uint16_t*>(ch.identify);
        w[0]  = 0x8580;
        set_ata_string(w + 27, "XBOX DVD", 40);
        set_ata_string(w + 10, "0000000001", 20);
        set_ata_string(w + 23, "1.00", 8);
        w[49] = 0x0200;
        w[80] = 0x007E;
    }

    static void set_ata_string(uint16_t* dst, const char* src, int byte_len) {
        char buf[64] = {};
        int slen = 0;
        while (src[slen] && slen < byte_len) { buf[slen] = src[slen]; slen++; }
        for (int i = slen; i < byte_len; i++) buf[i] = ' ';
        for (int i = 0; i < byte_len; i += 2)
            dst[i / 2] = (uint16_t)((uint8_t)buf[i] << 8 | (uint8_t)buf[i + 1]);
    }
};

static uint32_t ide_io_read(uint16_t port, unsigned size, void* user) {
    auto* ide = static_cast<IdeState*>(user);
    IdeChannel* ch;
    int reg;
    if (port >= 0x1F0 && port <= 0x1F7)      { ch = &ide->primary;   reg = port - 0x1F0; }
    else if (port == 0x3F6)                    { ch = &ide->primary;   reg = 8; }
    else if (port >= 0x170 && port <= 0x177)   { ch = &ide->secondary; reg = port - 0x170; }
    else if (port == 0x376)                    { ch = &ide->secondary; reg = 8; }
    else return 0xFF;

    if (!ch->present) return 0x00;
    switch (reg) {
    case 0: return 0;
    case 1: return ch->error;
    case 2: return ch->sect_count;
    case 3: return ch->lba_low;
    case 4: return ch->lba_mid;
    case 5: return ch->lba_high;
    case 6: return ch->device;
    case 7: return ch->status;
    case 8: return ch->status;
    default: return 0xFF;
    }
}

static void ide_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* ide = static_cast<IdeState*>(user);
    IdeChannel* ch;
    int reg;
    if (port >= 0x1F0 && port <= 0x1F7)      { ch = &ide->primary;   reg = port - 0x1F0; }
    else if (port == 0x3F6)                    { ch = &ide->primary;   reg = 8; }
    else if (port >= 0x170 && port <= 0x177)   { ch = &ide->secondary; reg = port - 0x170; }
    else if (port == 0x376)                    { ch = &ide->secondary; reg = 8; }
    else return;

    if (!ch->present) return;
    uint8_t v = (uint8_t)val;
    switch (reg) {
    case 0: break;
    case 1: ch->features = v; break;
    case 2: ch->sect_count = v; break;
    case 3: ch->lba_low = v; break;
    case 4: ch->lba_mid = v; break;
    case 5: ch->lba_high = v; break;
    case 6: ch->device = v; break;
    case 7:
        switch (v) {
        case 0xEC: ch->status = 0x58; ch->error = 0; break;
        case 0xA1: ch->status = 0x58; ch->error = 0; break;
        case 0xEF: ch->status = 0x50; ch->error = 0; break;
        case 0x91: ch->status = 0x50; ch->error = 0; break;
        case 0xE7: ch->status = 0x50; ch->error = 0; break;
        default:   ch->status = 0x51; ch->error = 0x04; break;
        }
        break;
    case 8:
        ch->control = v;
        if (v & 0x04) {
            ch->status = 0x50; ch->error = 0x01;
            ch->sect_count = 0x01; ch->lba_low = 0x01;
            ch->lba_mid = 0x00; ch->lba_high = 0x00;
            ch->device = 0x00;
        }
        break;
    }
}

} // namespace xbox
