#pragma once
// ---------------------------------------------------------------------------
// ide.hpp — IDE (ATA) dual-channel controller — HDD + DVD with PIO data
// transfer.
//
// Flat task-file register array indexed by port offset (0-8), matching the
// PGRAPH/APU pattern.  Named constants in the ide_tf:: namespace.
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
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>

namespace xbox {

// ===================== ATA Task-File Register Offsets ======================
// Port-relative offsets (0-8).  Ports 1 and 7 are dual-function:
//   Port 1: read = ERROR, write = FEATURES (share same array slot).
//   Port 7: read = STATUS, write dispatches command (STATUS stored in array).

namespace ide_tf {
static constexpr uint32_t DATA       = 0;   // 16-bit PIO data port
static constexpr uint32_t ERR_FEAT   = 1;   // read: error; write: features
static constexpr uint32_t SECT_COUNT = 2;
static constexpr uint32_t LBA_LOW    = 3;
static constexpr uint32_t LBA_MID    = 4;
static constexpr uint32_t LBA_HIGH   = 5;
static constexpr uint32_t DEVICE     = 6;
static constexpr uint32_t STAT_CMD   = 7;   // read: status; write: command dispatch
static constexpr uint32_t ALT_CTL    = 8;   // alternate status / device control
} // namespace ide_tf

// ===================== Bus Master DMA Offsets ==============================
// PCI BAR4 offsets for primary (0x00-0x07) and secondary (0x08-0x0F) channels.
// Each channel has 3 regs at offsets +0, +2, +4 (command, status, PRDT addr).

namespace ide_bm {
static constexpr uint32_t CMD        = 0x00; // Bus Master command (bit 0 = start/stop, bit 3 = write)
static constexpr uint32_t STATUS     = 0x02; // Bus Master status (W1C bits 1,2; bit 0=active)
static constexpr uint32_t PRDT_ADDR  = 0x04; // Physical Region Descriptor Table base (32-bit)

// Command bits
static constexpr uint8_t  CMD_START  = 0x01; // start DMA transfer
static constexpr uint8_t  CMD_WRITE  = 0x08; // direction: 1=device→memory

// Status bits
static constexpr uint8_t  STAT_ACTIVE   = 0x01; // DMA transfer active
static constexpr uint8_t  STAT_ERROR    = 0x02; // DMA error (W1C)
static constexpr uint8_t  STAT_IRQ      = 0x04; // interrupt (W1C)
static constexpr uint8_t  STAT_DMA0_CAP = 0x20; // drive 0 DMA capable
static constexpr uint8_t  STAT_DMA1_CAP = 0x40; // drive 1 DMA capable
} // namespace ide_bm

// =========================== IDE Channel ===================================

struct IdeChannel {
    // Task-file register array (indexed by port offset 0-8).
    static constexpr uint32_t REG_COUNT = 9;
    uint8_t regs[REG_COUNT] = {
        0x00,   // DATA (unused as register; PIO handled separately)
        0x01,   // ERROR
        0x01,   // SECT_COUNT
        0x01,   // LBA_LOW
        0x00,   // LBA_MID
        0x00,   // LBA_HIGH
        0x00,   // DEVICE
        0x50,   // STATUS (DRDY | DSC)
        0x00,   // CONTROL
    };
    bool     present    = false;
    uint8_t  identify[512] = {};

    // PIO data transfer state
    uint8_t  data_buf[512] = {};  // current sector buffer (512 bytes)
    uint16_t data_pos      = 0;   // byte offset into data_buf
    uint16_t data_len      = 0;   // bytes remaining in current transfer
    uint8_t  sectors_left  = 0;   // sectors remaining for multi-sector commands

    // Bus Master DMA state
    uint8_t  bm_cmd     = 0;     // command register
    uint8_t  bm_status  = 0x60;  // status: drive 0+1 DMA capable by default
    uint32_t bm_prdt    = 0;     // PRDT base address

    // Backing disk image (raw sector image, no header).
    uint8_t* image_data = nullptr;
    uint64_t image_size = 0;

    static constexpr uint32_t SECTOR_SIZE = 512;

    uint32_t lba28() const {
        return (uint32_t)regs[ide_tf::LBA_LOW] |
               ((uint32_t)regs[ide_tf::LBA_MID]  << 8) |
               ((uint32_t)regs[ide_tf::LBA_HIGH] << 16) |
               ((uint32_t)(regs[ide_tf::DEVICE] & 0x0F) << 24);
    }

    void load_sector(uint32_t lba) {
        uint64_t off = (uint64_t)lba * SECTOR_SIZE;
        if (image_data && off + SECTOR_SIZE <= image_size)
            memcpy(data_buf, image_data + off, SECTOR_SIZE);
        else
            memset(data_buf, 0, SECTOR_SIZE);
        data_pos = 0;
        data_len = SECTOR_SIZE;
    }

    void store_sector(uint32_t lba) {
        uint64_t off = (uint64_t)lba * SECTOR_SIZE;
        if (image_data && off + SECTOR_SIZE <= image_size)
            memcpy(image_data + off, data_buf, SECTOR_SIZE);
    }

    void advance_sector() {
        uint32_t lba = lba28() + 1;
        regs[ide_tf::LBA_LOW]  = (uint8_t)(lba & 0xFF);
        regs[ide_tf::LBA_MID]  = (uint8_t)((lba >> 8) & 0xFF);
        regs[ide_tf::LBA_HIGH] = (uint8_t)((lba >> 16) & 0xFF);
        regs[ide_tf::DEVICE]   = (regs[ide_tf::DEVICE] & 0xF0) |
                                 (uint8_t)((lba >> 24) & 0x0F);
    }
};

// =========================== IDE State =====================================

struct IdeState {
    IdeChannel primary;
    IdeChannel secondary;

    IdeState() {
        primary.present   = true;
        init_hdd_identify(primary);
        secondary.present = true;
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

// ========================== I/O Handlers ===================================

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
            if (ch->sectors_left > 0) {
                ch->sectors_left--;
                if (ch->sectors_left > 0) {
                    ch->advance_sector();
                    ch->load_sector(ch->lba28());
                    ch->regs[ide_tf::STAT_CMD] = 0x58;
                } else {
                    ch->regs[ide_tf::STAT_CMD] = 0x50;
                }
            } else {
                ch->regs[ide_tf::STAT_CMD] = 0x50;
            }
        }
        return w;
    }
    case 7:
    case 8:
        return ch->regs[ide_tf::STAT_CMD];
    default:
        if (reg >= 1 && reg <= 6) return ch->regs[reg];
        return 0xFF;
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
            ch->store_sector(ch->lba28());
            if (ch->sectors_left > 0) {
                ch->sectors_left--;
                if (ch->sectors_left > 0) {
                    ch->advance_sector();
                    memset(ch->data_buf, 0, IdeChannel::SECTOR_SIZE);
                    ch->data_pos = 0;
                    ch->data_len = IdeChannel::SECTOR_SIZE;
                    ch->regs[ide_tf::STAT_CMD] = 0x58;
                } else {
                    ch->regs[ide_tf::STAT_CMD] = 0x50;
                }
            } else {
                ch->regs[ide_tf::STAT_CMD] = 0x50;
            }
        }
        break;
    }
    case 1: case 2: case 3: case 4: case 5: case 6:
        ch->regs[reg] = v;
        break;
    case 7:
        // Command dispatch — features value lives in regs[ERROR] slot.
        switch (v) {
        case 0xEC:  // IDENTIFY DEVICE
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->regs[ide_tf::STAT_CMD] = 0x58;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case 0xA1:  // IDENTIFY PACKET DEVICE
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->regs[ide_tf::STAT_CMD] = 0x58;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case 0x20: {  // READ SECTORS (PIO, LBA28)
            uint8_t count = ch->regs[ide_tf::SECT_COUNT];
            if (count == 0) count = 1;
            ch->sectors_left = count;
            ch->load_sector(ch->lba28());
            ch->sectors_left--;
            ch->regs[ide_tf::STAT_CMD] = 0x58;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        }
        case 0x30: {  // WRITE SECTORS (PIO, LBA28)
            uint8_t count = ch->regs[ide_tf::SECT_COUNT];
            if (count == 0) count = 1;
            ch->sectors_left = count;
            memset(ch->data_buf, 0, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->regs[ide_tf::STAT_CMD] = 0x58;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        }
        case 0xEF:  // SET FEATURES
            ch->regs[ide_tf::STAT_CMD] = 0x50;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case 0x91:  // INIT DEV PARAMS
            ch->regs[ide_tf::STAT_CMD] = 0x50;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case 0xE7:  // FLUSH CACHE
            ch->regs[ide_tf::STAT_CMD] = 0x50;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        default:
            ch->regs[ide_tf::STAT_CMD] = 0x51;
            ch->regs[ide_tf::ERR_FEAT] = 0x04;
            break;
        }
        break;
    case 8:
        ch->regs[ide_tf::ALT_CTL] = v;
        if (v & 0x04) {
            ch->regs[ide_tf::STAT_CMD]     = 0x50;
            ch->regs[ide_tf::ERR_FEAT]      = 0x01;
            ch->regs[ide_tf::SECT_COUNT] = 0x01;
            ch->regs[ide_tf::LBA_LOW]    = 0x01;
            ch->regs[ide_tf::LBA_MID]    = 0x00;
            ch->regs[ide_tf::LBA_HIGH]   = 0x00;
            ch->regs[ide_tf::DEVICE]     = 0x00;
        }
        break;
    }
}

// ========================= Bus Master DMA I/O ==============================
// BAR4 base is typically 0xFF60 on Xbox.
// Primary channel: offsets 0x00-0x07, Secondary: 0x08-0x0F.

static uint32_t ide_bm_read(uint16_t port, unsigned size, void* user) {
    auto* ide = static_cast<IdeState*>(user);
    uint32_t off = port & 0x0F;
    IdeChannel* ch = (off < 8) ? &ide->primary : &ide->secondary;
    uint32_t reg = off & 0x07;

    switch (reg) {
    case ide_bm::CMD:    return ch->bm_cmd;
    case ide_bm::STATUS: return ch->bm_status;
    case ide_bm::PRDT_ADDR:
        if (size >= 4) return ch->bm_prdt;
        return (uint8_t)(ch->bm_prdt >> ((off & 3) * 8));
    default: return 0;
    }
}

static void ide_bm_write(uint16_t port, uint32_t val, unsigned size, void* user) {
    auto* ide = static_cast<IdeState*>(user);
    uint32_t off = port & 0x0F;
    IdeChannel* ch = (off < 8) ? &ide->primary : &ide->secondary;
    uint32_t reg = off & 0x07;

    switch (reg) {
    case ide_bm::CMD:
        ch->bm_cmd = (uint8_t)(val & 0x09);  // only bits 0,3 writable
        if (!(val & ide_bm::CMD_START))
            ch->bm_status &= ~ide_bm::STAT_ACTIVE;  // stop clears active
        break;
    case ide_bm::STATUS: {
        // Bits 1,2 are W1C; bits 5,6 are writable (DMA capable); bit 0 read-only.
        uint8_t w1c = (uint8_t)(val & (ide_bm::STAT_ERROR | ide_bm::STAT_IRQ));
        ch->bm_status &= ~w1c;
        // Bits 5,6 writable
        ch->bm_status = (ch->bm_status & ~0x60) | (uint8_t)(val & 0x60);
        break;
    }
    case ide_bm::PRDT_ADDR:
        if (size >= 4) {
            ch->bm_prdt = val & 0xFFFFFFFC;  // must be dword-aligned
        }
        break;
    }
}

} // namespace xbox
