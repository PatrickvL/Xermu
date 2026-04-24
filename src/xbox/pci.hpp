#pragma once
// PCI configuration space via I/O ports 0xCF8/0xCFC.
#include <cstdint>
#include <cstring>

namespace xbox {

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

    void init_xbox_devices() {
        set_id(0, 0, 0, 0x10DE, 0x02A5, 0x06, 0x00, 0x00, 0xB1);
        set_id(0, 1, 0, 0x10DE, 0x01B2, 0x06, 0x01, 0x00, 0xB1);
        set_id(0, 1, 1, 0x10DE, 0x01B4, 0x0C, 0x05, 0x00, 0xB1);
        { uint16_t bar = 0xC001; memcpy(config[0][1][1] + 0x14, &bar, 2); }
        set_id(0, 2, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
        set_id(0, 3, 0, 0x10DE, 0x01B0, 0x04, 0x01, 0x00, 0xB1);
        set_id(0, 4, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        set_id(0, 5, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        set_id(0, 9, 0, 0x10DE, 0x01BC, 0x01, 0x01, 0x8A, 0xB1);
        set_id(0, 30, 0, 0x10DE, 0x01B7, 0x06, 0x04, 0x00, 0xB1);
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
    if (!(addr & 0x80000000u)) return 0xFFFFFFFFu;
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

} // namespace xbox
