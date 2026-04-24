#pragma once
// OHCI USB host controller stubs (2 controllers, 2 ports each).
#include <cstdint>
#include <cstring>

namespace xbox {

// ====================== OHCI Register Offsets =============================

namespace ohci {
static constexpr uint32_t REVISION          = 0x00;
static constexpr uint32_t CONTROL           = 0x04;
static constexpr uint32_t COMMAND_STATUS    = 0x08;
static constexpr uint32_t INTERRUPT_STATUS  = 0x0C;
static constexpr uint32_t INTERRUPT_ENABLE  = 0x10;
static constexpr uint32_t INTERRUPT_DISABLE = 0x14;
static constexpr uint32_t HCCA              = 0x18;
static constexpr uint32_t PERIOD_CURRENT_ED = 0x1C;
static constexpr uint32_t CONTROL_HEAD_ED   = 0x20;
static constexpr uint32_t CONTROL_CURRENT_ED= 0x24;
static constexpr uint32_t BULK_HEAD_ED      = 0x28;
static constexpr uint32_t BULK_CURRENT_ED   = 0x2C;
static constexpr uint32_t DONE_HEAD         = 0x30;
static constexpr uint32_t FM_INTERVAL       = 0x34;
static constexpr uint32_t FM_REMAINING      = 0x38;
static constexpr uint32_t FM_NUMBER         = 0x3C;
static constexpr uint32_t PERIODIC_START    = 0x40;
static constexpr uint32_t LS_THRESHOLD      = 0x44;
static constexpr uint32_t RH_DESCRIPTOR_A   = 0x48;
static constexpr uint32_t RH_DESCRIPTOR_B   = 0x4C;
static constexpr uint32_t RH_STATUS         = 0x50;
static constexpr uint32_t RH_PORT_STATUS0   = 0x54;

// Control register functional state
static constexpr uint32_t CTRL_CBSR_MASK    = 0x03;       // control/bulk service ratio
static constexpr uint32_t CTRL_HCFS_MASK    = 0xC0;       // host controller functional state
static constexpr uint32_t CTRL_HCFS_RESET   = 0x00;
static constexpr uint32_t CTRL_HCFS_RESUME  = 0x40;
static constexpr uint32_t CTRL_HCFS_OPER    = 0x80;
static constexpr uint32_t CTRL_HCFS_SUSPEND = 0xC0;

// Interrupt status/enable bits
static constexpr uint32_t INTR_SO    = 0x00000001; // scheduling overrun
static constexpr uint32_t INTR_WDH   = 0x00000002; // writeback done head
static constexpr uint32_t INTR_SF    = 0x00000004; // start of frame
static constexpr uint32_t INTR_RD    = 0x00000008; // resume detected
static constexpr uint32_t INTR_FNO   = 0x00000020; // frame number overflow
static constexpr uint32_t INTR_RHSC  = 0x00000040; // root hub status change
static constexpr uint32_t INTR_MIE   = 0x80000000; // master interrupt enable
} // namespace ohci

struct OhciState {
    uint32_t regs[64] = {};
    uint32_t num_ports = 2;
    uint32_t frame_counter = 0;  // monotonic frame count (for tick)

    void init(uint32_t ports = 2) {
        memset(regs, 0, sizeof(regs));
        num_ports = ports;
        frame_counter = 0;
        regs[ohci::REVISION >> 2]        = 0x00000110;  // OHCI 1.1
        regs[ohci::CONTROL >> 2]         = 0x00000000;
        regs[ohci::FM_INTERVAL >> 2]     = 0x27782EDF;  // default: bit-time=0x2EDF, FSMPS=0x2778
        regs[ohci::RH_DESCRIPTOR_A >> 2] = ports & 0xFF;
    }

    // Advance the frame number (called by tick).
    void tick_frame() {
        uint32_t ctrl = regs[ohci::CONTROL >> 2];
        if ((ctrl & ohci::CTRL_HCFS_MASK) != ohci::CTRL_HCFS_OPER)
            return;  // only advance when operational
        uint32_t fn = regs[ohci::FM_NUMBER >> 2];
        uint32_t old_fn = fn;
        fn = (fn + 1) & 0xFFFF;
        regs[ohci::FM_NUMBER >> 2] = fn;
        // Frame number overflow: bit 15 toggled
        if ((old_fn ^ fn) & 0x8000)
            regs[ohci::INTERRUPT_STATUS >> 2] |= ohci::INTR_FNO;
        // Start of frame interrupt
        regs[ohci::INTERRUPT_STATUS >> 2] |= ohci::INTR_SF;
        frame_counter++;
    }
};

static uint32_t ohci_read(uint32_t pa, unsigned size, void* user) {
    auto* s = static_cast<OhciState*>(user);
    uint32_t off = pa & 0xFFF;
    if (off >= sizeof(s->regs)) return 0;
    uint32_t val = s->regs[off >> 2];
    if (off == ohci::FM_REMAINING)
        val = 0x2710;  // always report ~halfway through frame
    return val;
}

static void ohci_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* s = static_cast<OhciState*>(user);
    uint32_t off = pa & 0xFFF;
    if (off >= sizeof(s->regs)) return;

    switch (off) {
    case ohci::COMMAND_STATUS:
        if (val & 1) {
            uint32_t ports = s->num_ports;
            s->init(ports);
        }
        s->regs[off >> 2] |= (val & ~1u);
        break;
    case ohci::INTERRUPT_STATUS:
        s->regs[off >> 2] &= ~val;  // W1C
        break;
    case ohci::INTERRUPT_DISABLE:
        s->regs[ohci::INTERRUPT_ENABLE >> 2] &= ~val;
        break;
    case ohci::INTERRUPT_ENABLE:
        s->regs[off >> 2] |= val;
        break;
    case ohci::RH_STATUS:
        s->regs[off >> 2] = val;
        break;
    default:
        if (off >= ohci::RH_PORT_STATUS0 &&
            off < ohci::RH_PORT_STATUS0 + s->num_ports * 4) {
            s->regs[off >> 2] = val & 0x001F0000u;
        } else {
            s->regs[off >> 2] = val;
        }
        break;
    }
}

} // namespace xbox
