#pragma once
// PIT 8254 — Programmable Interval Timer (3 channels, ch0 → IRQ 0).
#include "pic.hpp"
#include <cstdint>

namespace xbox {

struct PitChannel {
    uint16_t reload     = 0;
    uint16_t count      = 0;
    uint8_t  mode       = 0;
    uint8_t  access     = 0;
    bool     lo_written = false;
    bool     lo_read    = false;
    bool     enabled    = false;
    uint16_t latch_val  = 0;
    bool     latched    = false;
    bool     output     = false;

    void load_count(uint16_t val) {
        reload  = val ? val : 65536;
        count   = reload;
        enabled = true;
        output  = false;
    }
};

struct PitState {
    PitChannel ch[3];
    PicPair*   pic = nullptr;

    void tick(uint32_t n = 1) {
        auto& c = ch[0];
        if (!c.enabled) return;
        for (uint32_t i = 0; i < n; ++i) {
            if (c.count == 0) c.count = c.reload;
            c.count--;
            if (c.count == 0) {
                if (c.mode == 0) {
                    c.output  = true;
                    c.enabled = false;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 2) {
                    c.count = c.reload;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 3) {
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

    if (c.access == 1) return val & 0xFF;
    if (c.access == 2) return (val >> 8) & 0xFF;
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
        int ch_sel  = (val >> 6) & 3;
        int access  = (val >> 4) & 3;
        int mode    = (val >> 1) & 7;

        if (ch_sel == 3) return;
        if (access == 0) {
            pit->ch[ch_sel].latch_val = pit->ch[ch_sel].count;
            pit->ch[ch_sel].latched   = true;
            return;
        }
        pit->ch[ch_sel].access     = (uint8_t)access;
        pit->ch[ch_sel].mode       = (uint8_t)(mode & 3);
        pit->ch[ch_sel].lo_written = false;
        pit->ch[ch_sel].lo_read    = false;
        pit->ch[ch_sel].enabled    = false;
        return;
    }

    int ch = port - 0x40;
    if (ch < 0 || ch > 2) return;
    auto& c = pit->ch[ch];

    if (c.access == 1) {
        c.load_count((c.reload & 0xFF00) | (val & 0xFF));
    } else if (c.access == 2) {
        c.load_count(uint16_t(((val & 0xFF) << 8) | (c.reload & 0xFF)));
    } else if (c.access == 3) {
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

} // namespace xbox
