#pragma once
// Miscellaneous I/O ports — System Port B (0x61), debug console (0xE9),
// and manufacturing test port (0x80).
#include <cstdint>
#include <cstdio>

namespace xbox {

// =========================== System Port B (0x61) ==========================
// Bits:
//   [0] Timer 2 gate      (R/W)
//   [1] Speaker data       (R/W)
//   [4] Refresh toggle     (RO, toggles)
//   [5] Timer 2 output     (RO)
//   [6] I/O channel check  (RO)
//   [7] Parity check       (RO)

struct MiscIoState {
    uint8_t sysctl        = 0x20;  // default: refresh toggle set
    uint8_t post_code     = 0x00;  // last POST code written to 0x80
    uint32_t sysctl_reads = 0;     // count reads for toggle
};

static uint32_t sysctl_read(uint16_t, unsigned, void* user) {
    auto* s = static_cast<MiscIoState*>(user);
    // Toggle refresh bit (bit 4) on each read, mimicking hardware behavior.
    s->sysctl ^= 0x10;
    s->sysctl_reads++;
    return s->sysctl;
}

static void sysctl_write(uint16_t, uint32_t val, unsigned, void* user) {
    auto* s = static_cast<MiscIoState*>(user);
    // Only bits 0-1 are writable (timer gate, speaker data).
    s->sysctl = (s->sysctl & 0xFC) | (uint8_t)(val & 0x03);
}

// =========================== Debug Console (0xE9) ==========================

static uint32_t debug_console_read(uint16_t, unsigned, void*) {
    return 0xE9;  // Magic signature: port 0xE9 is present
}

static void debug_console_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

// =========================== POST Code (0x80) ==============================

static uint32_t post_code_read(uint16_t, unsigned, void* user) {
    return static_cast<MiscIoState*>(user)->post_code;
}

static void post_code_write(uint16_t, uint32_t val, unsigned, void* user) {
    static_cast<MiscIoState*>(user)->post_code = (uint8_t)(val & 0xFF);
}

} // namespace xbox
