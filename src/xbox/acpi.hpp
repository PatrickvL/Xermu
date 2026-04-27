#pragma once
// Xbox ACPI — PM Timer, PM1a status/control, GPE0 registers.
//
// The Xbox MCPX southbridge exposes standard ACPI registers at I/O base 0x8000.
// Key ports:
//   0x8000-0x8001  PM1a_STS / PM1a_EN (event status/enable)
//   0x8004-0x8005  PM1a_CNT (control)
//   0x8008-0x800B  PM_TMR (32-bit 3.579545 MHz counter)
//   0x80C0         GPE0_STS (general-purpose event status, W1C)
//   0x80C1         GPE0_EN  (general-purpose event enable)
//
// Reference: xemu hw/xbox/acpi_xbox.c, ACPI spec (PM Timer).
#include <cstdint>

namespace xbox {

struct AcpiState {
    // PM Timer: 32-bit free-running counter at 3,579,545 Hz.
    // On real Xbox this is 32-bit (not 24-bit like standard ACPI).
    uint32_t pmtmr       = 0;

    // PM1a event registers (16-bit each)
    uint16_t pm1a_sts    = 0;   // PM1a_STS: event status (W1C)
    uint16_t pm1a_en     = 0;   // PM1a_EN: event enable
    uint16_t pm1a_cnt    = 0;   // PM1a_CNT: control (SCI_EN, etc.)

    // GPE0 registers (8-bit each)
    uint8_t  gpe0_sts    = 0;   // GPE0_STS: W1C status
    uint8_t  gpe0_en     = 0;   // GPE0_EN: enable

    // Advance the PM timer by the given number of ticks.
    void tick(uint32_t n = 4) { pmtmr += n; }
};

// I/O port callbacks — user pointer is AcpiState*.

static uint32_t acpi_pm1a_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<AcpiState*>(user);
    switch (port) {
    case 0x8000: return s->pm1a_sts;
    case 0x8001: return s->pm1a_en;
    default:     return 0;
    }
}
static void acpi_pm1a_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<AcpiState*>(user);
    switch (port) {
    case 0x8000: s->pm1a_sts &= ~(uint16_t)val; break;  // W1C
    case 0x8001: s->pm1a_en = (uint16_t)val;     break;
    }
}

static uint32_t acpi_pmtmr_read(uint16_t, unsigned /*size*/, void* user) {
    return static_cast<AcpiState*>(user)->pmtmr;
}

static uint32_t acpi_gpe0_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<AcpiState*>(user);
    return (port == 0x80C0) ? s->gpe0_sts : s->gpe0_en;
}
static void acpi_gpe0_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<AcpiState*>(user);
    if (port == 0x80C0) s->gpe0_sts &= ~(uint8_t)val;  // W1C
    else                s->gpe0_en   = (uint8_t)val;
}

} // namespace xbox
