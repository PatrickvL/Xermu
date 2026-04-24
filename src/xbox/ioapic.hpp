#pragma once
// I/O APIC (82093AA-compatible) register stub.
#include "address_map.hpp"
#include <cstdint>

namespace xbox {

struct IoApicState {
    uint32_t index = 0;
    uint32_t regs[24 * 2 + 2] = {};

    IoApicState() {
        regs[0] = 0;
        regs[1] = 0x00170011;
    }
};

static uint32_t ioapic_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) return s->index;
    if (off == 0x10 && s->index < 64) return s->regs[s->index];
    return 0;
}

static void ioapic_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) { s->index = val & 0xFF; return; }
    if (off == 0x10 && s->index < 64) { s->regs[s->index] = val; return; }
}

} // namespace xbox
