#pragma once
// MCPX APU register stubs — Voice/Global/Encode Processors.
#include "address_map.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

struct ApuState {
    uint32_t ists       = 0;
    uint32_t ien        = 0;
    uint32_t fectl      = 0;
    uint32_t fecv       = 0;
    uint32_t festate    = 0;
    uint32_t festatus   = 0;
    uint32_t sectl      = 0;
    uint32_t sestatus   = 0;
    uint32_t gprst      = 0;
    uint32_t gpists     = 0;
    uint32_t gpsaddr    = 0;
    uint32_t eprst      = 0;
    uint32_t epists     = 0;
    uint32_t epsaddr    = 0;
};

static uint32_t apu_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;
    if (off == 0x1000) return apu->ists;
    if (off == 0x1004) return apu->ien;
    if (off == 0x1100) return apu->fectl;
    if (off == 0x1104) return apu->fecv;
    if (off == 0x1108) return apu->festate;
    if (off == 0x110C) return apu->festatus;
    if (off == 0x2000) return apu->sectl;
    if (off == 0x200C) return apu->sestatus;
    if (off == 0x3000) return apu->gprst;
    if (off == 0x3004) return apu->gpists;
    if (off == 0x3008) return apu->gpsaddr;
    if (off == 0x4000) return apu->eprst;
    if (off == 0x4004) return apu->epists;
    if (off == 0x4008) return apu->epsaddr;
    return 0;
}

static void apu_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;
    if (off == 0x1000) { apu->ists &= ~val; return; }
    if (off == 0x1004) { apu->ien = val; return; }
    if (off == 0x1100) { apu->fectl = val; return; }
    if (off == 0x1108) { apu->festate = val; return; }
    if (off == 0x2000) { apu->sectl = val; return; }
    if (off == 0x3000) { apu->gprst = val; return; }
    if (off == 0x3008) { apu->gpsaddr = val; return; }
    if (off == 0x4000) { apu->eprst = val; return; }
    if (off == 0x4008) { apu->epsaddr = val; return; }
}

} // namespace xbox
