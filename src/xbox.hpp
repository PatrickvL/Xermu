#pragma once
// ---------------------------------------------------------------------------
// xbox.hpp — Xbox physical address map + device stubs.
//
// Sets up the MmioMap and I/O port handlers for the original Xbox hardware:
//
//   Guest PA          Size       Device
//   ─────────────────────────────────────────────────
//   0x0000_0000       64 MB      Main RAM
//   0x0C00_0000      128 MB      RAM mirror (NV2A tiling alias)
//   0xF000_0000        1 MB      Flash ROM (MCPX + BIOS)
//   0xFD00_0000       16 MB      NV2A GPU registers (MMIO)
//   0xFE80_0000        4 MB      APU / AC97 (MMIO)
//   0xFEC0_0000        4 KB      I/O APIC (MMIO)
//   0xFF00_0000        1 MB      BIOS shadow
//
//   I/O ports:
//   0x0020–0x0021     PIC (master 8259A)
//   0x00A0–0x00A1     PIC (slave 8259A)
//   0x0040–0x0043     PIT (8254 timer)
//   0x0061            System control port B
//   0x0CF8            PCI config address
//   0x0CFC–0x0CFF     PCI config data
//   0xC000–0xC00F     SMBus (MCPX)
//   0xE9              Debug console (bochs-style)
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include <cstdio>
#include <cstring>

// ============================= Address Map =================================

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

static constexpr uint32_t BIOS_BASE         = 0xFF000000u;
static constexpr uint32_t BIOS_SIZE         = 0x00100000u;  // 1 MB

// ============================= NV2A GPU ====================================

struct Nv2aState {
    uint32_t pmc_boot_0 = 0x02A000A1;  // NV2A chip ID (NV20 derivative)
    uint32_t pmc_enable = 0;
    uint32_t pfb_cfg0   = 0x03070103;  // 64 MB RAM
    uint32_t pbus_0     = 0;
    uint32_t ptimer_num = 1;
    uint32_t ptimer_den = 1;
    uint32_t pcrtc_start = 0;
    uint32_t pvideo_intr = 0;
};

static uint32_t nv2a_read(uint32_t pa, unsigned size, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    // PMC (0x000xxx) — master control
    if (off == 0x000000) return nv->pmc_boot_0;    // PMC_BOOT_0
    if (off == 0x000200) return nv->pmc_enable;     // PMC_ENABLE

    // PBUS (0x001xxx) — bus control
    if (off == 0x001200) return nv->pbus_0;         // PBUS_PCI_NV_0
    if (off == 0x001214) return 0;                  // PBUS interrupt status

    // PFIFO (0x002xxx) — FIFO engine
    if (off == 0x002100) return 0;                  // PFIFO_INTR_0
    if (off == 0x002140) return 0;                  // PFIFO_INTR_EN_0
    if (off == 0x002500) return 0;                  // PFIFO_CACHES

    // PTIMER (0x009xxx) — timer
    if (off == 0x009200) return nv->ptimer_num;
    if (off == 0x009210) return nv->ptimer_den;
    if (off == 0x009400) return 0;                  // PTIMER_TIME_0 (low)
    if (off == 0x009410) return 0;                  // PTIMER_TIME_1 (high)
    if (off == 0x009100) return 0;                  // PTIMER_INTR_0

    // PFB (0x100xxx) — framebuffer control
    if (off == 0x100200) return nv->pfb_cfg0;       // PFB_CFG0
    if (off == 0x100204) return 0;                  // PFB_CFG1

    // PCRTC (0x600xxx) — CRTC
    if (off == 0x600100) return 0;                  // PCRTC_INTR_0
    if (off == 0x600800) return nv->pcrtc_start;    // PCRTC_START

    // PVIDEO (0x008xxx) — video overlay
    if (off == 0x008100) return nv->pvideo_intr;

    // Default: bus float
    return 0;
}

static void nv2a_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    if (off == 0x000200) { nv->pmc_enable = val; return; }
    if (off == 0x001200) { nv->pbus_0 = val;     return; }
    if (off == 0x009200) { nv->ptimer_num = val;  return; }
    if (off == 0x009210) { nv->ptimer_den = val;  return; }
    if (off == 0x600800) { nv->pcrtc_start = val; return; }
    if (off == 0x600100) { nv->pvideo_intr &= ~val; return; } // W1C
    if (off == 0x008100) { nv->pvideo_intr &= ~val; return; } // W1C

    // Silently drop unhandled writes.
}

// ============================= APU =========================================

static uint32_t apu_read(uint32_t /*pa*/, unsigned /*size*/, void* /*user*/) {
    return 0;  // stub: all zeros
}
static void apu_write(uint32_t, uint32_t, unsigned, void*) {}

// ============================= I/O APIC ====================================

struct IoApicState {
    uint32_t index = 0;
    uint32_t regs[24 * 2 + 2] = {}; // ID, VER, ARB, + 24 redirection entries × 2

    IoApicState() {
        regs[0] = 0;           // IOAPIC_ID
        regs[1] = 0x00170011;  // IOAPIC_VER: 17h max redir, version 11h
    }
};

static uint32_t ioapic_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) return s->index;           // IOREGSEL
    if (off == 0x10 && s->index < 64) return s->regs[s->index]; // IOWIN
    return 0;
}

static void ioapic_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) { s->index = val & 0xFF; return; }
    if (off == 0x10 && s->index < 64) { s->regs[s->index] = val; return; }
}

// ============================= RAM Mirror ==================================
// NV2A tiling alias: reads/writes at [0x0C000000..0x13FFFFFF] map to main RAM
// with PA = (access_pa - 0x0C000000) % actual_ram_size.

static uint32_t ram_mirror_read(uint32_t pa, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    uint32_t val = 0;
    memcpy(&val, ram + off, size);
    return val;
}

static void ram_mirror_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    memcpy(ram + off, &val, size);
}

// ============================= Flash ROM ===================================
// 1 MB flash ROM. Reads return stored bytes or 0xFF (empty). Writes ignored.

struct FlashState {
    uint8_t data[FLASH_SIZE];
    FlashState() { memset(data, 0xFF, FLASH_SIZE); }
};

static uint32_t flash_read(uint32_t pa, unsigned size, void* user) {
    auto* f = static_cast<FlashState*>(user);
    // Both FLASH_BASE (0xF0000000) and BIOS_BASE (0xFF000000) map here.
    // Wrap into 1 MB range.
    uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
    uint32_t val = 0;
    memcpy(&val, f->data + off, size);
    return val;
}

static void flash_write(uint32_t, uint32_t, unsigned, void*) {}

// ============================= PCI Config ==================================
// PCI configuration space via I/O ports 0xCF8 (address) / 0xCFC (data).
// The Xbox MCPX provides buses 0-1 with a handful of devices.

struct PciState {
    uint32_t config_address = 0; // written to port 0xCF8
    // Simple flat config space: [bus][dev][fn][256 bytes]
    // Only a few devices are populated.
    static constexpr int MAX_DEVS = 32;
    uint8_t config[2][MAX_DEVS][8][256] = {}; // [bus][dev][fn][offset]

    void set_id(int bus, int dev, int fn, uint16_t vendor, uint16_t device,
                uint8_t cls, uint8_t subcls, uint8_t progif, uint8_t rev) {
        uint8_t* c = config[bus][dev][fn];
        memcpy(c + 0x00, &vendor, 2);
        memcpy(c + 0x02, &device, 2);
        c[0x08] = rev;
        c[0x09] = progif;
        c[0x0A] = subcls;
        c[0x0B] = cls;
    }

    void init_xbox_devices() {
        // Bus 0, Dev 0: MCPX Host Bridge
        set_id(0, 0, 0, 0x10DE, 0x02A5, 0x06, 0x00, 0x00, 0xB1);
        // Bus 0, Dev 1, Fn 0: MCPX LPC (ISA Bridge)
        set_id(0, 1, 0, 0x10DE, 0x01B2, 0x06, 0x01, 0x00, 0xB1);
        // Bus 0, Dev 1, Fn 1: MCPX SMBus controller
        set_id(0, 1, 1, 0x10DE, 0x01B4, 0x0C, 0x05, 0x00, 0xB1);
        // Set SMBus I/O base (BAR) at offset 0x14 = 0xC000
        {
            uint16_t bar = 0xC001; // I/O space bit set
            memcpy(config[0][1][1] + 0x14, &bar, 2);
        }
        // Bus 0, Dev 2: NV2A (GPU)
        set_id(0, 2, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
        // Bus 0, Dev 3: MCPX Audio (APU)
        set_id(0, 3, 0, 0x10DE, 0x01B0, 0x04, 0x01, 0x00, 0xB1);
        // Bus 0, Dev 4: OHCI USB #0
        set_id(0, 4, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        // Bus 0, Dev 5: OHCI USB #1
        set_id(0, 5, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        // Bus 0, Dev 9: IDE controller
        set_id(0, 9, 0, 0x10DE, 0x01BC, 0x01, 0x01, 0x8A, 0xB1);
        // Bus 0, Dev 30: AGP → PCI bridge
        set_id(0, 30, 0, 0x10DE, 0x01B7, 0x06, 0x04, 0x00, 0xB1);
        // Bus 1, Dev 0: NV2A GPU (on AGP bus)
        set_id(1, 0, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
    }
};

static void pci_io_write_cf8(uint16_t /*port*/, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PciState*>(user)->config_address = val;
}
static uint32_t pci_io_read_cf8(uint16_t /*port*/, unsigned /*size*/, void* user) {
    return static_cast<PciState*>(user)->config_address;
}

static uint32_t pci_io_read_cfc(uint16_t port, unsigned size, void* user) {
    auto* pci = static_cast<PciState*>(user);
    uint32_t addr = pci->config_address;
    if (!(addr & 0x80000000u)) return 0xFFFFFFFFu; // enable bit not set
    int bus = (addr >> 16) & 0xFF;
    int dev = (addr >> 11) & 0x1F;
    int fn  = (addr >>  8) & 0x07;
    int off = (addr & 0xFC) + (port - 0xCFC);
    if (bus > 1 || dev >= PciState::MAX_DEVS || off + (int)size > 256)
        return 0xFFFFFFFFu;
    uint32_t val = 0;
    memcpy(&val, pci->config[bus][dev][fn] + off, size);
    return val;
}

static void pci_io_write_cfc(uint16_t port, uint32_t val, unsigned size, void* user) {
    auto* pci = static_cast<PciState*>(user);
    uint32_t addr = pci->config_address;
    if (!(addr & 0x80000000u)) return;
    int bus = (addr >> 16) & 0xFF;
    int dev = (addr >> 11) & 0x1F;
    int fn  = (addr >>  8) & 0x07;
    int off = (addr & 0xFC) + (port - 0xCFC);
    if (bus > 1 || dev >= PciState::MAX_DEVS || off + (int)size > 256) return;
    memcpy(pci->config[bus][dev][fn] + off, &val, size);
}

// ============================= SMBus =======================================
// Minimal SMBus controller stub at I/O ports 0xC000–0xC00F.
// The Xbox kernel probes SMBus to detect hardware (EEPROM at address 0x54,
// video encoder, temperature sensor, etc.).

struct SmbusState {
    uint8_t status  = 0;
    uint8_t control = 0;
    uint8_t address = 0;
    uint8_t command = 0;
    uint8_t data    = 0;
    // EEPROM data stub: all zeros except serial number area
    uint8_t eeprom[256] = {};

    SmbusState() {
        // Xbox EEPROM: first 0x30 bytes are encrypted config, rest is game data.
        // Zero is a valid "empty" state for the stub.
    }
};

static uint32_t smbus_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00: return s->status;   // SMBUS_STATUS
    case 0x02: return s->control;  // SMBUS_CONTROL
    case 0x04: return s->address;  // SMBUS_ADDRESS
    case 0x06: return s->data;     // SMBUS_DATA
    case 0x08: return s->command;  // SMBUS_COMMAND
    default:   return 0;
    }
}

static void smbus_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00:
        s->status &= ~(uint8_t)val;  // W1C (write-1-to-clear)
        return;
    case 0x02: // SMBUS_CONTROL — trigger a transaction
        s->control = (uint8_t)val;
        // Auto-complete: set status bit 4 (SMBUS_STATUS_DONE)
        s->status |= 0x10;
        // If reading from EEPROM (address 0xA9 = read from device 0x54):
        if (s->address == 0xA9 && s->command < sizeof(s->eeprom))
            s->data = s->eeprom[s->command];
        return;
    case 0x04: s->address = (uint8_t)val; return;
    case 0x06: s->data = (uint8_t)val;    return;
    case 0x08: s->command = (uint8_t)val;  return;
    }
}

// ============================= PIC (8259A) =================================
// Dual 8259A PIC: master (ports 0x20-0x21), slave (ports 0xA0-0xA1).
// Full ICW1-4 initialization sequence, IMR, IRR/ISR, OCW2/OCW3, cascade.

struct Pic8259 {
    uint8_t irr         = 0;     // Interrupt Request Register
    uint8_t imr         = 0xFF;  // Interrupt Mask Register (all masked)
    uint8_t isr         = 0;     // In-Service Register
    uint8_t vector_base = 0;     // ICW2: base vector (top 5 bits)
    uint8_t icw3        = 0;     // ICW3: cascade config
    bool    icw4_needed = false;
    bool    auto_eoi    = false;
    bool    read_isr    = false; // OCW3: read ISR (true) or IRR (false)
    int     init_step   = 0;     // 0=operational, 1..3=expecting ICW2/3/4
    bool    single_mode = false; // ICW1 bit 1: no slave

    void raise(int line)  { irr |=  (1 << line); }
    void lower(int line)  { irr &= ~(1 << line); }

    bool has_pending() const { return (irr & ~imr & ~isr) != 0; }

    int highest_pending() const {
        uint8_t bits = irr & ~imr & ~isr;
        if (!bits) return -1;
        for (int i = 0; i < 8; ++i)
            if (bits & (1 << i)) return i;
        return -1;
    }

    uint8_t ack() {
        int line = highest_pending();
        if (line < 0) return vector_base;
        irr &= ~(1 << line);
        isr |=  (1 << line);
        if (auto_eoi) isr &= ~(1 << line);
        return uint8_t(vector_base + line);
    }

    void eoi() {
        for (int i = 0; i < 8; ++i)
            if (isr & (1 << i)) { isr &= ~(1 << i); return; }
    }
    void eoi_specific(int line) { isr &= ~(1 << line); }

    void write(int port_off, uint8_t val) {
        if (port_off == 0) {
            if (val & 0x10) {                // ICW1
                init_step   = 1;
                irr = 0; isr = 0; imr = 0;
                icw4_needed = (val & 0x01) != 0;
                single_mode = (val & 0x02) != 0;
                read_isr    = false;
                return;
            }
            if ((val & 0x18) == 0x00) {      // OCW2
                int cmd = (val >> 5) & 7;
                if (cmd == 1) eoi();                     // non-specific EOI
                else if (cmd == 3) eoi_specific(val & 7); // specific EOI
                return;
            }
            if ((val & 0x18) == 0x08) {      // OCW3
                if (val & 0x02) read_isr = (val & 0x01) != 0;
                return;
            }
            return;
        }
        // port_off == 1
        if (init_step == 1) {
            vector_base = val & 0xF8;
            init_step = single_mode ? (icw4_needed ? 3 : 0) : 2;
            return;
        }
        if (init_step == 2) {
            icw3 = val;
            init_step = icw4_needed ? 3 : 0;
            return;
        }
        if (init_step == 3) {
            auto_eoi = (val & 0x02) != 0;
            init_step = 0;
            return;
        }
        imr = val;  // OCW1: write mask register
    }

    uint8_t read(int port_off) const {
        if (port_off == 0) return read_isr ? isr : irr;
        return imr;
    }
};

struct PicPair {
    Pic8259 master, slave;

    // Raise a system IRQ line (0-15).
    void raise_irq(int irq) {
        if (irq < 8) { master.raise(irq); }
        else          { slave.raise(irq - 8); master.raise(2); }
    }
    void lower_irq(int irq) {
        if (irq < 8) { master.lower(irq); }
        else {
            slave.lower(irq - 8);
            if (!slave.has_pending()) master.lower(2);
        }
    }

    bool has_pending() const { return master.has_pending(); }

    uint8_t ack() {
        int line = master.highest_pending();
        if (line == 2 && slave.has_pending()) {
            // Cascade: master acknowledges IRQ 2, slave provides the vector.
            master.irr &= ~(1 << 2);
            master.isr |=  (1 << 2);
            if (master.auto_eoi) master.isr &= ~(1 << 2);
            return slave.ack();
        }
        return master.ack();
    }
};

// I/O port handlers for PIC.
static uint32_t pic_master_read(uint16_t port, unsigned /*size*/, void* user) {
    return static_cast<PicPair*>(user)->master.read(port & 1);
}
static void pic_master_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PicPair*>(user)->master.write(port & 1, (uint8_t)val);
}
static uint32_t pic_slave_read(uint16_t port, unsigned /*size*/, void* user) {
    return static_cast<PicPair*>(user)->slave.read(port & 1);
}
static void pic_slave_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PicPair*>(user)->slave.write(port & 1, (uint8_t)val);
}

// IRQ controller callbacks for Executor.
static bool pic_irq_check(void* user) {
    return static_cast<PicPair*>(user)->has_pending();
}
static uint8_t pic_irq_ack(void* user) {
    return static_cast<PicPair*>(user)->ack();
}

// ============================= PIT (8254) ==================================
// Programmable Interval Timer — 3 channels, only channel 0 is wired to IRQ 0.
// Channel 2 gates the PC speaker / system port B.

struct PitChannel {
    uint16_t reload     = 0;     // programmed count value
    uint16_t count      = 0;     // current countdown
    uint8_t  mode       = 0;     // operating mode (0-5)
    uint8_t  access     = 0;     // 0=latch, 1=lo, 2=hi, 3=lo/hi
    bool     lo_written = false; // for lo/hi access: lo byte written
    bool     lo_read    = false; // for lo/hi read: lo byte read (need hi next)
    bool     enabled    = false;
    uint16_t latch_val  = 0;
    bool     latched    = false;
    bool     output     = false; // output pin state (for mode 3 square wave)

    void load_count(uint16_t val) {
        reload  = val ? val : 65536; // 0 means 65536
        count   = reload;
        enabled = true;
        output  = false;
    }
};

struct PitState {
    PitChannel ch[3];
    PicPair*   pic = nullptr;

    // Tick channel 0 by `n` oscillator ticks.  Raises IRQ 0 on the PIC when
    // the counter expires (modes 0, 2, 3).
    void tick(uint32_t n = 1) {
        auto& c = ch[0];
        if (!c.enabled) return;
        for (uint32_t i = 0; i < n; ++i) {
            if (c.count == 0) c.count = c.reload;
            c.count--;
            if (c.count == 0) {
                if (c.mode == 0) {
                    // Mode 0: one-shot interrupt on terminal count.
                    c.output  = true;
                    c.enabled = false;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 2) {
                    // Mode 2: rate generator — reload and fire.
                    c.count = c.reload;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 3) {
                    // Mode 3: square wave — toggle output, fire on transition.
                    c.output = !c.output;
                    c.count  = c.reload;
                    if (pic) pic->raise_irq(0);
                }
            }
        }
    }
};

static uint32_t pit_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* pit = static_cast<PitState*>(user);
    int ch = port - 0x40;
    if (ch < 0 || ch > 2) return 0;
    auto& c = pit->ch[ch];

    uint16_t val = c.latched ? c.latch_val : c.count;

    if (c.access == 1) return val & 0xFF;         // lo byte only
    if (c.access == 2) return (val >> 8) & 0xFF;  // hi byte only
    if (c.access == 3) {
        if (!c.lo_read) {
            c.lo_read = true;
            return val & 0xFF;
        }
        c.lo_read = false;
        c.latched = false;
        return (val >> 8) & 0xFF;
    }
    return 0;
}

static void pit_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* pit = static_cast<PitState*>(user);

    if (port == 0x43) {
        // Mode/command register.
        int ch_sel  = (val >> 6) & 3;
        int access  = (val >> 4) & 3;
        int mode    = (val >> 1) & 7;

        if (ch_sel == 3) {
            // Read-back command (not implemented — rare).
            return;
        }
        if (access == 0) {
            // Counter latch command.
            pit->ch[ch_sel].latch_val = pit->ch[ch_sel].count;
            pit->ch[ch_sel].latched   = true;
            return;
        }
        pit->ch[ch_sel].access     = (uint8_t)access;
        pit->ch[ch_sel].mode       = (uint8_t)(mode & 3); // modes 4-7 alias to 0-3
        pit->ch[ch_sel].lo_written = false;
        pit->ch[ch_sel].lo_read    = false;
        pit->ch[ch_sel].enabled    = false;
        return;
    }

    // Data register write (port 0x40-0x42).
    int ch = port - 0x40;
    if (ch < 0 || ch > 2) return;
    auto& c = pit->ch[ch];

    if (c.access == 1) {
        // Lo byte only.
        c.load_count((c.reload & 0xFF00) | (val & 0xFF));
    } else if (c.access == 2) {
        // Hi byte only.
        c.load_count(uint16_t(((val & 0xFF) << 8) | (c.reload & 0xFF)));
    } else if (c.access == 3) {
        // Lo/hi byte pair.
        if (!c.lo_written) {
            c.reload = (c.reload & 0xFF00) | (val & 0xFF);
            c.lo_written = true;
        } else {
            c.reload = uint16_t(((val & 0xFF) << 8) | (c.reload & 0xFF));
            c.lo_written = false;
            c.load_count(c.reload);
        }
    }
}

// Tick callback for executor run loop.
static void pit_tick_callback(void* user) {
    static_cast<PitState*>(user)->tick();
}

// ============================= System Port B (0x61) ========================

static uint32_t sysctl_read(uint16_t, unsigned, void*) { return 0x20; /* timer 2 output */ }
static void sysctl_write(uint16_t, uint32_t, unsigned, void*) {}

// ============================= Debug console (0xE9) ========================

static void debug_console_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

// ============================= Setup =======================================
// All device state is heap-allocated and owned by XboxHardware.

struct XboxHardware {
    MmioMap     mmio;
    Nv2aState   nv2a;
    IoApicState ioapic;
    FlashState  flash;
    PciState    pci;
    SmbusState  smbus;
    PicPair     pic;
    PitState    pit;
};

// Set up the Xbox MMIO map and I/O ports on an Executor.
// Returns a heap-allocated XboxHardware whose lifetime must exceed the Executor.
inline XboxHardware* xbox_setup(Executor& exec) {
    auto* hw = new XboxHardware();

    // PCI device table
    hw->pci.init_xbox_devices();

    // Initialize Executor first so that exec.ram is allocated.
    exec.init(&hw->mmio);

    // MMIO regions (must come after init — exec.ram is the user pointer)
    hw->mmio.add(RAM_MIRROR_BASE, RAM_MIRROR_SIZE,
                 ram_mirror_read, ram_mirror_write, exec.ram);
    hw->mmio.add(FLASH_BASE, FLASH_SIZE,
                 flash_read, flash_write, &hw->flash);
    hw->mmio.add(NV2A_BASE, NV2A_SIZE,
                 nv2a_read, nv2a_write, &hw->nv2a);
    hw->mmio.add(APU_BASE, APU_SIZE,
                 apu_read, apu_write, nullptr);
    hw->mmio.add(IOAPIC_BASE, IOAPIC_SIZE,
                 ioapic_read, ioapic_write, &hw->ioapic);
    // BIOS shadow = flash alias
    hw->mmio.add(BIOS_BASE, BIOS_SIZE,
                 flash_read, flash_write, &hw->flash);

    // I/O ports
    exec.register_io(0x20, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0x21, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0xA0, pic_slave_read, pic_slave_write, &hw->pic);
    exec.register_io(0xA1, pic_slave_read, pic_slave_write, &hw->pic);

    // Connect PIC as the interrupt controller.
    exec.irq_check = pic_irq_check;
    exec.irq_ack   = pic_irq_ack;
    exec.irq_user  = &hw->pic;

    exec.register_io(0x40, pit_io_read, pit_io_write, &hw->pit); // PIT ch0
    exec.register_io(0x41, pit_io_read, pit_io_write, &hw->pit); // PIT ch1
    exec.register_io(0x42, pit_io_read, pit_io_write, &hw->pit); // PIT ch2
    exec.register_io(0x43, pit_io_read, pit_io_write, &hw->pit); // PIT mode

    // Wire PIT channel 0 → IRQ 0 on PIC.
    hw->pit.pic = &hw->pic;

    // Periodic tick: call PIT every trace dispatch.
    exec.tick_fn     = pit_tick_callback;
    exec.tick_user   = &hw->pit;
    exec.tick_period = 1;

    exec.register_io(0x61, sysctl_read, sysctl_write);         // System B
    exec.register_io(0xE9, nullptr, debug_console_write);       // Debug
    exec.register_io(0xCF8, pci_io_read_cf8, pci_io_write_cf8, &hw->pci);
    exec.register_io(0xCFC, pci_io_read_cfc, pci_io_write_cfc, &hw->pci);

    // SMBus (0xC000–0xC00F): register even-numbered ports used by kernel
    for (uint16_t p = 0xC000; p <= 0xC00E; p += 2)
        exec.register_io(p, smbus_io_read, smbus_io_write, &hw->smbus);

    return hw;
}

} // namespace xbox
