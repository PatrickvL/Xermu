#pragma once
// ---------------------------------------------------------------------------
// ioapic.hpp — I/O APIC (82093AA-compatible) register emulation.
//
// The Xbox I/O APIC is an 82093AA-compatible unit at PA 0xFEC00000.
// It has 24 interrupt redirection entries (IRQ 0–23), each a 64-bit
// register pair.  Access is indirect: write the register index to
// IOREGSEL (offset 0x00), then read/write IOWIN (offset 0x10).
// ---------------------------------------------------------------------------
#include "address_map.hpp"
#include <cstdint>

namespace xbox {

// Named register indices for indirect access via IOREGSEL.
namespace ioapic {
static constexpr uint32_t ID       = 0x00;  // IOAPIC ID (bits [27:24])
static constexpr uint32_t VER      = 0x01;  // version (bits [7:0]) + max redir (bits [23:16])
static constexpr uint32_t ARB      = 0x02;  // arbitration ID (bits [27:24])
// Redirection table: entries 0–23, each 2 registers (low + high dword).
// Entry N low  = REDIR_BASE + N*2
// Entry N high = REDIR_BASE + N*2 + 1
static constexpr uint32_t REDIR_BASE = 0x10;
static constexpr uint32_t MAX_IRQS   = 24;
static constexpr uint32_t REG_COUNT  = REDIR_BASE + MAX_IRQS * 2;  // 0x10 + 48 = 64

// Register field values
static constexpr uint32_t VERSION_82093AA    = 0x00170011;  // version 0x11, max redir 23
static constexpr uint32_t ENTRY_MASKED       = 0x00010000;  // mask bit (bit 16) set
static constexpr uint32_t ID_FIELD_MASK      = 0x0F000000;  // ID bits [27:24]
static constexpr uint32_t ENTRY_DEST_MASK    = 0xFF000000;  // destination bits [31:24]
static constexpr uint32_t ENTRY_RO_MASK      = (1u << 12) | (1u << 14);  // delivery_status + remote_irr
} // namespace ioapic

struct IoApicState {
    uint32_t ioregsel = 0;
    uint32_t regs[ioapic::REG_COUNT] = {};

    IoApicState() {
        regs[ioapic::ID]  = 0;
        regs[ioapic::VER] = ioapic::VERSION_82093AA;
        regs[ioapic::ARB] = 0;
        // Default all redirection entries to masked (bit 16 = 1).
        for (uint32_t i = 0; i < ioapic::MAX_IRQS; ++i)
            regs[ioapic::REDIR_BASE + i * 2] = ioapic::ENTRY_MASKED;
    }
};

static uint32_t ioapic_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) return s->ioregsel;
    if (off == 0x10 && s->ioregsel < ioapic::REG_COUNT) return s->regs[s->ioregsel];
    return 0;
}

static void ioapic_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) { s->ioregsel = val & 0xFF; return; }
    if (off == 0x10) {
        uint32_t idx = s->ioregsel;
        if (idx >= ioapic::REG_COUNT) return;
        // ID: only bits [27:24] writable.
        if (idx == ioapic::ID) { s->regs[idx] = val & ioapic::ID_FIELD_MASK; return; }
        // VER and ARB are read-only.
        if (idx == ioapic::VER || idx == ioapic::ARB) return;
        // Redirection entry low: vector[7:0], delivery[10:8], dest_mode[11],
        // delivery_status[12] (RO), polarity[13], remote_irr[14] (RO),
        // trigger[15], mask[16].
        if (idx >= ioapic::REDIR_BASE) {
            uint32_t entry_off = idx - ioapic::REDIR_BASE;
            if (entry_off % 2 == 0) {
                // Low dword: mask writable bits (RO: delivery_status[12], remote_irr[14])
                s->regs[idx] = (val & ~ioapic::ENTRY_RO_MASK) | (s->regs[idx] & ioapic::ENTRY_RO_MASK);
            } else {
                // High dword: destination[31:24], rest reserved.
                s->regs[idx] = val & ioapic::ENTRY_DEST_MASK;
            }
            return;
        }
        s->regs[idx] = val;
    }
}

} // namespace xbox
