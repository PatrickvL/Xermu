#pragma once
// Miscellaneous I/O ports — System Port B (0x61) and debug console (0xE9).
#include <cstdint>
#include <cstdio>

namespace xbox {

static uint32_t sysctl_read(uint16_t, unsigned, void*) { return 0x20; }
static void sysctl_write(uint16_t, uint32_t, unsigned, void*) {}

static void debug_console_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

} // namespace xbox
