#pragma once
// OHCI USB host controller stubs (2 controllers, 2 ports each).
#include <cstdint>
#include <cstring>

namespace xbox {

struct OhciState {
    uint32_t regs[64] = {};

    static constexpr uint32_t HC_REVISION         = 0x00;
    static constexpr uint32_t HC_CONTROL          = 0x04;
    static constexpr uint32_t HC_COMMAND_STATUS   = 0x08;
    static constexpr uint32_t HC_INTERRUPT_STATUS = 0x0C;
    static constexpr uint32_t HC_INTERRUPT_ENABLE = 0x10;
    static constexpr uint32_t HC_INTERRUPT_DISABLE= 0x14;
    static constexpr uint32_t HC_FM_INTERVAL      = 0x38;
    static constexpr uint32_t HC_FM_REMAINING     = 0x3C;
    static constexpr uint32_t HC_FM_NUMBER        = 0x40;
    static constexpr uint32_t HC_PERIODIC_START   = 0x44;
    static constexpr uint32_t HC_RH_DESCRIPTOR_A  = 0x48;
    static constexpr uint32_t HC_RH_STATUS        = 0x50;
    static constexpr uint32_t HC_RH_PORT_STATUS0  = 0x54;

    uint32_t num_ports;

    void init(uint32_t ports = 2) {
        memset(regs, 0, sizeof(regs));
        num_ports = ports;
        regs[HC_REVISION >> 2]        = 0x00000110;
        regs[HC_CONTROL >> 2]         = 0x00000000;
        regs[HC_FM_INTERVAL >> 2]     = 0x27782EDF;
        regs[HC_RH_DESCRIPTOR_A >> 2] = ports & 0xFF;
    }
};

static uint32_t ohci_read(uint32_t pa, unsigned size, void* user) {
    auto* ohci = static_cast<OhciState*>(user);
    uint32_t off = pa & 0xFFF;
    if (off >= sizeof(ohci->regs)) return 0;
    uint32_t val = ohci->regs[off >> 2];
    if (off == OhciState::HC_FM_REMAINING)
        val = 0x2710;
    return val;
}

static void ohci_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* ohci = static_cast<OhciState*>(user);
    uint32_t off = pa & 0xFFF;
    if (off >= sizeof(ohci->regs)) return;

    switch (off) {
    case OhciState::HC_COMMAND_STATUS:
        if (val & 1) { uint32_t ports = ohci->num_ports; ohci->init(ports); }
        ohci->regs[off >> 2] |= (val & ~1u);
        break;
    case OhciState::HC_INTERRUPT_STATUS:
        ohci->regs[off >> 2] &= ~val;
        break;
    case OhciState::HC_INTERRUPT_DISABLE:
        ohci->regs[OhciState::HC_INTERRUPT_ENABLE >> 2] &= ~val;
        break;
    case OhciState::HC_RH_STATUS:
        ohci->regs[off >> 2] = val;
        break;
    default:
        if (off >= OhciState::HC_RH_PORT_STATUS0 &&
            off < OhciState::HC_RH_PORT_STATUS0 + ohci->num_ports * 4) {
            ohci->regs[off >> 2] = val & 0x001F0000u;
        } else {
            ohci->regs[off >> 2] = val;
        }
        break;
    }
}

} // namespace xbox
