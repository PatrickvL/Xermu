#pragma once
// IDE (ATA) dual-channel controller — HDD + DVD with PIO data transfer.
//
// Supports:
//   - IDENTIFY DEVICE (0xEC) / IDENTIFY PACKET DEVICE (0xA1)
//   - READ SECTORS (0x20) — LBA28 PIO read from backing image
//   - WRITE SECTORS (0x30) — LBA28 PIO write to backing image
//   - SET FEATURES (0xEF), INIT DEV PARAMS (0x91), FLUSH CACHE (0xE7)
//   - Software reset via control register
//   - Data port (0x1F0 / 0x170) 16-bit PIO transfer
//
// Disk backing: set IdeChannel::image_data / image_size to a raw sector
// image (512 bytes per sector).  No image = zero-fill on read, discard on write.
#include <cstdint>
#include <cstring>

namespace xbox {

struct IdeChannel {
    // Task-file registers
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

    // PIO data transfer state
    uint8_t  data_buf[512] = {};  // current sector buffer (512 bytes)
    uint16_t data_pos      = 0;   // byte offset into data_buf
    uint16_t data_len      = 0;   // bytes remaining in current transfer
    uint8_t  sectors_left  = 0;   // sectors remaining for multi-sector commands

    // Backing disk image (raw sector image, no header).
    // Caller owns the memory; nullptr = no backing (zero-fill reads, discard writes).
    uint8_t* image_data = nullptr;
    uint64_t image_size = 0;       // bytes

    static constexpr uint32_t SECTOR_SIZE = 512;

    uint32_t lba28() const {
        return (uint32_t)lba_low |
               ((uint32_t)lba_mid  << 8) |
               ((uint32_t)lba_high << 16) |
               ((uint32_t)(device & 0x0F) << 24);
    }

    // Load the next sector from the image into data_buf.
    void load_sector(uint32_t lba) {
        uint64_t off = (uint64_t)lba * SECTOR_SIZE;
        if (image_data && off + SECTOR_SIZE <= image_size)
            memcpy(data_buf, image_data + off, SECTOR_SIZE);
        else
            memset(data_buf, 0, SECTOR_SIZE);
        data_pos = 0;
        data_len = SECTOR_SIZE;
    }

    // Store the current data_buf to the image.
    void store_sector(uint32_t lba) {
        uint64_t off = (uint64_t)lba * SECTOR_SIZE;
        if (image_data && off + SECTOR_SIZE <= image_size)
            memcpy(image_data + off, data_buf, SECTOR_SIZE);
    }

    // Advance to next sector in a multi-sector transfer (READ/WRITE SECTORS).
    void advance_sector() {
        uint32_t lba = lba28();
        lba++;
        lba_low  = (uint8_t)(lba & 0xFF);
        lba_mid  = (uint8_t)((lba >> 8) & 0xFF);
        lba_high = (uint8_t)((lba >> 16) & 0xFF);
        device   = (device & 0xF0) | (uint8_t)((lba >> 24) & 0x0F);
    }
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
    case 0: {
        // Data port — 16-bit PIO read from data_buf.
        if (ch->data_len == 0) return 0;
        uint16_t w = 0;
        if (ch->data_pos + 1 < IdeChannel::SECTOR_SIZE)
            memcpy(&w, ch->data_buf + ch->data_pos, 2);
        ch->data_pos += 2;
        ch->data_len -= 2;
        if (ch->data_len == 0) {
            // Sector transfer complete.
            if (ch->sectors_left > 0) {
                ch->sectors_left--;
                if (ch->sectors_left > 0) {
                    // More sectors: advance LBA and load next.
                    ch->advance_sector();
                    ch->load_sector(ch->lba28());
                    ch->status = 0x58;  // DRDY | DSC | DRQ
                } else {
                    ch->status = 0x50;  // DRDY | DSC (transfer done)
                }
            } else {
                ch->status = 0x50;
            }
        }
        return w;
    }
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
    case 0: {
        // Data port — 16-bit PIO write to data_buf.
        if (ch->data_len == 0) break;
        uint16_t w = (uint16_t)(val & 0xFFFF);
        if (ch->data_pos + 1 < IdeChannel::SECTOR_SIZE)
            memcpy(ch->data_buf + ch->data_pos, &w, 2);
        ch->data_pos += 2;
        ch->data_len -= 2;
        if (ch->data_len == 0) {
            // Sector buffer full — write it out.
            ch->store_sector(ch->lba28());
            if (ch->sectors_left > 0) {
                ch->sectors_left--;
                if (ch->sectors_left > 0) {
                    ch->advance_sector();
                    memset(ch->data_buf, 0, IdeChannel::SECTOR_SIZE);
                    ch->data_pos = 0;
                    ch->data_len = IdeChannel::SECTOR_SIZE;
                    ch->status = 0x58;  // DRDY | DSC | DRQ
                } else {
                    ch->status = 0x50;  // DRDY | DSC (transfer done)
                }
            } else {
                ch->status = 0x50;
            }
        }
        break;
    }
    case 1: ch->features = v; break;
    case 2: ch->sect_count = v; break;
    case 3: ch->lba_low = v; break;
    case 4: ch->lba_mid = v; break;
    case 5: ch->lba_high = v; break;
    case 6: ch->device = v; break;
    case 7:
        switch (v) {
        case 0xEC:  // IDENTIFY DEVICE
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->status = 0x58; ch->error = 0;
            break;
        case 0xA1:  // IDENTIFY PACKET DEVICE
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->status = 0x58; ch->error = 0;
            break;
        case 0x20: {  // READ SECTORS (PIO, LBA28)
            uint8_t count = ch->sect_count;
            if (count == 0) count = 1;  // 0 means 256, but clamp to 1 for safety
            ch->sectors_left = count;
            ch->load_sector(ch->lba28());
            ch->sectors_left--;
            ch->status = 0x58; ch->error = 0;
            break;
        }
        case 0x30: {  // WRITE SECTORS (PIO, LBA28)
            uint8_t count = ch->sect_count;
            if (count == 0) count = 1;
            ch->sectors_left = count;
            memset(ch->data_buf, 0, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->status = 0x58; ch->error = 0;
            break;
        }
        case 0xEF: ch->status = 0x50; ch->error = 0; break;  // SET FEATURES
        case 0x91: ch->status = 0x50; ch->error = 0; break;  // INIT DEV PARAMS
        case 0xE7: ch->status = 0x50; ch->error = 0; break;  // FLUSH CACHE
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
