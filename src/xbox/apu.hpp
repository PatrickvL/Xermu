#pragma once
// MCPX APU register stubs — Voice/Global/Encode Processors.
//
// Flat register file indexed by MMIO offset / 4, matching the PGRAPH
// pattern.  Named constants in the papu:: namespace mirror NV_PAPU_*
// register offsets from the hardware documentation.
#include "address_map.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

// ========================== NV_PAPU Register Offsets =======================
// Offsets are relative to APU_BASE (0xFE800000).

namespace papu {

// Front-end (FE) — command processor
static constexpr uint32_t ISTS        = 0x1000;  // interrupt status (W1C)
static constexpr uint32_t IEN         = 0x1004;  // interrupt enable
static constexpr uint32_t FECTL       = 0x1100;  // FE control (method mode)
static constexpr uint32_t FECV        = 0x1110;  // FE current voice
static constexpr uint32_t FEAV        = 0x1118;  // FE active voice
static constexpr uint32_t FESTATE     = 0x1108;  // FE state
static constexpr uint32_t FESTATUS    = 0x110C;  // FE status
static constexpr uint32_t FETFORCE0   = 0x1500;  // FE test force 0
static constexpr uint32_t FETFORCE1   = 0x1504;  // FE test force 1 (SE2FE_IDLE_VOICE)

// Setup engine (SE) — voice/scatter-gather configuration
static constexpr uint32_t SECTL       = 0x2000;  // SE control (XCNTMODE)
static constexpr uint32_t XGSCNT      = 0x200C;  // global sample counter
static constexpr uint32_t VPVADDR     = 0x202C;  // VP voice list base address
static constexpr uint32_t VPSGEADDR   = 0x2030;  // VP scatter-gather base
static constexpr uint32_t VPSSLADDR   = 0x2034;  // VP SSL (sample start list)
static constexpr uint32_t GPSADDR     = 0x2040;  // GP scratch address
static constexpr uint32_t GPFADDR     = 0x2044;  // GP frame address
static constexpr uint32_t EPSADDR     = 0x2048;  // EP scratch address
static constexpr uint32_t EPFADDR     = 0x204C;  // EP frame address

// Voice list descriptors (2D / 3D / multipass)
static constexpr uint32_t TVL2D       = 0x2054;  // top voice list 2D
static constexpr uint32_t CVL2D       = 0x2058;  // current voice list 2D
static constexpr uint32_t NVL2D       = 0x205C;  // num voices 2D
static constexpr uint32_t TVL3D       = 0x2060;  // top voice list 3D
static constexpr uint32_t CVL3D       = 0x2064;  // current voice list 3D
static constexpr uint32_t NVL3D       = 0x2068;  // num voices 3D
static constexpr uint32_t TVLMP       = 0x206C;  // top voice list multipass
static constexpr uint32_t CVLMP       = 0x2070;  // current voice list multipass
static constexpr uint32_t NVLMP       = 0x2074;  // num voices multipass

// SGE limits
static constexpr uint32_t GPSMAXSGE   = 0x20D4;  // GP scratch max SGE
static constexpr uint32_t GPFMAXSGE   = 0x20D8;  // GP frame max SGE
static constexpr uint32_t EPSMAXSGE   = 0x20DC;  // EP scratch max SGE
static constexpr uint32_t EPFMAXSGE   = 0x20E0;  // EP frame max SGE

// Global Processor (GP) — DSP
static constexpr uint32_t GPRST       = 0x3000;  // GP reset
static constexpr uint32_t GPISTS      = 0x3004;  // GP interrupt status (W1C)
static constexpr uint32_t GPOFBASE0   = 0x3024;  // GP output FIFO base ch0
static constexpr uint32_t GPOFEND0    = 0x3028;  // GP output FIFO end  ch0
static constexpr uint32_t GPOFCUR0    = 0x302C;  // GP output FIFO cur  ch0
static constexpr uint32_t GPOFBASE1   = 0x3034;  // ch1
static constexpr uint32_t GPOFEND1    = 0x3038;
static constexpr uint32_t GPOFCUR1    = 0x303C;
static constexpr uint32_t GPOFBASE2   = 0x3044;  // ch2
static constexpr uint32_t GPOFEND2    = 0x3048;
static constexpr uint32_t GPOFCUR2    = 0x304C;
static constexpr uint32_t GPOFBASE3   = 0x3054;  // ch3
static constexpr uint32_t GPOFEND3    = 0x3058;
static constexpr uint32_t GPOFCUR3    = 0x305C;
static constexpr uint32_t GPIFBASE0   = 0x3064;  // GP input FIFO base ch0
static constexpr uint32_t GPIFEND0    = 0x3068;
static constexpr uint32_t GPIFCUR0    = 0x306C;
static constexpr uint32_t GPIFBASE1   = 0x3074;  // ch1
static constexpr uint32_t GPIFEND1    = 0x3078;
static constexpr uint32_t GPIFCUR1    = 0x307C;

// Encode Processor (EP) — DSP
static constexpr uint32_t EPRST       = 0x4000;  // EP reset
static constexpr uint32_t EPISTS      = 0x4004;  // EP interrupt status (W1C)
static constexpr uint32_t EPOFBASE0   = 0x4024;  // EP output FIFO base
static constexpr uint32_t EPOFEND0    = 0x4028;
static constexpr uint32_t EPOFCUR0    = 0x402C;
static constexpr uint32_t EPIFBASE0   = 0x4064;  // EP input FIFO base
static constexpr uint32_t EPIFEND0    = 0x4068;
static constexpr uint32_t EPIFCUR0    = 0x406C;

} // namespace papu

// =========================== APU State =====================================

struct ApuState {
    // Flat register file covering MMIO offsets 0x0000..0x4FFF.
    // Indexed by offset / 4.  Most registers are plain read/write;
    // W1C and FETFORCE1 idle-bit are handled as special cases.
    static constexpr uint32_t REG_SPACE = 0x5000;            // 20 KB
    static constexpr uint32_t REG_COUNT = REG_SPACE / 4;     // 5120 slots
    uint32_t regs[REG_COUNT] = {};

    // Voice Processor (VP) scratch state:
    // The VP can address up to 256 voice slots. Each voice is 128 bytes
    // (32 dwords) in the NV_PAVS voice memory region.
    // For now we just provide a flat R/W array for software to configure.
    static constexpr uint32_t MAX_VOICES       = 256;
    static constexpr uint32_t VOICE_SIZE_DWORDS = 32;  // 128 bytes
    uint32_t voices[MAX_VOICES * VOICE_SIZE_DWORDS] = {};

    uint32_t& reg(uint32_t off)       { return regs[off / 4]; }
    uint32_t  reg(uint32_t off) const { return regs[off / 4]; }
};

// ========================== NV_PAVS Voice Offsets ==========================
// Voice memory base is at APU_BASE + 0x20000.
// Each voice is 128 bytes (32 dwords), 256 voices total.
// Offsets within each voice slot (per NV docs):

namespace pavs {
static constexpr uint32_t VP_BASE       = 0x20000;
static constexpr uint32_t VP_SIZE       = 0x8000;  // 256 * 128 = 32KB

// Per-voice register offsets (within 128-byte slot)
static constexpr uint32_t V_CFG         = 0x00;  // configuration (format, state)
static constexpr uint32_t V_CTL         = 0x04;  // control (pitch, etc.)
static constexpr uint32_t V_TAR_PITCH   = 0x08;  // target pitch
static constexpr uint32_t V_CUR_VOL     = 0x10;  // current volume
static constexpr uint32_t V_TAR_VOL     = 0x14;  // target volume
static constexpr uint32_t V_CUR_POS     = 0x30;  // current position (fractional)
static constexpr uint32_t V_CUR_POS_F   = 0x34;  // position fraction
static constexpr uint32_t V_EAR         = 0x40;  // envelope attack rate
static constexpr uint32_t V_EDR         = 0x44;  // envelope decay rate
static constexpr uint32_t V_ESR         = 0x48;  // envelope sustain rate
static constexpr uint32_t V_ERR         = 0x4C;  // envelope release rate
} // namespace pavs

// ========================== MMIO Handlers ==================================

static uint32_t apu_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;

    // VP voice memory region
    if (off >= pavs::VP_BASE && off < pavs::VP_BASE + pavs::VP_SIZE) {
        uint32_t voff = off - pavs::VP_BASE;
        return apu->voices[voff / 4];
    }

    if (off / 4 >= ApuState::REG_COUNT) return 0;

    uint32_t val = apu->reg(off);

    // FETFORCE1: always report SE2FE_IDLE_VOICE (bit 7) — no actual voice
    // processing, so the voice processor is always idle.
    if (off == papu::FETFORCE1) val |= (1u << 7);

    return val;
}

static void apu_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;

    // VP voice memory region
    if (off >= pavs::VP_BASE && off < pavs::VP_BASE + pavs::VP_SIZE) {
        uint32_t voff = off - pavs::VP_BASE;
        apu->voices[voff / 4] = val;
        return;
    }

    if (off / 4 >= ApuState::REG_COUNT) return;

    switch (off) {
    // Write-1-to-clear interrupt status registers
    case papu::ISTS:
    case papu::GPISTS:
    case papu::EPISTS:
        apu->reg(off) &= ~val;
        return;
    default:
        apu->reg(off) = val;
        return;
    }
}

} // namespace xbox
