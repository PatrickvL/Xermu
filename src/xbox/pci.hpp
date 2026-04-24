#pragma once
// ---------------------------------------------------------------------------
// pci.hpp — PCI configuration space via I/O ports 0xCF8/0xCFC.
//
// Two PCI buses (0 and 1).  Xbox device table with correct vendor/device
// IDs, class codes, BARs, interrupt lines, and header types.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cstring>

namespace xbox {

// Named PCI config-space offsets (Type 0 header).
namespace pci_cfg {
static constexpr uint8_t VENDOR_ID       = 0x00;  // 16-bit
static constexpr uint8_t DEVICE_ID       = 0x02;  // 16-bit
static constexpr uint8_t COMMAND         = 0x04;  // 16-bit
static constexpr uint8_t STATUS          = 0x06;  // 16-bit
static constexpr uint8_t REVISION        = 0x08;
static constexpr uint8_t PROG_IF         = 0x09;
static constexpr uint8_t SUBCLASS        = 0x0A;
static constexpr uint8_t CLASS_CODE      = 0x0B;
static constexpr uint8_t HEADER_TYPE     = 0x0E;
static constexpr uint8_t BAR0            = 0x10;
static constexpr uint8_t BAR1            = 0x14;
static constexpr uint8_t BAR2            = 0x18;
static constexpr uint8_t BAR3            = 0x1C;
static constexpr uint8_t BAR4            = 0x20;
static constexpr uint8_t BAR5            = 0x24;
static constexpr uint8_t SUBSYSTEM_VID   = 0x2C;  // 16-bit
static constexpr uint8_t SUBSYSTEM_ID    = 0x2E;  // 16-bit
static constexpr uint8_t INTERRUPT_LINE  = 0x3C;
static constexpr uint8_t INTERRUPT_PIN   = 0x3D;
// PCI-PCI bridge (Type 1 header)
static constexpr uint8_t PRIMARY_BUS     = 0x18;
static constexpr uint8_t SECONDARY_BUS   = 0x19;
static constexpr uint8_t SUBORDINATE_BUS = 0x1A;
} // namespace pci_cfg

struct PciState {
    uint32_t config_address = 0;
    static constexpr int MAX_DEVS = 32;
    uint8_t config[2][MAX_DEVS][8][256] = {};

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

    void set_bar(int bus, int dev, int fn, uint8_t bar_offset, uint32_t value) {
        memcpy(config[bus][dev][fn] + bar_offset, &value, 4);
    }

    void set_irq(int bus, int dev, int fn, uint8_t line, uint8_t pin) {
        config[bus][dev][fn][pci_cfg::INTERRUPT_LINE] = line;
        config[bus][dev][fn][pci_cfg::INTERRUPT_PIN]  = pin;
    }

    void set_subsystem(int bus, int dev, int fn, uint16_t svid, uint16_t sid) {
        memcpy(config[bus][dev][fn] + pci_cfg::SUBSYSTEM_VID, &svid, 2);
        memcpy(config[bus][dev][fn] + pci_cfg::SUBSYSTEM_ID, &sid, 2);
    }

    void init_xbox_devices() {
        // --- Bus 0, Dev 0: MCPX Host Bridge ---
        set_id(0, 0, 0, 0x10DE, 0x02A5, 0x06, 0x00, 0x00, 0xB1);

        // --- Bus 0, Dev 1, Fn 0: ISA/LPC Bridge ---
        set_id(0, 1, 0, 0x10DE, 0x01B2, 0x06, 0x01, 0x00, 0xB1);

        // --- Bus 0, Dev 1, Fn 1: SMBus Controller ---
        set_id(0, 1, 1, 0x10DE, 0x01B4, 0x0C, 0x05, 0x00, 0xB1);
        set_bar(0, 1, 1, pci_cfg::BAR1, 0xC001);   // I/O BAR at 0xC000 (bit 0 = I/O)
        set_irq(0, 1, 1, 11, 1);  // IRQ 11, INTA#

        // --- Bus 0, Dev 2: NV2A GPU (AGP) ---
        set_id(0, 2, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
        set_bar(0, 2, 0, pci_cfg::BAR0, 0xFD000000); // NV2A MMIO (memory BAR)
        set_irq(0, 2, 0, 3, 1);   // IRQ 3, INTA#

        // --- Bus 0, Dev 3: MCPX APU ---
        set_id(0, 3, 0, 0x10DE, 0x01B0, 0x04, 0x01, 0x00, 0xB1);
        set_bar(0, 3, 0, pci_cfg::BAR0, 0xFE800000); // APU MMIO
        set_irq(0, 3, 0, 5, 1);   // IRQ 5, INTA#

        // --- Bus 0, Dev 4: USB OHCI #0 ---
        set_id(0, 4, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        set_bar(0, 4, 0, pci_cfg::BAR0, 0xFED00000); // USB0 MMIO
        set_irq(0, 4, 0, 1, 1);   // IRQ 1, INTA#

        // --- Bus 0, Dev 5: USB OHCI #1 ---
        set_id(0, 5, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        set_bar(0, 5, 0, pci_cfg::BAR0, 0xFED08000); // USB1 MMIO
        set_irq(0, 5, 0, 9, 1);   // IRQ 9, INTA#

        // --- Bus 0, Dev 9: IDE Controller ---
        set_id(0, 9, 0, 0x10DE, 0x01BC, 0x01, 0x01, 0x8A, 0xB1);
        set_bar(0, 9, 0, pci_cfg::BAR4, 0xFF61);    // Bus master I/O (bit 0 = I/O)
        set_irq(0, 9, 0, 14, 1);  // IRQ 14, INTA#

        // --- Bus 0, Dev 30: AGP Bridge (Type 1 header) ---
        set_id(0, 30, 0, 0x10DE, 0x01B7, 0x06, 0x04, 0x00, 0xB1);
        config[0][30][0][pci_cfg::HEADER_TYPE] = 0x01;  // PCI-PCI bridge
        config[0][30][0][pci_cfg::PRIMARY_BUS] = 0;
        config[0][30][0][pci_cfg::SECONDARY_BUS] = 1;
        config[0][30][0][pci_cfg::SUBORDINATE_BUS] = 1;

        // --- Bus 1, Dev 0: NV2A GPU (secondary bus view) ---
        set_id(1, 0, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
        set_bar(1, 0, 0, pci_cfg::BAR0, 0xFD000000);
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
    if (!(addr & 0x80000000u)) return 0xFFFFFFFFu;
    int bus = (addr >> 16) & 0xFF;
    int dev = (addr >> 11) & 0x1F;
    int fn  = (addr >>  8) & 0x07;
    int off = (addr & 0xFC) + (port - 0xCFC);
    if (bus > 1 || dev >= PciState::MAX_DEVS || off + (int)size > 256)
        return 0xFFFFFFFFu;
    // Non-existent device: vendor ID 0x0000 means no device present.
    uint16_t vendor = 0;
    memcpy(&vendor, pci->config[bus][dev][fn] + pci_cfg::VENDOR_ID, 2);
    if (vendor == 0) return 0xFFFFFFFFu;
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

} // namespace xbox
