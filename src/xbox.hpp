#pragma once
// ---------------------------------------------------------------------------
// xbox.hpp — Xbox physical address map + device stubs.
//
// Sets up the MmioMap and I/O port handlers for the original Xbox hardware:
//
//   Guest PA          Size       Device
//   ─────────────────────────────────────────────────
//   0x0000_0000       64 MB      Main RAM
//   0x0C00_0000      128 MB      RAM mirror (NV2A tiling alias)
//   0xF000_0000        1 MB      Flash ROM (MCPX + BIOS)
//   0xFD00_0000       16 MB      NV2A GPU registers (MMIO)
//   0xFE80_0000        4 MB      APU / AC97 (MMIO)
//   0xFEC0_0000        4 KB      I/O APIC (MMIO)
//   0xFF00_0000        1 MB      BIOS shadow
//
//   I/O ports:
//   0x0020–0x0021     PIC (master 8259A)
//   0x00A0–0x00A1     PIC (slave 8259A)
//   0x0040–0x0043     PIT (8254 timer)
//   0x0061            System control port B
//   0x0CF8            PCI config address
//   0x0CFC–0x0CFF     PCI config data
//   0xC000–0xC00F     SMBus (MCPX)
//   0x01F0–0x01F7     IDE primary (HDD)
//   0x0170–0x0177     IDE secondary (DVD)
//   0x03F6            IDE primary control
//   0x0376            IDE secondary control
//   0xE9              Debug console (bochs-style)
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include <cstdio>
#include <cstring>

// ============================= Address Map =================================

namespace xbox {

static constexpr uint32_t RAM_SIZE_RETAIL   = 64u * 1024u * 1024u;   // 64 MB
static constexpr uint32_t RAM_SIZE_DEVKIT   = 128u * 1024u * 1024u;  // 128 MB

static constexpr uint32_t RAM_MIRROR_BASE   = 0x0C000000u;
static constexpr uint32_t RAM_MIRROR_SIZE   = 0x08000000u;  // 128 MB

static constexpr uint32_t FLASH_BASE        = 0xF0000000u;
static constexpr uint32_t FLASH_SIZE        = 0x00100000u;  // 1 MB

static constexpr uint32_t NV2A_BASE         = 0xFD000000u;
static constexpr uint32_t NV2A_SIZE         = 0x01000000u;  // 16 MB

static constexpr uint32_t APU_BASE          = 0xFE800000u;
static constexpr uint32_t APU_SIZE          = 0x00400000u;  // 4 MB

static constexpr uint32_t IOAPIC_BASE       = 0xFEC00000u;
static constexpr uint32_t IOAPIC_SIZE       = 0x00001000u;  // 4 KB

static constexpr uint32_t BIOS_BASE         = 0xFF000000u;
static constexpr uint32_t BIOS_SIZE         = 0x00100000u;  // 1 MB

// ============================= NV2A GPU ====================================

struct Nv2aState {
    uint32_t pmc_boot_0  = 0x02A000A1;  // NV2A chip ID (NV20 derivative)
    uint32_t pmc_enable  = 0;           // PMC_ENABLE
    uint32_t pmc_intr_en = 0;           // PMC_INTR_EN_0 (master IRQ enable)
    uint32_t pfb_cfg0    = 0x03070103;  // 64 MB RAM
    uint32_t pbus_0      = 0;
    uint32_t ptimer_num  = 1;
    uint32_t ptimer_den  = 1;
    uint32_t pcrtc_start = 0;
    uint32_t pcrtc_intr  = 0;           // PCRTC_INTR_0 (bit 0 = vblank pending)
    uint32_t pcrtc_intr_en = 0;         // PCRTC_INTR_EN_0 (bit 0 = vblank enable)
    uint32_t pvideo_intr = 0;

    // PFIFO registers
    uint32_t pfifo_intr      = 0;       // PFIFO_INTR_0
    uint32_t pfifo_intr_en   = 0;       // PFIFO_INTR_EN_0
    uint32_t pfifo_caches    = 0;       // PFIFO_CACHES (bit 0 = reassign enable)
    uint32_t pfifo_mode      = 0;       // PFIFO_MODE (per-channel DMA/PIO)
    uint32_t pfifo_cache1_push0 = 0;    // CACHE1_PUSH0 (bit 0 = access enable)
    uint32_t pfifo_cache1_pull0 = 0;    // CACHE1_PULL0 (bit 0 = access enable)
    uint32_t pfifo_cache1_push1 = 0;    // CACHE1_PUSH1 (channel id in bits 4:0)
    uint32_t pfifo_cache1_status = 0x10; // CACHE1_STATUS (bit 4 = empty)
    uint32_t pfifo_cache1_dma_push = 0;  // CACHE1_DMA_PUSH (bit 0 = access enable)
    uint32_t pfifo_cache1_dma_put = 0;
    uint32_t pfifo_cache1_dma_get = 0;
    uint32_t pfifo_runout_status = 0x10; // RUNOUT_STATUS (bit 4 = empty)

    // PGRAPH registers
    uint32_t pgraph_intr     = 0;       // PGRAPH_INTR
    uint32_t pgraph_intr_en  = 0;       // PGRAPH_NSOURCE / INTR_EN
    uint32_t pgraph_fifo     = 0;       // PGRAPH_FIFO (bit 0 = access enable)
    uint32_t pgraph_channel_ctx_status = 0; // PGRAPH_CTX_CONTROL (bit 24 = channel valid)

    // PRAMDAC registers — PLL / video clock configuration
    // Default PLL values: NV_CLK = 233 MHz, MEM_CLK = 200 MHz, VIDEO_CLK = 25.175 MHz
    uint32_t pramdac_nvpll   = 0x00011C01;  // NVPLL_COEFF (N=0x1C, M=1, P=0 → ~233 MHz)
    uint32_t pramdac_mpll    = 0x00011801;  // MPLL_COEFF  (N=0x18, M=1, P=0 → ~200 MHz)
    uint32_t pramdac_vpll    = 0x00031801;  // VPLL_COEFF  (N=0x18, M=1, P=3 → ~25 MHz)
    uint32_t pramdac_pll_test = 0;          // PLL_TEST_COUNTER

    // PTIMER: 56-bit freerunning nanosecond counter.
    uint64_t ptimer_ns = 0;
    static constexpr uint64_t NS_PER_TICK = 100;  // ~10 MHz effective

    // PCRTC vblank: fires every VBLANK_PERIOD ticks (~60 Hz at ~16667 ticks).
    uint32_t vblank_counter = 0;
    static constexpr uint32_t VBLANK_PERIOD = 16667;
    bool     vblank_irq_pending = false;  // set by tick, cleared by hw_tick_callback

    void tick_timer() {
        ptimer_ns += NS_PER_TICK;

        // Vblank generation.
        if (++vblank_counter >= VBLANK_PERIOD) {
            vblank_counter = 0;
            if (pcrtc_intr_en & 1)
                pcrtc_intr |= 1;   // set vblank pending
            // Signal to the combined tick callback to raise IRQ.
            if ((pmc_intr_en & 0x01000000) && (pcrtc_intr & 1))
                vblank_irq_pending = true;
        }
    }

    // PFIFO DMA pusher: advance GET toward PUT, consuming commands.
    // Each tick consumes up to MAX_DWORDS_PER_TICK dwords from the push buffer.
    // This is a stub — commands are discarded, not interpreted.
    static constexpr uint32_t MAX_DWORDS_PER_TICK = 128;

    void tick_fifo(const uint8_t* /*ram*/, uint32_t /*ram_size_bytes*/) {
        // DMA pusher must be enabled.
        if (!(pfifo_cache1_dma_push & 1)) return;
        // PUSH0 access must be enabled.
        if (!(pfifo_cache1_push0 & 1)) return;
        // Nothing to do if GET == PUT.
        if (pfifo_cache1_dma_get == pfifo_cache1_dma_put) {
            pfifo_cache1_status = 0x10;  // empty
            return;
        }
        // Consume up to MAX_DWORDS_PER_TICK dwords.
        pfifo_cache1_status = 0;  // not empty
        uint32_t get = pfifo_cache1_dma_get;
        uint32_t put = pfifo_cache1_dma_put;
        uint32_t consumed = 0;
        while (get != put && consumed < MAX_DWORDS_PER_TICK) {
            get += 4;  // skip one dword (4 bytes)
            consumed++;
        }
        pfifo_cache1_dma_get = get;
        if (get == put)
            pfifo_cache1_status = 0x10;  // empty
    }

    // PMC_INTR_0: read-only summary of all NV2A interrupt sources.
    // Bit 24 = PCRTC interrupt pending.
    uint32_t pmc_intr_0() const {
        uint32_t val = 0;
        if (pcrtc_intr & 1) val |= 0x01000000;  // bit 24: PCRTC
        return val;
    }
};

static uint32_t nv2a_read(uint32_t pa, unsigned size, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    // PMC (0x000xxx) — master control
    if (off == 0x000000) return nv->pmc_boot_0;    // PMC_BOOT_0
    if (off == 0x000100) return nv->pmc_intr_0();  // PMC_INTR_0 (summary)
    if (off == 0x000140) return nv->pmc_intr_en;   // PMC_INTR_EN_0
    if (off == 0x000200) return nv->pmc_enable;    // PMC_ENABLE

    // PBUS (0x001xxx) — bus control
    if (off == 0x001200) return nv->pbus_0;         // PBUS_PCI_NV_0
    if (off == 0x001214) return 0;                  // PBUS interrupt status

    // PFIFO (0x002xxx) — FIFO engine
    if (off == 0x002100) return nv->pfifo_intr;           // PFIFO_INTR_0
    if (off == 0x002140) return nv->pfifo_intr_en;        // PFIFO_INTR_EN_0
    if (off == 0x002500) return nv->pfifo_caches;         // PFIFO_CACHES
    if (off == 0x002504) return nv->pfifo_mode;           // PFIFO_MODE
    if (off == 0x003200) return nv->pfifo_cache1_push0;   // CACHE1_PUSH0
    if (off == 0x003210) return nv->pfifo_cache1_push1;   // CACHE1_PUSH1
    if (off == 0x003220) return nv->pfifo_cache1_dma_push;// CACHE1_DMA_PUSH
    if (off == 0x003240) return nv->pfifo_cache1_dma_put; // CACHE1_DMA_PUT
    if (off == 0x003244) return nv->pfifo_cache1_dma_get; // CACHE1_DMA_GET
    if (off == 0x003250) return nv->pfifo_cache1_pull0;   // CACHE1_PULL0
    if (off == 0x003214) return nv->pfifo_cache1_status;  // CACHE1_STATUS
    if (off == 0x002400) return nv->pfifo_runout_status;  // PFIFO_RUNOUT_STATUS
    if (off == 0x003228) {                                 // CACHE1_DMA_STATE
        // Bit 0 = busy (1 if GET != PUT and pusher enabled).
        bool busy = (nv->pfifo_cache1_dma_push & 1) &&
                    (nv->pfifo_cache1_dma_get != nv->pfifo_cache1_dma_put);
        return busy ? 1u : 0u;
    }

    // PTIMER (0x009xxx) — timer
    if (off == 0x009200) return nv->ptimer_num;
    if (off == 0x009210) return nv->ptimer_den;
    if (off == 0x009400) return (uint32_t)(nv->ptimer_ns & 0xFFFFFFE0u); // TIME_0 (bits[31:5])
    if (off == 0x009410) return (uint32_t)(nv->ptimer_ns >> 32);         // TIME_1
    if (off == 0x009100) return 0;                  // PTIMER_INTR_0

    // PFB (0x100xxx) — framebuffer control
    if (off == 0x100200) return nv->pfb_cfg0;       // PFB_CFG0
    if (off == 0x100204) return 0;                  // PFB_CFG1

    // PCRTC (0x600xxx) — CRTC
    if (off == 0x600100) return nv->pcrtc_intr;     // PCRTC_INTR_0
    if (off == 0x600140) return nv->pcrtc_intr_en;  // PCRTC_INTR_EN_0
    if (off == 0x600800) return nv->pcrtc_start;    // PCRTC_START

    // PVIDEO (0x008xxx) — video overlay
    if (off == 0x008100) return nv->pvideo_intr;

    // PGRAPH (0x400xxx) — 3D engine
    if (off == 0x400100) return nv->pgraph_intr;            // PGRAPH_INTR
    if (off == 0x400140) return nv->pgraph_intr_en;         // PGRAPH_INTR_EN
    if (off == 0x400720) return nv->pgraph_fifo;            // PGRAPH_FIFO
    if (off == 0x400170) return nv->pgraph_channel_ctx_status; // PGRAPH_CTX_CONTROL

    // PRAMDAC (0x680xxx) — PLL / video clock
    if (off == 0x680500) return nv->pramdac_nvpll;          // NVPLL_COEFF
    if (off == 0x680504) return nv->pramdac_mpll;           // MPLL_COEFF
    if (off == 0x680508) return nv->pramdac_vpll;           // VPLL_COEFF
    if (off == 0x680514) return nv->pramdac_pll_test;       // PLL_TEST_COUNTER
    // PRAMDAC general control: bit 0 = VGA blanking, bit 4 = DAC width (8bpp)
    if (off == 0x680600) return 0x00000101;                 // PRAMDAC_GENERAL_CONTROL
    if (off == 0x6808C0) return 0;                          // PRAMDAC_FP_DEBUG_0

    // Default: bus float
    return 0;
}

static void nv2a_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    if (off == 0x000140) { nv->pmc_intr_en = val; return; }    // PMC_INTR_EN_0
    if (off == 0x000200) { nv->pmc_enable = val; return; }
    if (off == 0x001200) { nv->pbus_0 = val;     return; }
    if (off == 0x009200) { nv->ptimer_num = val;  return; }
    if (off == 0x009210) { nv->ptimer_den = val;  return; }
    if (off == 0x600100) { nv->pcrtc_intr &= ~val; return; }   // PCRTC_INTR_0 W1C
    if (off == 0x600140) { nv->pcrtc_intr_en = val; return; }  // PCRTC_INTR_EN_0
    if (off == 0x600800) { nv->pcrtc_start = val; return; }
    if (off == 0x008100) { nv->pvideo_intr &= ~val; return; }  // W1C

    // PFIFO writes
    if (off == 0x002100) { nv->pfifo_intr &= ~val; return; }      // PFIFO_INTR_0 W1C
    if (off == 0x002140) { nv->pfifo_intr_en = val; return; }     // PFIFO_INTR_EN_0
    if (off == 0x002500) { nv->pfifo_caches = val; return; }      // PFIFO_CACHES
    if (off == 0x002504) { nv->pfifo_mode = val; return; }        // PFIFO_MODE
    if (off == 0x003200) { nv->pfifo_cache1_push0 = val; return; }// CACHE1_PUSH0
    if (off == 0x003210) { nv->pfifo_cache1_push1 = val; return; }// CACHE1_PUSH1
    if (off == 0x003220) { nv->pfifo_cache1_dma_push = val; return; } // CACHE1_DMA_PUSH
    if (off == 0x003240) { nv->pfifo_cache1_dma_put = val; return; }  // CACHE1_DMA_PUT
    if (off == 0x003244) { nv->pfifo_cache1_dma_get = val; return; }  // CACHE1_DMA_GET
    if (off == 0x003250) { nv->pfifo_cache1_pull0 = val; return; }// CACHE1_PULL0

    // PGRAPH writes
    if (off == 0x400100) { nv->pgraph_intr &= ~val; return; }    // PGRAPH_INTR W1C
    if (off == 0x400140) { nv->pgraph_intr_en = val; return; }   // PGRAPH_INTR_EN
    if (off == 0x400720) { nv->pgraph_fifo = val; return; }      // PGRAPH_FIFO
    if (off == 0x400170) { nv->pgraph_channel_ctx_status = val; return; }

    // PRAMDAC writes
    if (off == 0x680500) { nv->pramdac_nvpll = val; return; }    // NVPLL_COEFF
    if (off == 0x680504) { nv->pramdac_mpll = val; return; }     // MPLL_COEFF
    if (off == 0x680508) { nv->pramdac_vpll = val; return; }     // VPLL_COEFF
    if (off == 0x680514) { nv->pramdac_pll_test = val; return; } // PLL_TEST_COUNTER

    // Silently drop unhandled writes.
}

// ============================= APU (MCPX Audio) ============================
//
// Xbox MCPX APU @ 0xFE800000 (4 MB).
// Register blocks:
//   0x0000 – 0x1FFF   General APU control / front-end / setup engine
//   0x20000 – 0x2FFFF  VP (Voice Processor) — 256 voice descriptors
//   0x30000 – 0x3FFFF  GP (Global Processor) — DSP scratch + PMEM
//   0x40000 – 0x4FFFF  EP (Encode Processor) — DSP scratch + PMEM
//
// Key register offsets within APU_BASE:
//   NV_PAPU_ISTS     0x1000  Interrupt status
//   NV_PAPU_IEN      0x1004  Interrupt enable mask
//   NV_PAPU_FECTL    0x1100  Front-end control
//   NV_PAPU_FECV     0x1104  Front-end current voice
//   NV_PAPU_FESTATE  0x1108  Front-end state (0 = idle)
//   NV_PAPU_FESTATUS 0x110C  Front-end status (bit 0 = busy)
//   NV_PAPU_SECTL    0x2000  Setup engine control
//   NV_PAPU_SESTATUS 0x200C  Setup engine status (0 = idle)
//   NV_PAPU_GPRST    0x3000  GP reset
//   NV_PAPU_GPISTS   0x3004  GP interrupt status
//   NV_PAPU_GPSADDR  0x3008  GP scratch base addr
//   NV_PAPU_EPRST    0x4000  EP reset
//   NV_PAPU_EPISTS   0x4004  EP interrupt status
//   NV_PAPU_EPSADDR  0x4008  EP scratch base addr
// ---------------------------------------------------------------------------

struct ApuState {
    // General APU control
    uint32_t ists       = 0;            // NV_PAPU_ISTS  — interrupt status
    uint32_t ien        = 0;            // NV_PAPU_IEN   — interrupt enable
    uint32_t fectl      = 0;            // NV_PAPU_FECTL — front-end control
    uint32_t fecv       = 0;            // NV_PAPU_FECV  — front-end current voice
    uint32_t festate    = 0;            // NV_PAPU_FESTATE — 0 = idle
    uint32_t festatus   = 0;            // NV_PAPU_FESTATUS — bit 0 = busy

    // Setup engine
    uint32_t sectl      = 0;            // NV_PAPU_SECTL
    uint32_t sestatus   = 0;            // NV_PAPU_SESTATUS — 0 = idle

    // Global Processor
    uint32_t gprst      = 0;            // NV_PAPU_GPRST
    uint32_t gpists     = 0;            // NV_PAPU_GPISTS
    uint32_t gpsaddr    = 0;            // NV_PAPU_GPSADDR  — GP scratch base address

    // Encode Processor
    uint32_t eprst      = 0;            // NV_PAPU_EPRST
    uint32_t epists     = 0;            // NV_PAPU_EPISTS
    uint32_t epsaddr    = 0;            // NV_PAPU_EPSADDR  — EP scratch base address
};

static uint32_t apu_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;

    // General APU registers (0x1000 – 0x1FFF)
    if (off == 0x1000) return apu->ists;
    if (off == 0x1004) return apu->ien;
    if (off == 0x1100) return apu->fectl;
    if (off == 0x1104) return apu->fecv;
    if (off == 0x1108) return apu->festate;    // 0 = idle
    if (off == 0x110C) return apu->festatus;   // 0 = not busy

    // Setup engine
    if (off == 0x2000) return apu->sectl;
    if (off == 0x200C) return apu->sestatus;   // 0 = idle

    // Global Processor
    if (off == 0x3000) return apu->gprst;
    if (off == 0x3004) return apu->gpists;
    if (off == 0x3008) return apu->gpsaddr;

    // Encode Processor
    if (off == 0x4000) return apu->eprst;
    if (off == 0x4004) return apu->epists;
    if (off == 0x4008) return apu->epsaddr;

    return 0;  // default: bus float
}

static void apu_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* apu = static_cast<ApuState*>(user);
    uint32_t off = pa - APU_BASE;

    // General APU registers
    if (off == 0x1000) { apu->ists &= ~val; return; }  // W1C
    if (off == 0x1004) { apu->ien = val; return; }
    if (off == 0x1100) { apu->fectl = val; return; }
    if (off == 0x1108) { apu->festate = val; return; }

    // Setup engine
    if (off == 0x2000) { apu->sectl = val; return; }

    // Global Processor
    if (off == 0x3000) { apu->gprst = val; return; }
    if (off == 0x3008) { apu->gpsaddr = val; return; }

    // Encode Processor
    if (off == 0x4000) { apu->eprst = val; return; }
    if (off == 0x4008) { apu->epsaddr = val; return; }

    // Silently drop unhandled writes.
}

// ============================= IDE (ATA) ===================================
//
// Xbox IDE controller (PCI Dev 9): dual-channel ATA.
//   Primary   (0x1F0–0x1F7, ctrl 0x3F6): HDD (master)
//   Secondary (0x170–0x177, ctrl 0x376): DVD (master)
//
// ATA task-file registers (offset from channel base):
//   +0  Data (16-bit PIO read/write)
//   +1  Error (read) / Features (write)
//   +2  Sector Count
//   +3  LBA Low  (Sector Number)
//   +4  LBA Mid  (Cylinder Low)
//   +5  LBA High (Cylinder High)
//   +6  Device/Head
//   +7  Status (read) / Command (write)
//   Control register (0x3F6/0x376):
//     Bit 1 = nIEN (disable interrupts), Bit 2 = SRST (software reset)
//     Read returns Alternate Status.
// ---------------------------------------------------------------------------

struct IdeChannel {
    // Task-file registers
    uint8_t  error      = 0x01;     // Error register (read); 0x01 = no error
    uint8_t  features   = 0;        // Features (write)
    uint8_t  sect_count = 0x01;     // Sector count
    uint8_t  lba_low    = 0x01;     // LBA[7:0]  / sector number
    uint8_t  lba_mid    = 0x00;     // LBA[15:8] / cylinder low
    uint8_t  lba_high   = 0x00;     // LBA[23:16]/ cylinder high
    uint8_t  device     = 0x00;     // Device/head
    uint8_t  status     = 0x50;     // Status: DRDY + DSC (device ready, seek complete)
    uint8_t  control    = 0x00;     // Device control (nIEN, SRST)
    bool     present    = false;    // true if device exists on this channel

    // ATA IDENTIFY data (512 bytes) — filled once for present devices.
    uint8_t  identify[512] = {};
};

struct IdeState {
    IdeChannel primary;     // HDD
    IdeChannel secondary;   // DVD

    IdeState() {
        // Primary master = HDD (present).
        primary.present   = true;
        primary.status    = 0x50;   // DRDY | DSC
        init_hdd_identify(primary);

        // Secondary master = DVD (present).
        secondary.present = true;
        secondary.status  = 0x50;   // DRDY | DSC
        init_dvd_identify(secondary);
    }

    static void init_hdd_identify(IdeChannel& ch) {
        memset(ch.identify, 0, 512);
        auto* w = reinterpret_cast<uint16_t*>(ch.identify);
        w[0]  = 0x0040;        // General config: fixed drive
        w[1]  = 16383;         // Logical cylinders
        w[3]  = 16;            // Logical heads
        w[6]  = 63;            // Sectors per track
        // Model string (words 27-46): "XBOX HDD            " (padded to 40 chars, byte-swapped)
        set_ata_string(w + 27, "XBOX HDD", 40);
        // Serial number (words 10-19)
        set_ata_string(w + 10, "0123456789", 20);
        // Firmware revision (words 23-26)
        set_ata_string(w + 23, "1.00", 8);
        w[47] = 0x8010;        // Max sectors per multi-R/W (16)
        w[49] = 0x0200;        // Capabilities: LBA supported
        w[53] = 0x0006;        // Words 64-70, 88 valid
        w[60] = 0x5C10;        // Total sectors (LBA28) low  = ~8 GB
        w[61] = 0x0097;        // Total sectors (LBA28) high
        w[80] = 0x007E;        // Major version: ATA-1 through ATA-6
        w[83] = 0x4000;        // Command set: 48-bit LBA not supported (8 GB HDD)
        w[88] = 0x003F;        // Ultra DMA modes supported (0-5)
    }

    static void init_dvd_identify(IdeChannel& ch) {
        memset(ch.identify, 0, 512);
        auto* w = reinterpret_cast<uint16_t*>(ch.identify);
        w[0]  = 0x8580;        // General config: ATAPI, CD-ROM, removable
        set_ata_string(w + 27, "XBOX DVD", 40);
        set_ata_string(w + 10, "0000000001", 20);
        set_ata_string(w + 23, "1.00", 8);
        w[49] = 0x0200;        // LBA
        w[80] = 0x007E;        // ATA-6
    }

    // ATA strings are byte-swapped (each pair of bytes exchanged).
    static void set_ata_string(uint16_t* dst, const char* src, int byte_len) {
        char buf[64] = {};
        int slen = 0;
        while (src[slen] && slen < byte_len) { buf[slen] = src[slen]; slen++; }
        for (int i = slen; i < byte_len; i++) buf[i] = ' ';  // pad with spaces
        for (int i = 0; i < byte_len; i += 2)
            dst[i / 2] = (uint16_t)((uint8_t)buf[i] << 8 | (uint8_t)buf[i + 1]);
    }
};

static uint32_t ide_io_read(uint16_t port, unsigned size, void* user) {
    auto* ide = static_cast<IdeState*>(user);

    // Determine channel.
    IdeChannel* ch;
    int reg;
    if (port >= 0x1F0 && port <= 0x1F7) {
        ch = &ide->primary; reg = port - 0x1F0;
    } else if (port == 0x3F6) {
        ch = &ide->primary; reg = 8; // alternate status / control
    } else if (port >= 0x170 && port <= 0x177) {
        ch = &ide->secondary; reg = port - 0x170;
    } else if (port == 0x376) {
        ch = &ide->secondary; reg = 8;
    } else {
        return 0xFF;
    }

    if (!ch->present) return 0x00;  // No device: float low

    switch (reg) {
    case 0: return 0;                   // Data (PIO read — stub)
    case 1: return ch->error;           // Error
    case 2: return ch->sect_count;      // Sector count
    case 3: return ch->lba_low;         // LBA low
    case 4: return ch->lba_mid;         // LBA mid
    case 5: return ch->lba_high;        // LBA high
    case 6: return ch->device;          // Device/head
    case 7: return ch->status;          // Status
    case 8: return ch->status;          // Alternate status
    default: return 0xFF;
    }
}

static void ide_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* ide = static_cast<IdeState*>(user);

    IdeChannel* ch;
    int reg;
    if (port >= 0x1F0 && port <= 0x1F7) {
        ch = &ide->primary; reg = port - 0x1F0;
    } else if (port == 0x3F6) {
        ch = &ide->primary; reg = 8;
    } else if (port >= 0x170 && port <= 0x177) {
        ch = &ide->secondary; reg = port - 0x170;
    } else if (port == 0x376) {
        ch = &ide->secondary; reg = 8;
    } else {
        return;
    }

    if (!ch->present) return;

    uint8_t v = (uint8_t)val;

    switch (reg) {
    case 0: break;                          // Data (PIO write — stub)
    case 1: ch->features = v; break;        // Features
    case 2: ch->sect_count = v; break;      // Sector count
    case 3: ch->lba_low = v; break;         // LBA low
    case 4: ch->lba_mid = v; break;         // LBA mid
    case 5: ch->lba_high = v; break;        // LBA high
    case 6: ch->device = v; break;          // Device/head
    case 7:                                  // Command
        // Handle a few essential ATA commands.
        switch (v) {
        case 0xEC: // IDENTIFY DEVICE
            ch->status = 0x58; // DRDY | DSC | DRQ (data ready)
            ch->error  = 0;
            break;
        case 0xA1: // IDENTIFY PACKET DEVICE (ATAPI)
            ch->status = 0x58;
            ch->error  = 0;
            break;
        case 0xEF: // SET FEATURES
            ch->status = 0x50; // DRDY | DSC (success, no data)
            ch->error  = 0;
            break;
        case 0x91: // INITIALIZE DEVICE PARAMETERS
            ch->status = 0x50;
            ch->error  = 0;
            break;
        case 0xE7: // FLUSH CACHE
            ch->status = 0x50;
            ch->error  = 0;
            break;
        default:
            // Unknown command — set error (abort).
            ch->status = 0x51; // DRDY | ERR
            ch->error  = 0x04; // ABRT
            break;
        }
        break;
    case 8: // Device control
        ch->control = v;
        if (v & 0x04) { // SRST — software reset
            ch->status     = 0x50;
            ch->error      = 0x01;
            ch->sect_count = 0x01;
            ch->lba_low    = 0x01;
            ch->lba_mid    = 0x00;
            ch->lba_high   = 0x00;
            ch->device     = 0x00;
        }
        break;
    }
}

// ============================= I/O APIC ====================================

struct IoApicState {
    uint32_t index = 0;
    uint32_t regs[24 * 2 + 2] = {}; // ID, VER, ARB, + 24 redirection entries × 2

    IoApicState() {
        regs[0] = 0;           // IOAPIC_ID
        regs[1] = 0x00170011;  // IOAPIC_VER: 17h max redir, version 11h
    }
};

static uint32_t ioapic_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) return s->index;           // IOREGSEL
    if (off == 0x10 && s->index < 64) return s->regs[s->index]; // IOWIN
    return 0;
}

static void ioapic_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<IoApicState*>(user);
    uint32_t off = pa - IOAPIC_BASE;
    if (off == 0x00) { s->index = val & 0xFF; return; }
    if (off == 0x10 && s->index < 64) { s->regs[s->index] = val; return; }
}

// ============================= RAM Mirror ==================================
// NV2A tiling alias: reads/writes at [0x0C000000..0x13FFFFFF] map to main RAM
// with PA = (access_pa - 0x0C000000) % actual_ram_size.

static uint32_t ram_mirror_read(uint32_t pa, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    uint32_t val = 0;
    memcpy(&val, ram + off, size);
    return val;
}

static void ram_mirror_write(uint32_t pa, uint32_t val, unsigned size, void* user) {
    auto* ram = static_cast<uint8_t*>(user);
    uint32_t off = (pa - RAM_MIRROR_BASE) & (RAM_SIZE_RETAIL - 1);
    memcpy(ram + off, &val, size);
}

// ============================= Flash ROM ===================================
// 1 MB flash ROM. Reads return stored bytes or 0xFF (empty). Writes ignored.

struct FlashState {
    uint8_t data[FLASH_SIZE];
    FlashState() { memset(data, 0xFF, FLASH_SIZE); }
};

static uint32_t flash_read(uint32_t pa, unsigned size, void* user) {
    auto* f = static_cast<FlashState*>(user);
    // Both FLASH_BASE (0xF0000000) and BIOS_BASE (0xFF000000) map here.
    // Wrap into 1 MB range.
    uint32_t off = (pa - FLASH_BASE) & (FLASH_SIZE - 1);
    uint32_t val = 0;
    memcpy(&val, f->data + off, size);
    return val;
}

static void flash_write(uint32_t, uint32_t, unsigned, void*) {}

// ============================= PCI Config ==================================
// PCI configuration space via I/O ports 0xCF8 (address) / 0xCFC (data).
// The Xbox MCPX provides buses 0-1 with a handful of devices.

struct PciState {
    uint32_t config_address = 0; // written to port 0xCF8
    // Simple flat config space: [bus][dev][fn][256 bytes]
    // Only a few devices are populated.
    static constexpr int MAX_DEVS = 32;
    uint8_t config[2][MAX_DEVS][8][256] = {}; // [bus][dev][fn][offset]

    void set_id(int bus, int dev, int fn, uint16_t vendor, uint16_t device,
                uint8_t cls, uint8_t subcls, uint8_t progif, uint8_t rev) {
        uint8_t* c = config[bus][dev][fn];
        memcpy(c + 0x00, &vendor, 2);
        memcpy(c + 0x02, &device, 2);
        c[0x08] = rev;
        c[0x09] = progif;
        c[0x0A] = subcls;
        c[0x0B] = cls;
    }

    void init_xbox_devices() {
        // Bus 0, Dev 0: MCPX Host Bridge
        set_id(0, 0, 0, 0x10DE, 0x02A5, 0x06, 0x00, 0x00, 0xB1);
        // Bus 0, Dev 1, Fn 0: MCPX LPC (ISA Bridge)
        set_id(0, 1, 0, 0x10DE, 0x01B2, 0x06, 0x01, 0x00, 0xB1);
        // Bus 0, Dev 1, Fn 1: MCPX SMBus controller
        set_id(0, 1, 1, 0x10DE, 0x01B4, 0x0C, 0x05, 0x00, 0xB1);
        // Set SMBus I/O base (BAR) at offset 0x14 = 0xC000
        {
            uint16_t bar = 0xC001; // I/O space bit set
            memcpy(config[0][1][1] + 0x14, &bar, 2);
        }
        // Bus 0, Dev 2: NV2A (GPU)
        set_id(0, 2, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
        // Bus 0, Dev 3: MCPX Audio (APU)
        set_id(0, 3, 0, 0x10DE, 0x01B0, 0x04, 0x01, 0x00, 0xB1);
        // Bus 0, Dev 4: OHCI USB #0
        set_id(0, 4, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        // Bus 0, Dev 5: OHCI USB #1
        set_id(0, 5, 0, 0x10DE, 0x01C2, 0x0C, 0x03, 0x10, 0xB1);
        // Bus 0, Dev 9: IDE controller
        set_id(0, 9, 0, 0x10DE, 0x01BC, 0x01, 0x01, 0x8A, 0xB1);
        // Bus 0, Dev 30: AGP → PCI bridge
        set_id(0, 30, 0, 0x10DE, 0x01B7, 0x06, 0x04, 0x00, 0xB1);
        // Bus 1, Dev 0: NV2A GPU (on AGP bus)
        set_id(1, 0, 0, 0x10DE, 0x02A0, 0x03, 0x00, 0x00, 0xA1);
    }
};

static void pci_io_write_cf8(uint16_t /*port*/, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PciState*>(user)->config_address = val;
}
static uint32_t pci_io_read_cf8(uint16_t /*port*/, unsigned /*size*/, void* user) {
    return static_cast<PciState*>(user)->config_address;
}

static uint32_t pci_io_read_cfc(uint16_t port, unsigned size, void* user) {
    auto* pci = static_cast<PciState*>(user);
    uint32_t addr = pci->config_address;
    if (!(addr & 0x80000000u)) return 0xFFFFFFFFu; // enable bit not set
    int bus = (addr >> 16) & 0xFF;
    int dev = (addr >> 11) & 0x1F;
    int fn  = (addr >>  8) & 0x07;
    int off = (addr & 0xFC) + (port - 0xCFC);
    if (bus > 1 || dev >= PciState::MAX_DEVS || off + (int)size > 256)
        return 0xFFFFFFFFu;
    uint32_t val = 0;
    memcpy(&val, pci->config[bus][dev][fn] + off, size);
    return val;
}

static void pci_io_write_cfc(uint16_t port, uint32_t val, unsigned size, void* user) {
    auto* pci = static_cast<PciState*>(user);
    uint32_t addr = pci->config_address;
    if (!(addr & 0x80000000u)) return;
    int bus = (addr >> 16) & 0xFF;
    int dev = (addr >> 11) & 0x1F;
    int fn  = (addr >>  8) & 0x07;
    int off = (addr & 0xFC) + (port - 0xCFC);
    if (bus > 1 || dev >= PciState::MAX_DEVS || off + (int)size > 256) return;
    memcpy(pci->config[bus][dev][fn] + off, &val, size);
}

// ============================= SMBus =======================================
// Minimal SMBus controller stub at I/O ports 0xC000–0xC00F.
// The Xbox kernel probes SMBus to detect hardware (EEPROM at address 0x54,
// video encoder, temperature sensor, etc.).

struct SmbusState {
    uint8_t status  = 0;
    uint8_t control = 0;
    uint8_t address = 0;
    uint8_t command = 0;
    uint8_t data    = 0;
    // Xbox EEPROM (256 bytes) at SMBus address 0x54.
    // Layout: encrypted section (0x00-0x2F), factory section (0x30-0x4F),
    //         user section (0x50-0xFF).
    uint8_t eeprom[256] = {};

    SmbusState() {
        init_eeprom();
    }

    void init_eeprom() {
        memset(eeprom, 0, sizeof(eeprom));
        // --- Encrypted section (0x00-0x2F) ---
        // 0x00-0x13: HMAC hash (leave zero — not validated in HLE mode)
        // 0x14-0x1B: Confounder (8 bytes)
        // 0x1C-0x2B: HDD key (16 bytes, leave zero)
        // 0x2C-0x2F: Game region = 1 (NTSC-NA)
        eeprom[0x2C] = 0x01; // EEPROM_GAME_REGION: 1=NA, 2=Japan, 4=Europe

        // --- Factory section (0x30-0x4F) ---
        // 0x30-0x33: Checksum2 (leave zero — not validated in HLE mode)
        // 0x34-0x3F: Serial number (12 ASCII chars)
        const char* serial = "000000000000";
        memcpy(eeprom + 0x34, serial, 12);
        // 0x40-0x45: MAC address (6 bytes — use locally administered address)
        eeprom[0x40] = 0x00; eeprom[0x41] = 0x50; eeprom[0x42] = 0xF2;
        eeprom[0x43] = 0x00; eeprom[0x44] = 0x00; eeprom[0x45] = 0x01;
        // 0x46-0x47: Reserved
        // 0x48-0x4B: Online key (leave zero)
        // 0x4C-0x4F: Video standard: 0x00800100 = NTSC-M
        eeprom[0x4C] = 0x00; eeprom[0x4D] = 0x01;
        eeprom[0x4E] = 0x80; eeprom[0x4F] = 0x00;

        // --- User section (0x50-0xFF) ---
        // 0x50-0x53: Checksum3 (leave zero)
        // 0x54-0x57: Time zone bias (minutes from UTC, little-endian int32)
        //            0 = UTC
        // 0x58-0x97: Time zone standard name (64 bytes, leave empty)
        // 0x98-0xD7: Time zone daylight name (64 bytes, leave empty)
        // 0xD8-0xDB: Time zone standard date (leave zero)
        // 0xDC-0xDF: Time zone daylight date (leave zero)
        // 0xE0-0xE3: Time zone standard bias (0)
        // 0xE4-0xE7: Time zone daylight bias (0)
        // 0xE8-0xEB: Language: 1 = English
        eeprom[0xE8] = 0x01;
        // 0xEC-0xEF: Video flags: 0x00 (default widescreen off, letterbox off)
        // 0xF0-0xF3: Audio flags: 0x00 (stereo)
        // 0xF4-0xF7: Parental control (0 = off)
        // 0xF8-0xFB: Parental control password (0)
        // 0xFC-0xFF: DVD region: 1 = Region 1 (North America)
        eeprom[0xFC] = 0x01;
    }
};

static uint32_t smbus_io_read(uint16_t port, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00: return s->status;   // SMBUS_STATUS
    case 0x02: return s->control;  // SMBUS_CONTROL
    case 0x04: return s->address;  // SMBUS_ADDRESS
    case 0x06: return s->data;     // SMBUS_DATA
    case 0x08: return s->command;  // SMBUS_COMMAND
    default:   return 0;
    }
}

static void smbus_io_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto* s = static_cast<SmbusState*>(user);
    switch (port & 0xF) {
    case 0x00:
        s->status &= ~(uint8_t)val;  // W1C (write-1-to-clear)
        return;
    case 0x02: { // SMBUS_CONTROL — trigger a transaction
        s->control = (uint8_t)val;
        s->status |= 0x10;  // auto-complete: SMBUS_STATUS_DONE
        uint8_t dev_addr = s->address >> 1; // 7-bit device address
        bool is_read = (s->address & 1) != 0;

        if (dev_addr == 0x54) {
            // EEPROM (24C02)
            if (is_read && s->command < sizeof(s->eeprom))
                s->data = s->eeprom[s->command];
            else if (!is_read && s->command < sizeof(s->eeprom))
                s->eeprom[s->command] = s->data; // byte write
        } else if (dev_addr == 0x10) {
            // System Management Controller (PIC16LC / SMC)
            if (is_read) {
                switch (s->command) {
                case 0x01: s->data = 0xD0; break; // SMC_VERSION (v1.0 retail)
                case 0x03: s->data = 0x60; break; // TRAY_STATE (closed, no disc)
                case 0x09: s->data = 25;   break; // CPU_TEMP (25°C)
                case 0x0A: s->data = 35;   break; // MB_TEMP (35°C)
                case 0x0F: s->data = 0x05; break; // SMC_REVISION
                case 0x11: s->data = 0x40; break; // AVPACK (composite)
                default:   s->data = 0;    break;
                }
            }
            // Writes to SMC (LED control, fan speed, etc.) are silently accepted
        } else if (dev_addr == 0x45) {
            // Conexant CX25871 video encoder — stub: always succeed
            if (is_read) s->data = 0;
        } else if (dev_addr == 0x6A) {
            // Focus FS454 video encoder (1.4+ Xboxes) — stub
            if (is_read) s->data = 0;
        }
        // All other device addresses: auto-complete with data=0
        return;
    }
    case 0x04: s->address = (uint8_t)val; return;
    case 0x06: s->data = (uint8_t)val;    return;
    case 0x08: s->command = (uint8_t)val;  return;
    }
}

// ============================= PIC (8259A) =================================
// Dual 8259A PIC: master (ports 0x20-0x21), slave (ports 0xA0-0xA1).
// Full ICW1-4 initialization sequence, IMR, IRR/ISR, OCW2/OCW3, cascade.

struct Pic8259 {
    uint8_t irr         = 0;     // Interrupt Request Register
    uint8_t imr         = 0xFF;  // Interrupt Mask Register (all masked)
    uint8_t isr         = 0;     // In-Service Register
    uint8_t vector_base = 0;     // ICW2: base vector (top 5 bits)
    uint8_t icw3        = 0;     // ICW3: cascade config
    bool    icw4_needed = false;
    bool    auto_eoi    = false;
    bool    read_isr    = false; // OCW3: read ISR (true) or IRR (false)
    int     init_step   = 0;     // 0=operational, 1..3=expecting ICW2/3/4
    bool    single_mode = false; // ICW1 bit 1: no slave

    void raise(int line)  { irr |=  (1 << line); }
    void lower(int line)  { irr &= ~(1 << line); }

    bool has_pending() const { return (irr & ~imr & ~isr) != 0; }

    int highest_pending() const {
        uint8_t bits = irr & ~imr & ~isr;
        if (!bits) return -1;
        for (int i = 0; i < 8; ++i)
            if (bits & (1 << i)) return i;
        return -1;
    }

    uint8_t ack() {
        int line = highest_pending();
        if (line < 0) return vector_base;
        irr &= ~(1 << line);
        isr |=  (1 << line);
        if (auto_eoi) isr &= ~(1 << line);
        return uint8_t(vector_base + line);
    }

    void eoi() {
        for (int i = 0; i < 8; ++i)
            if (isr & (1 << i)) { isr &= ~(1 << i); return; }
    }
    void eoi_specific(int line) { isr &= ~(1 << line); }

    void write(int port_off, uint8_t val) {
        if (port_off == 0) {
            if (val & 0x10) {                // ICW1
                init_step   = 1;
                irr = 0; isr = 0; imr = 0;
                icw4_needed = (val & 0x01) != 0;
                single_mode = (val & 0x02) != 0;
                read_isr    = false;
                return;
            }
            if ((val & 0x18) == 0x00) {      // OCW2
                int cmd = (val >> 5) & 7;
                if (cmd == 1) eoi();                     // non-specific EOI
                else if (cmd == 3) eoi_specific(val & 7); // specific EOI
                return;
            }
            if ((val & 0x18) == 0x08) {      // OCW3
                if (val & 0x02) read_isr = (val & 0x01) != 0;
                return;
            }
            return;
        }
        // port_off == 1
        if (init_step == 1) {
            vector_base = val & 0xF8;
            init_step = single_mode ? (icw4_needed ? 3 : 0) : 2;
            return;
        }
        if (init_step == 2) {
            icw3 = val;
            init_step = icw4_needed ? 3 : 0;
            return;
        }
        if (init_step == 3) {
            auto_eoi = (val & 0x02) != 0;
            init_step = 0;
            return;
        }
        imr = val;  // OCW1: write mask register
    }

    uint8_t read(int port_off) const {
        if (port_off == 0) return read_isr ? isr : irr;
        return imr;
    }
};

struct PicPair {
    Pic8259 master, slave;

    // Raise a system IRQ line (0-15).
    void raise_irq(int irq) {
        if (irq < 8) { master.raise(irq); }
        else          { slave.raise(irq - 8); master.raise(2); }
    }
    void lower_irq(int irq) {
        if (irq < 8) { master.lower(irq); }
        else {
            slave.lower(irq - 8);
            if (!slave.has_pending()) master.lower(2);
        }
    }

    bool has_pending() const { return master.has_pending(); }

    uint8_t ack() {
        int line = master.highest_pending();
        if (line == 2 && slave.has_pending()) {
            // Cascade: master acknowledges IRQ 2, slave provides the vector.
            master.irr &= ~(1 << 2);
            master.isr |=  (1 << 2);
            if (master.auto_eoi) master.isr &= ~(1 << 2);
            return slave.ack();
        }
        return master.ack();
    }
};

// I/O port handlers for PIC.
static uint32_t pic_master_read(uint16_t port, unsigned /*size*/, void* user) {
    return static_cast<PicPair*>(user)->master.read(port & 1);
}
static void pic_master_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PicPair*>(user)->master.write(port & 1, (uint8_t)val);
}
static uint32_t pic_slave_read(uint16_t port, unsigned /*size*/, void* user) {
    return static_cast<PicPair*>(user)->slave.read(port & 1);
}
static void pic_slave_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    static_cast<PicPair*>(user)->slave.write(port & 1, (uint8_t)val);
}

// IRQ controller callbacks for Executor.
static bool pic_irq_check(void* user) {
    return static_cast<PicPair*>(user)->has_pending();
}
static uint8_t pic_irq_ack(void* user) {
    return static_cast<PicPair*>(user)->ack();
}

// ============================= PIT (8254) ==================================
// Programmable Interval Timer — 3 channels, only channel 0 is wired to IRQ 0.
// Channel 2 gates the PC speaker / system port B.

struct PitChannel {
    uint16_t reload     = 0;     // programmed count value
    uint16_t count      = 0;     // current countdown
    uint8_t  mode       = 0;     // operating mode (0-5)
    uint8_t  access     = 0;     // 0=latch, 1=lo, 2=hi, 3=lo/hi
    bool     lo_written = false; // for lo/hi access: lo byte written
    bool     lo_read    = false; // for lo/hi read: lo byte read (need hi next)
    bool     enabled    = false;
    uint16_t latch_val  = 0;
    bool     latched    = false;
    bool     output     = false; // output pin state (for mode 3 square wave)

    void load_count(uint16_t val) {
        reload  = val ? val : 65536; // 0 means 65536
        count   = reload;
        enabled = true;
        output  = false;
    }
};

struct PitState {
    PitChannel ch[3];
    PicPair*   pic = nullptr;

    // Tick channel 0 by `n` oscillator ticks.  Raises IRQ 0 on the PIC when
    // the counter expires (modes 0, 2, 3).
    void tick(uint32_t n = 1) {
        auto& c = ch[0];
        if (!c.enabled) return;
        for (uint32_t i = 0; i < n; ++i) {
            if (c.count == 0) c.count = c.reload;
            c.count--;
            if (c.count == 0) {
                if (c.mode == 0) {
                    // Mode 0: one-shot interrupt on terminal count.
                    c.output  = true;
                    c.enabled = false;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 2) {
                    // Mode 2: rate generator — reload and fire.
                    c.count = c.reload;
                    if (pic) pic->raise_irq(0);
                } else if (c.mode == 3) {
                    // Mode 3: square wave — toggle output, fire on transition.
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

    if (c.access == 1) return val & 0xFF;         // lo byte only
    if (c.access == 2) return (val >> 8) & 0xFF;  // hi byte only
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
        // Mode/command register.
        int ch_sel  = (val >> 6) & 3;
        int access  = (val >> 4) & 3;
        int mode    = (val >> 1) & 7;

        if (ch_sel == 3) {
            // Read-back command (not implemented — rare).
            return;
        }
        if (access == 0) {
            // Counter latch command.
            pit->ch[ch_sel].latch_val = pit->ch[ch_sel].count;
            pit->ch[ch_sel].latched   = true;
            return;
        }
        pit->ch[ch_sel].access     = (uint8_t)access;
        pit->ch[ch_sel].mode       = (uint8_t)(mode & 3); // modes 4-7 alias to 0-3
        pit->ch[ch_sel].lo_written = false;
        pit->ch[ch_sel].lo_read    = false;
        pit->ch[ch_sel].enabled    = false;
        return;
    }

    // Data register write (port 0x40-0x42).
    int ch = port - 0x40;
    if (ch < 0 || ch > 2) return;
    auto& c = pit->ch[ch];

    if (c.access == 1) {
        // Lo byte only.
        c.load_count((c.reload & 0xFF00) | (val & 0xFF));
    } else if (c.access == 2) {
        // Hi byte only.
        c.load_count(uint16_t(((val & 0xFF) << 8) | (c.reload & 0xFF)));
    } else if (c.access == 3) {
        // Lo/hi byte pair.
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

// ============================= System Port B (0x61) ========================

static uint32_t sysctl_read(uint16_t, unsigned, void*) { return 0x20; /* timer 2 output */ }
static void sysctl_write(uint16_t, uint32_t, unsigned, void*) {}

// ============================= Debug console (0xE9) ========================

static void debug_console_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

// ============================= Setup =======================================
// All device state is heap-allocated and owned by XboxHardware.

struct XboxHardware {
    MmioMap     mmio;
    Nv2aState   nv2a;
    ApuState    apu;
    IdeState    ide;
    IoApicState ioapic;
    FlashState  flash;
    PciState    pci;
    SmbusState  smbus;
    PicPair     pic;
    PitState    pit;
    uint8_t*    ram = nullptr;       // pointer to guest RAM (set by xbox_setup)
    uint32_t    ram_size = 0;        // guest RAM size in bytes
};

// Tick callback for executor run loop — advances PIT, NV2A PTIMER, and PFIFO.
static void hw_tick_callback(void* user) {
    auto* hw = static_cast<XboxHardware*>(user);
    hw->pit.tick();
    hw->nv2a.tick_timer();
    hw->nv2a.tick_fifo(hw->ram, hw->ram_size);
    // Raise NV2A vblank IRQ on PIC if pending.
    if (hw->nv2a.vblank_irq_pending) {
        hw->nv2a.vblank_irq_pending = false;
        hw->pic.raise_irq(1);  // NV2A → IRQ 1 on Xbox
    }
}

// Set up the Xbox MMIO map and I/O ports on an Executor.
// Returns a heap-allocated XboxHardware whose lifetime must exceed the Executor.
inline XboxHardware* xbox_setup(Executor& exec) {
    auto* hw = new XboxHardware();

    // PCI device table
    hw->pci.init_xbox_devices();

    // Initialize Executor first so that exec.ram is allocated.
    exec.init(&hw->mmio);

    // Store RAM pointer for tick_fifo.
    hw->ram = exec.ram;
    hw->ram_size = GUEST_RAM_SIZE;

    // MMIO regions (must come after init — exec.ram is the user pointer)
    hw->mmio.add(RAM_MIRROR_BASE, RAM_MIRROR_SIZE,
                 ram_mirror_read, ram_mirror_write, exec.ram);
    hw->mmio.add(FLASH_BASE, FLASH_SIZE,
                 flash_read, flash_write, &hw->flash);
    hw->mmio.add(NV2A_BASE, NV2A_SIZE,
                 nv2a_read, nv2a_write, &hw->nv2a);
    hw->mmio.add(APU_BASE, APU_SIZE,
                 apu_read, apu_write, &hw->apu);
    hw->mmio.add(IOAPIC_BASE, IOAPIC_SIZE,
                 ioapic_read, ioapic_write, &hw->ioapic);
    // BIOS shadow = flash alias
    hw->mmio.add(BIOS_BASE, BIOS_SIZE,
                 flash_read, flash_write, &hw->flash);

    // I/O ports
    exec.register_io(0x20, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0x21, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0xA0, pic_slave_read, pic_slave_write, &hw->pic);
    exec.register_io(0xA1, pic_slave_read, pic_slave_write, &hw->pic);

    // Connect PIC as the interrupt controller.
    exec.irq_check = pic_irq_check;
    exec.irq_ack   = pic_irq_ack;
    exec.irq_user  = &hw->pic;

    exec.register_io(0x40, pit_io_read, pit_io_write, &hw->pit); // PIT ch0
    exec.register_io(0x41, pit_io_read, pit_io_write, &hw->pit); // PIT ch1
    exec.register_io(0x42, pit_io_read, pit_io_write, &hw->pit); // PIT ch2
    exec.register_io(0x43, pit_io_read, pit_io_write, &hw->pit); // PIT mode

    // Wire PIT channel 0 → IRQ 0 on PIC.
    hw->pit.pic  = &hw->pic;

    // Periodic tick: call PIT + NV2A PTIMER every trace dispatch.
    exec.tick_fn     = hw_tick_callback;
    exec.tick_user   = hw;
    exec.tick_period = 1;

    exec.register_io(0x61, sysctl_read, sysctl_write);         // System B
    exec.register_io(0xE9, nullptr, debug_console_write);       // Debug
    exec.register_io(0xCF8, pci_io_read_cf8, pci_io_write_cf8, &hw->pci);
    exec.register_io(0xCFC, pci_io_read_cfc, pci_io_write_cfc, &hw->pci);

    // SMBus (0xC000–0xC00F): register even-numbered ports used by kernel
    for (uint16_t p = 0xC000; p <= 0xC00E; p += 2)
        exec.register_io(p, smbus_io_read, smbus_io_write, &hw->smbus);

    // IDE (ATA) — primary channel (HDD): 0x1F0–0x1F7, ctrl 0x3F6
    for (uint16_t p = 0x1F0; p <= 0x1F7; p++)
        exec.register_io(p, ide_io_read, ide_io_write, &hw->ide);
    exec.register_io(0x3F6, ide_io_read, ide_io_write, &hw->ide);

    // IDE (ATA) — secondary channel (DVD): 0x170–0x177, ctrl 0x376
    for (uint16_t p = 0x170; p <= 0x177; p++)
        exec.register_io(p, ide_io_read, ide_io_write, &hw->ide);
    exec.register_io(0x376, ide_io_read, ide_io_write, &hw->ide);

    return hw;
}

} // namespace xbox
