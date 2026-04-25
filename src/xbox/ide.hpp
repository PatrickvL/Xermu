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

// ===================== ATA Status Register Bits ============================
namespace ata_status {
static constexpr uint8_t ERR   = 0x01;  // error occurred
static constexpr uint8_t DRQ   = 0x08;  // data request
static constexpr uint8_t DSC   = 0x10;  // device seek complete
static constexpr uint8_t DRDY  = 0x40;  // device ready
static constexpr uint8_t BSY   = 0x80;  // busy
// Common composites
static constexpr uint8_t DRDY_DSC      = DRDY | DSC;         // 0x50
static constexpr uint8_t DRDY_DSC_DRQ  = DRDY | DSC | DRQ;   // 0x58
static constexpr uint8_t DRDY_DSC_ERR  = DRDY | DSC | ERR;   // 0x51
} // namespace ata_status

// ===================== ATA Error Register Bits =============================
namespace ata_err {
static constexpr uint8_t AMNF  = 0x01;  // address mark not found
static constexpr uint8_t ABRT  = 0x04;  // aborted command
static constexpr uint8_t IDNF  = 0x10;  // ID not found
static constexpr uint8_t UNC   = 0x40;  // uncorrectable data error
} // namespace ata_err

// ===================== ATA Command Codes ===================================
namespace ata_cmd {
static constexpr uint8_t READ_SECTORS      = 0x20;
static constexpr uint8_t WRITE_SECTORS     = 0x30;
static constexpr uint8_t INIT_DEV_PARAMS   = 0x91;
static constexpr uint8_t PACKET            = 0xA0;  // ATAPI packet command
static constexpr uint8_t IDENTIFY_PACKET   = 0xA1;  // IDENTIFY PACKET DEVICE
static constexpr uint8_t SET_FEATURES      = 0xEF;
static constexpr uint8_t FLUSH_CACHE       = 0xE7;
static constexpr uint8_t IDENTIFY_DEVICE   = 0xEC;
} // namespace ata_cmd

// ===================== ATA Device Control Bits =============================
namespace ata_ctl {
static constexpr uint8_t NIEN = 0x02;  // disable interrupts
static constexpr uint8_t SRST = 0x04;  // software reset
} // namespace ata_ctl

// ===================== ATAPI Signature =====================================
namespace atapi {
static constexpr uint8_t SIGNATURE_MID  = 0x14;  // LBA_MID after reset
static constexpr uint8_t SIGNATURE_HIGH = 0xEB;  // LBA_HIGH after reset
} // namespace atapi

// ===================== SCSI Command Codes ==================================
namespace scsi {
static constexpr uint8_t TEST_UNIT_READY = 0x00;
static constexpr uint8_t INQUIRY         = 0x12;
static constexpr uint8_t MODE_SENSE_10   = 0x5A;
// INQUIRY response device types
static constexpr uint8_t INQ_TYPE_CDROM  = 0x05;
static constexpr uint8_t INQ_REMOVABLE   = 0x80;
} // namespace scsi

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
        ata_status::DRDY_DSC,  // STATUS
        0x00,   // CONTROL
    };
    bool     present    = false;
    bool     is_atapi   = false;
    uint8_t  identify[512] = {};

    // PIO data transfer state
    uint8_t  data_buf[512] = {};  // current sector buffer (512 bytes)
    uint16_t data_pos      = 0;   // byte offset into data_buf
    uint16_t data_len      = 0;   // bytes remaining in current transfer
    uint8_t  sectors_left  = 0;   // sectors remaining for multi-sector commands

    // ATAPI packet command state
    uint8_t  packet_buf[12] = {}; // 12-byte command packet
    uint8_t  packet_pos     = 0;  // bytes received so far
    bool     awaiting_packet = false;

    // Bus Master DMA state
    uint8_t  bm_cmd     = 0;     // command register
    uint8_t  bm_status  = ide_bm::STAT_DMA0_CAP | ide_bm::STAT_DMA1_CAP;
    uint32_t bm_prdt    = 0;     // PRDT base address

    // Backing disk image (raw sector image, no header).
    uint8_t* image_data = nullptr;
    uint64_t image_size = 0;

    static constexpr uint32_t SECTOR_SIZE = 512;

    uint32_t lba28() const {
        return (uint32_t)regs[ide_tf::LBA_LOW] |
               ((uint32_t)regs[ide_tf::LBA_MID]  << 8) |
               ((uint32_t)regs[ide_tf::LBA_HIGH] << 16) |
               ((uint32_t)(regs[ide_tf::DEVICE] & 0x0Fu) << 24);
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
        secondary.present  = true;
        secondary.is_atapi = true;
        init_dvd_identify(secondary);
        // Set ATAPI signature on secondary channel
        secondary.regs[ide_tf::LBA_MID]    = atapi::SIGNATURE_MID;
        secondary.regs[ide_tf::LBA_HIGH]   = atapi::SIGNATURE_HIGH;
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
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
                } else {
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
                }
            } else {
                ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
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
        // Data port — 16-bit PIO write to data_buf or ATAPI packet.
        if (ch->awaiting_packet) {
            uint16_t w = (uint16_t)(val & 0xFFFF);
            if (ch->packet_pos + 1 < 12) {
                ch->packet_buf[ch->packet_pos]     = (uint8_t)(w & 0xFF);
                ch->packet_buf[ch->packet_pos + 1] = (uint8_t)(w >> 8);
            }
            ch->packet_pos += 2;
            if (ch->packet_pos >= 12) {
                ch->awaiting_packet = false;
                // Execute ATAPI command
                uint8_t scsi_op = ch->packet_buf[0];
                switch (scsi_op) {
                case scsi::TEST_UNIT_READY:
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
                    ch->regs[ide_tf::ERR_FEAT] = 0;
                    break;
                case scsi::INQUIRY: {
                    memset(ch->data_buf, 0, 36);
                    ch->data_buf[0] = scsi::INQ_TYPE_CDROM;
                    ch->data_buf[1] = scsi::INQ_REMOVABLE;
                    ch->data_buf[2] = 0x00;  // version
                    ch->data_buf[3] = 0x21;  // response format
                    ch->data_buf[4] = 31;    // additional length
                    memcpy(ch->data_buf + 8,  "XBOX    ", 8);  // vendor
                    memcpy(ch->data_buf + 16, "DVD             ", 16); // product
                    memcpy(ch->data_buf + 32, "1.00", 4);     // revision
                    ch->data_pos = 0;
                    ch->data_len = 36;
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
                    ch->regs[ide_tf::ERR_FEAT] = 0;
                    break;
                }
                case scsi::MODE_SENSE_10: {
                    // Return minimal header: 8 bytes
                    memset(ch->data_buf, 0, 8);
                    ch->data_buf[1] = 6;  // mode data length - 1
                    ch->data_pos = 0;
                    ch->data_len = 8;
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
                    ch->regs[ide_tf::ERR_FEAT] = 0;
                    break;
                }
                default:
                    // Unknown SCSI op — set error (ABRT)
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_ERR;
                    ch->regs[ide_tf::ERR_FEAT] = ata_err::ABRT;
                    break;
                }
            }
            break;
        }
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
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
                } else {
                    ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
                }
            } else {
                ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
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
        case ata_cmd::IDENTIFY_DEVICE:
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case ata_cmd::IDENTIFY_PACKET:
            memcpy(ch->data_buf, ch->identify, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->sectors_left = 0;
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case ata_cmd::PACKET:
            if (!ch->is_atapi) {
                ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_ERR;
                ch->regs[ide_tf::ERR_FEAT] = ata_err::ABRT;
                break;
            }
            ch->awaiting_packet = true;
            ch->packet_pos = 0;
            memset(ch->packet_buf, 0, 12);
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            // Set byte count (LBA_MID/HIGH) to indicate packet size
            ch->regs[ide_tf::SECT_COUNT] = 0x01; // C/D=1, I/O=0 = command
            break;
        case ata_cmd::READ_SECTORS: {
            uint8_t count = ch->regs[ide_tf::SECT_COUNT];
            if (count == 0) count = 1;
            ch->sectors_left = count;
            ch->load_sector(ch->lba28());
            ch->sectors_left--;
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        }
        case ata_cmd::WRITE_SECTORS: {
            uint8_t count = ch->regs[ide_tf::SECT_COUNT];
            if (count == 0) count = 1;
            ch->sectors_left = count;
            memset(ch->data_buf, 0, IdeChannel::SECTOR_SIZE);
            ch->data_pos = 0;
            ch->data_len = IdeChannel::SECTOR_SIZE;
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_DRQ;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        }
        case ata_cmd::SET_FEATURES:
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case ata_cmd::INIT_DEV_PARAMS:
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        case ata_cmd::FLUSH_CACHE:
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC;
            ch->regs[ide_tf::ERR_FEAT] = 0;
            break;
        default:
            ch->regs[ide_tf::STAT_CMD] = ata_status::DRDY_DSC_ERR;
            ch->regs[ide_tf::ERR_FEAT] = ata_err::ABRT;
            break;
        }
        break;
    case 8:
        ch->regs[ide_tf::ALT_CTL] = v;
        if (v & ata_ctl::SRST) {
            ch->regs[ide_tf::STAT_CMD]  = ata_status::DRDY_DSC;
            ch->regs[ide_tf::ERR_FEAT]  = 0x01;
            ch->regs[ide_tf::SECT_COUNT] = 0x01;
            ch->regs[ide_tf::LBA_LOW]   = 0x01;
            if (ch->is_atapi) {
                ch->regs[ide_tf::LBA_MID]  = atapi::SIGNATURE_MID;
                ch->regs[ide_tf::LBA_HIGH] = atapi::SIGNATURE_HIGH;
            } else {
                ch->regs[ide_tf::LBA_MID]  = 0x00;
                ch->regs[ide_tf::LBA_HIGH] = 0x00;
            }
            ch->regs[ide_tf::DEVICE]    = 0x00;
            ch->awaiting_packet = false;
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
        constexpr uint8_t dma_cap = ide_bm::STAT_DMA0_CAP | ide_bm::STAT_DMA1_CAP;
        ch->bm_status = (ch->bm_status & ~dma_cap) | (uint8_t)(val & dma_cap);
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
