#pragma once
// ---------------------------------------------------------------------------
// cmos.hpp — MC146818 CMOS/RTC emulation (ports 0x70-0x71).
//
// Provides enough emulation for nboxkrnl's HalpCalibrateStallExecution:
//   - Register select (port 0x70 write)
//   - Register read/write (port 0x71)
//   - Periodic interrupt (register B bit 6) → IRQ 8 via PIC
//   - ELCR ports 0x4D0/0x4D1 (edge/level control)
// ---------------------------------------------------------------------------

#include "pic.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

struct CmosState {
    uint8_t  selected_reg = 0;
    uint8_t  regs[128]    = {};
    PicPair* pic          = nullptr;

    // Periodic interrupt timing (in tick callbacks).
    // When register B bit 6 is set, we count ticks and raise IRQ 8
    // after 'period_ticks' ticks.
    uint32_t period_ticks  = 0;   // 0 = disabled
    uint32_t tick_counter  = 0;

    // ELCR (edge/level control registers).
    uint8_t  elcr[2] = { 0, 0 };  // [0]=master 0x4D0, [1]=slave 0x4D1

    void init() {
        memset(regs, 0, sizeof(regs));
        // Default register A: divider = 010 (32.768 kHz), rate = 0110 (1024 Hz)
        regs[0x0A] = 0x26;
        // Default register B: 24h mode, BCD format
        regs[0x0B] = 0x02;
        // Default register D: VRT bit set (valid RAM and time)
        regs[0x0D] = 0x80;
    }

    // Called every tick callback (once per trace dispatch).
    // Returns true if IRQ 8 should be raised.
    bool tick() {
        if (period_ticks == 0) return false;
        if (++tick_counter >= period_ticks) {
            tick_counter = 0;
            // Set periodic interrupt flag in register C.
            regs[0x0C] |= 0x40;  // PF (periodic flag)
            regs[0x0C] |= 0x80;  // IRQF (interrupt request flag)
            return true;
        }
        return false;
    }

    // Compute period_ticks from register A rate bits.
    // Rate = bits [3:0] of register A.
    // Actual period = 1/(32768 >> (rate-1)) seconds.
    // We approximate in tick counts (1 tick ≈ 1 trace dispatch).
    void update_period() {
        bool periodic_enabled = (regs[0x0B] & 0x40) != 0;
        if (!periodic_enabled) {
            period_ticks = 0;
            tick_counter = 0;
            return;
        }
        uint8_t rate = regs[0x0A] & 0x0F;
        if (rate == 0) {
            period_ticks = 0;
            return;
        }
        // Rate 0x0D = 8 Hz → 125ms period.
        // In emulation, we fire quickly to avoid spin loops timing out.
        // Use a small fixed tick count to ensure the ISR fires before
        // the spin detector triggers (threshold = 32).
        // Smaller = faster calibration, but 8 is well under the 32 threshold.
        period_ticks = 8;
        tick_counter = 0;
    }
};

// Port 0x70 write: select CMOS register (and optionally disable NMI).
// Port 0x71 read/write: data register.

static uint32_t cmos_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* cmos = static_cast<CmosState*>(user);
    if (port == 0x70) return cmos->selected_reg;
    if (port == 0x71) {
        uint8_t reg = cmos->selected_reg & 0x7F;
        uint8_t val = cmos->regs[reg];
        // Reading register C clears all flags (acknowledges interrupt).
        if (reg == 0x0C) cmos->regs[0x0C] = 0;
        return val;
    }
    return 0xFF;
}

static void cmos_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* cmos = static_cast<CmosState*>(user);
    if (port == 0x70) {
        cmos->selected_reg = (uint8_t)(val & 0xFF);
        return;
    }
    if (port == 0x71) {
        uint8_t reg = cmos->selected_reg & 0x7F;
        cmos->regs[reg] = (uint8_t)(val & 0xFF);
        // Update periodic interrupt timing when A or B changes.
        if (reg == 0x0A || reg == 0x0B) {
            cmos->update_period();
        }
    }
}

// ELCR ports 0x4D0 (master) / 0x4D1 (slave).

static uint32_t elcr_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* cmos = static_cast<CmosState*>(user);
    return cmos->elcr[port - 0x4D0];
}

static void elcr_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* cmos = static_cast<CmosState*>(user);
    cmos->elcr[port - 0x4D0] = (uint8_t)(val & 0xFF);
}

} // namespace xbox
