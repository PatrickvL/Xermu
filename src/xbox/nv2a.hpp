#pragma once
// ---------------------------------------------------------------------------
// nv2a.hpp — NV2A GPU register file + PFIFO DMA pusher.
//
// Per-block flat register arrays indexed by block-relative offset / 4,
// matching the PGRAPH / APU pattern.  Named constants in per-block
// namespaces mirror NV2A register documentation.
// ---------------------------------------------------------------------------
#include "address_map.hpp"
#include <cstdint>
#include <cstring>

namespace xbox {

// ===================== Per-Block Register Offset Constants ==================
// Offsets are relative to each block's base within the NV2A MMIO range.
// Block bases (from NV2A_BASE): PMC +0x000000, PBUS +0x001000,
// PFIFO +0x002000, PVIDEO +0x008000, PTIMER +0x009000, PFB +0x100000,
// PGRAPH +0x400000, PCRTC +0x600000, PRAMDAC +0x680000.

namespace pmc {
static constexpr uint32_t BOOT_0    = 0x000;   // chip ID (NV2A derivative)
static constexpr uint32_t INTR_0    = 0x100;   // master interrupt status (computed)
static constexpr uint32_t INTR_EN   = 0x140;   // master interrupt enable
static constexpr uint32_t ENABLE    = 0x200;   // subsystem enable
} // namespace pmc

namespace pbus {
static constexpr uint32_t INTR       = 0x100;   // PBUS interrupt status (W1C)
static constexpr uint32_t INTR_EN    = 0x140;   // PBUS interrupt enable
static constexpr uint32_t REG_0      = 0x200;   // bus control register 0
static constexpr uint32_t FBIO_RAM   = 0x218;   // FBIO RAM type (DDR indicator)
static constexpr uint32_t DEBUG_1    = 0x084;   // debug register 1
} // namespace pbus

namespace pfifo {
// Base PFIFO registers (block-relative 0x0000-0x0FFF)
static constexpr uint32_t INTR            = 0x0100;
static constexpr uint32_t INTR_EN         = 0x0140;
static constexpr uint32_t RUNOUT_STATUS   = 0x0400;
static constexpr uint32_t CACHES          = 0x0500;
static constexpr uint32_t MODE            = 0x0504;
// CACHE1 registers (block-relative 0x1000-0x1FFF)
static constexpr uint32_t CACHE1_PUSH0    = 0x1200;
static constexpr uint32_t CACHE1_PUSH1    = 0x1210;
static constexpr uint32_t CACHE1_STATUS   = 0x1214;
static constexpr uint32_t CACHE1_DMA_PUSH = 0x1220;
static constexpr uint32_t CACHE1_DMA_STATE = 0x1228; // computed: busy flag
static constexpr uint32_t CACHE1_DMA_PUT  = 0x1240;
static constexpr uint32_t CACHE1_DMA_GET  = 0x1244;
static constexpr uint32_t CACHE1_DMA_SUBROUTINE = 0x124C; // bits[28:2] return addr, bit[0] active
static constexpr uint32_t CACHE1_PULL0    = 0x1250;
// Emulator extensions (diagnostics / testing)
static constexpr uint32_t EXT_METHODS     = 0x1F00;  // methods dispatched count
static constexpr uint32_t EXT_DWORDS      = 0x1F04;  // dwords consumed count
static constexpr uint32_t EXT_JUMPS       = 0x1F08;  // jump commands count
static constexpr uint32_t EXT_CALLS       = 0x1F0C;  // call commands count
} // namespace pfifo

namespace pvideo {
static constexpr uint32_t INTR     = 0x100;    // PVIDEO interrupt status (W1C)
static constexpr uint32_t INTR_EN  = 0x140;    // PVIDEO interrupt enable
static constexpr uint32_t BUFFER   = 0x700;    // active buffer select (bit 4)
static constexpr uint32_t STOP     = 0x704;    // stop overlay
static constexpr uint32_t BASE0    = 0x900;    // buffer 0 base address
static constexpr uint32_t BASE1    = 0x904;    // buffer 1 base address
static constexpr uint32_t LIMIT0   = 0x908;    // buffer 0 limit
static constexpr uint32_t LIMIT1   = 0x90C;    // buffer 1 limit
static constexpr uint32_t LUMINANCE0 = 0x910;  // buffer 0 luminance range
static constexpr uint32_t LUMINANCE1 = 0x914;  // buffer 1 luminance range
static constexpr uint32_t CHROMINANCE0 = 0x918;// buffer 0 chrominance range
static constexpr uint32_t CHROMINANCE1 = 0x91C;// buffer 1 chrominance range
static constexpr uint32_t OFFSET0  = 0x920;    // buffer 0 offset
static constexpr uint32_t OFFSET1  = 0x924;    // buffer 1 offset
static constexpr uint32_t SIZE_IN0 = 0x928;    // buffer 0 input size
static constexpr uint32_t SIZE_IN1 = 0x92C;    // buffer 1 input size
static constexpr uint32_t POINT_IN0 = 0x930;   // buffer 0 input point
static constexpr uint32_t POINT_IN1 = 0x934;   // buffer 1 input point
static constexpr uint32_t DS_DX0   = 0x938;    // buffer 0 horizontal scale
static constexpr uint32_t DS_DX1   = 0x93C;    // buffer 1 horizontal scale
static constexpr uint32_t DT_DY0   = 0x940;    // buffer 0 vertical scale
static constexpr uint32_t DT_DY1   = 0x944;    // buffer 1 vertical scale
static constexpr uint32_t POINT_OUT0 = 0x948;  // buffer 0 output point
static constexpr uint32_t POINT_OUT1 = 0x94C;  // buffer 1 output point
static constexpr uint32_t SIZE_OUT0  = 0x950;  // buffer 0 output size
static constexpr uint32_t SIZE_OUT1  = 0x954;  // buffer 1 output size
static constexpr uint32_t FORMAT0  = 0x958;    // buffer 0 format (color, pitch)
static constexpr uint32_t FORMAT1  = 0x95C;    // buffer 1 format
} // namespace pvideo

namespace ptimer {
static constexpr uint32_t INTR     = 0x100;    // PTIMER interrupt status (W1C)
static constexpr uint32_t INTR_EN  = 0x140;    // PTIMER interrupt enable
static constexpr uint32_t NUM      = 0x200;    // numerator
static constexpr uint32_t DEN      = 0x210;    // denominator
static constexpr uint32_t TIME_0   = 0x400;    // low 32 bits of ns counter (computed)
static constexpr uint32_t TIME_1   = 0x410;    // high 24 bits of ns counter (computed)
static constexpr uint32_t ALARM_0  = 0x420;    // alarm low 32 bits
} // namespace ptimer

namespace pfb {
static constexpr uint32_t CFG0     = 0x200;    // framebuffer config (RAM size)
} // namespace pfb

namespace pgraph_ctl {
static constexpr uint32_t INTR       = 0x100;
static constexpr uint32_t INTR_EN    = 0x140;
static constexpr uint32_t CTX_STATUS = 0x170;
static constexpr uint32_t FIFO       = 0x720;
} // namespace pgraph_ctl

namespace pcrtc {
static constexpr uint32_t INTR     = 0x100;    // vblank interrupt status (W1C)
static constexpr uint32_t INTR_EN  = 0x140;    // vblank interrupt enable
static constexpr uint32_t START    = 0x800;    // framebuffer start address
static constexpr uint32_t CONFIG   = 0x804;    // display config (bpp, width)
static constexpr uint32_t RASTER   = 0x808;    // current raster line (read-only)
} // namespace pcrtc

namespace pramdac {
static constexpr uint32_t NVPLL     = 0x500;   // core PLL
static constexpr uint32_t MPLL      = 0x504;   // memory PLL
static constexpr uint32_t VPLL      = 0x508;   // video PLL
static constexpr uint32_t PLL_TEST  = 0x514;   // PLL test register
static constexpr uint32_t FP_DEBUG0 = 0x600;   // flat-panel debug 0
static constexpr uint32_t GENERAL_CTRL = 0x600; // general control
static constexpr uint32_t FP_TMDS   = 0x8C0;   // TMDS control
static constexpr uint32_t TV_SETUP  = 0x700;   // TV encoder setup
static constexpr uint32_t TV_VTOTAL = 0x720;   // TV vertical total lines
static constexpr uint32_t TV_VSKEW  = 0x724;   // TV vertical skew
static constexpr uint32_t TV_VSYNC_D = 0x728;  // TV vsync delay
} // namespace pramdac

// ========================== NV2A State =====================================

struct Nv2aState {
    // --- Per-block register arrays (indexed by block-relative offset / 4) ---
    static constexpr uint32_t PMC_COUNT     = 0x400  / 4;  // 256 slots
    static constexpr uint32_t PBUS_COUNT    = 0x400  / 4;
    static constexpr uint32_t PFIFO_COUNT   = 0x2000 / 4;  // 2048 slots (0x002000-0x003FFF)
    static constexpr uint32_t PVIDEO_COUNT  = 0xA00  / 4;  // covers up to 0x960
    static constexpr uint32_t PTIMER_COUNT  = 0x800  / 4;
    static constexpr uint32_t PFB_COUNT     = 0x400  / 4;
    static constexpr uint32_t PGRAPH_COUNT  = 0x800  / 4;
    static constexpr uint32_t PCRTC_COUNT   = 0x1000 / 4;
    static constexpr uint32_t PRAMDAC_COUNT = 0x1000 / 4;

    uint32_t pmc_regs[PMC_COUNT]         = {};
    uint32_t pbus_regs[PBUS_COUNT]       = {};
    uint32_t pfifo_regs[PFIFO_COUNT]     = {};
    uint32_t pvideo_regs[PVIDEO_COUNT]   = {};
    uint32_t ptimer_regs[PTIMER_COUNT]   = {};
    uint32_t pfb_regs[PFB_COUNT]         = {};
    uint32_t pgraph_regs[PGRAPH_COUNT]   = {};
    uint32_t pcrtc_regs[PCRTC_COUNT]     = {};
    uint32_t pramdac_regs[PRAMDAC_COUNT] = {};

    // PRAMIN: Private Raster Memory (GPU instance memory).
    // Mapped at NV2A_BASE + 0x700000 (1 MB). Contains RAMHT, RAMFC, RAMRO,
    // and GPU object instances used by PFIFO and PGRAPH.
    static constexpr uint32_t PRAMIN_SIZE = 1024 * 1024;  // 1 MB
    uint8_t pramin[PRAMIN_SIZE] = {};

    // PTIMER: 56-bit freerunning nanosecond counter (not a flat register).
    uint64_t ptimer_ns = 0;
    static constexpr uint64_t NS_PER_TICK = 100;

    // Pointer to PGRAPH state shadow (set by xbox_setup, used for diag reads).
    struct PgraphState;  // forward decl — full definition in pgraph.hpp
    void* pgraph_ptr = nullptr;

    // PCRTC vblank
    uint32_t vblank_counter = 0;
    static constexpr uint32_t VBLANK_PERIOD = 16667;
    bool     vblank_irq_pending = false;

    // GPU method handler — called for each (subchannel, method, data) tuple.
    using MethodHandler = void(*)(void* user, uint32_t subchannel,
                                  uint32_t method, uint32_t data);
    MethodHandler method_handler = nullptr;
    void*         method_user    = nullptr;

    // FIFO thread notification — called when DMA_PUT or enable registers
    // are written so the NV2A processing thread can wake up.
    using FifoNotify = void(*)(void* user);
    FifoNotify fifo_notify      = nullptr;
    void*      fifo_notify_user = nullptr;

    Nv2aState() {
        pmc_regs[pmc::BOOT_0 / 4]             = 0x02A000A1;  // NV2A chip ID
        pmc_regs[pmc::ENABLE / 4]             = 0xFFFFFFFF;  // all subsystems enabled
        pbus_regs[pbus::FBIO_RAM / 4]         = 0x00000003;  // DDR SDRAM
        ptimer_regs[ptimer::NUM / 4]           = 1;
        ptimer_regs[ptimer::DEN / 4]           = 1;
        pfb_regs[pfb::CFG0 / 4]               = 0x03070103;  // 64 MB RAM
        pfifo_regs[pfifo::CACHE1_STATUS / 4]   = 0x10;       // idle
        pfifo_regs[pfifo::RUNOUT_STATUS / 4]   = 0x10;       // idle
        pramdac_regs[pramdac::NVPLL / 4]       = 0x00011C01;
        pramdac_regs[pramdac::MPLL / 4]        = 0x00011801;
        pramdac_regs[pramdac::VPLL / 4]        = 0x00031801;
    }

    void tick_timer() {
        ptimer_ns += NS_PER_TICK;
        // Check PTIMER alarm: if alarm is set and time >= alarm, set INTR bit 0.
        uint32_t alarm_lo = ptimer_regs[ptimer::ALARM_0 / 4];
        if (alarm_lo != 0) {
            uint32_t time_lo = (uint32_t)(ptimer_ns & 0xFFFFFFE0u);
            if (time_lo >= alarm_lo)
                ptimer_regs[ptimer::INTR / 4] |= 1;
        }
        if (++vblank_counter >= VBLANK_PERIOD) {
            vblank_counter = 0;
            if (pcrtc_regs[pcrtc::INTR_EN / 4] & 1)
                pcrtc_regs[pcrtc::INTR / 4] |= 1;
            if ((pmc_regs[pmc::INTR_EN / 4] & 0x01000000) &&
                (pcrtc_regs[pcrtc::INTR / 4] & 1))
                vblank_irq_pending = true;
        }
    }

    uint32_t pmc_intr_0() const {
        // Computed: aggregates pending interrupts from sub-blocks.
        // Each sub-block's INTR register is non-zero if any interrupt is pending.
        // PMC_INTR_0 bit assignments (NV2A):
        //   bit  0 = PFIFO
        //   bit  1 = PVIDEO
        //   bit  4 = PTIMER
        //   bit  8 = PGRAPH  (not yet wired)
        //   bit 24 = PCRTC
        //   bit 28 = PBUS
        uint32_t val = 0;
        if (pfifo_regs[pfifo::INTR / 4])   val |= (1u << 0);
        if (pvideo_regs[pvideo::INTR / 4])  val |= (1u << 1);
        if (ptimer_regs[ptimer::INTR / 4])  val |= (1u << 4);
        if (pcrtc_regs[pcrtc::INTR / 4])    val |= (1u << 24);
        if (pbus_regs[pbus::INTR / 4])      val |= (1u << 28);
        return val;
    }

    // ---------------------------------------------------------------
    // PFIFO DMA pusher
    // ---------------------------------------------------------------
    static constexpr uint32_t MAX_DWORDS_PER_TICK = 128;

    void tick_fifo(const uint8_t* ram, uint32_t ram_size_bytes) {
        auto& dma_push = pfifo_regs[pfifo::CACHE1_DMA_PUSH / 4];
        auto& push0    = pfifo_regs[pfifo::CACHE1_PUSH0 / 4];
        auto& dma_get  = pfifo_regs[pfifo::CACHE1_DMA_GET / 4];
        auto& dma_put  = pfifo_regs[pfifo::CACHE1_DMA_PUT / 4];
        auto& status   = pfifo_regs[pfifo::CACHE1_STATUS / 4];
        auto& subr     = pfifo_regs[pfifo::CACHE1_DMA_SUBROUTINE / 4];
        auto& ext_methods = pfifo_regs[pfifo::EXT_METHODS / 4];
        auto& ext_dwords  = pfifo_regs[pfifo::EXT_DWORDS / 4];
        auto& ext_jumps   = pfifo_regs[pfifo::EXT_JUMPS / 4];
        auto& ext_calls   = pfifo_regs[pfifo::EXT_CALLS / 4];

        if (!(dma_push & 1)) return;
        if (!(push0 & 1)) return;
        if (dma_get == dma_put) { status = 0x10; return; }
        status = 0;

        uint32_t get = dma_get;
        uint32_t put = dma_put;
        uint32_t consumed = 0;

        while (get != put && consumed < MAX_DWORDS_PER_TICK) {
            if (get + 4 > ram_size_bytes) break;

            uint32_t hdr;
            memcpy(&hdr, ram + get, 4);
            get += 4;
            consumed++;

            if (hdr == 0) continue;  // NOP

            if ((hdr & 0xC0000000u) == 0x40000000u) {
                // JUMP
                get = hdr & 0x1FFFFFFCu;
                ext_jumps++;
                continue;
            }

            if ((hdr & 3) == 2) {
                // CALL: save GET, jump to target (single-level on Xbox).
                if (!(subr & 1)) {
                    subr = (get & 0x1FFFFFFC) | 1;
                    get = hdr & 0xFFFFFFFCu;
                    ext_calls++;
                }
                continue;
            }

            if (hdr == 0x00020000u) {
                // RETURN: restore GET from saved address.
                if (subr & 1) {
                    get = subr & 0x1FFFFFFC;
                    subr = 0;
                }
                continue;
            }

            uint32_t type = (hdr >> 29) & 0x7;
            if (type == 0 || type == 4) {
                uint32_t method     = (hdr >>  0) & 0x1FFC;
                uint32_t subchannel = (hdr >> 13) & 0x7;
                uint32_t count      = (hdr >> 18) & 0x7FF;

                for (uint32_t i = 0; i < count && consumed < MAX_DWORDS_PER_TICK; ++i) {
                    if (get + 4 > ram_size_bytes) goto done;

                    uint32_t data;
                    memcpy(&data, ram + get, 4);
                    get += 4;
                    consumed++;

                    uint32_t m = (type == 0) ? (method + i * 4) : method;
                    if (method_handler)
                        method_handler(method_user, subchannel, m, data);
                    ext_methods++;
                }
            }
        }
done:
        ext_dwords += consumed;
        dma_get = get;
        if (get == put)
            status = 0x10;
    }
};

// ========================== MMIO Handlers ==================================

static uint32_t nv2a_read(uint32_t pa, unsigned /*size*/, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    // --- PMC (0x000000) ---
    if (off < 0x001000) {
        if (off == pmc::INTR_0) return nv->pmc_intr_0();  // computed
        if (off / 4 < Nv2aState::PMC_COUNT) return nv->pmc_regs[off / 4];
        return 0;
    }
    // --- PBUS (0x001000) ---
    if (off < 0x002000) {
        uint32_t r = off - 0x001000;
        if (r / 4 < Nv2aState::PBUS_COUNT) return nv->pbus_regs[r / 4];
        return 0;
    }
    // --- PFIFO (0x002000) ---
    if (off < 0x004000) {
        uint32_t r = off - 0x002000;
        // DMA_STATE is computed: busy if push enabled and GET != PUT.
        if (r == pfifo::CACHE1_DMA_STATE) {
            bool busy = (nv->pfifo_regs[pfifo::CACHE1_DMA_PUSH / 4] & 1) &&
                        (nv->pfifo_regs[pfifo::CACHE1_DMA_GET / 4] !=
                         nv->pfifo_regs[pfifo::CACHE1_DMA_PUT / 4]);
            return busy ? 1u : 0u;
        }
        if (r / 4 < Nv2aState::PFIFO_COUNT) return nv->pfifo_regs[r / 4];
        return 0;
    }
    // --- PVIDEO (0x008000) ---
    if (off >= 0x008000 && off < 0x009000) {
        uint32_t r = off - 0x008000;
        if (r / 4 < Nv2aState::PVIDEO_COUNT) return nv->pvideo_regs[r / 4];
        return 0;
    }
    // --- PTIMER (0x009000) ---
    if (off >= 0x009000 && off < 0x00A000) {
        uint32_t r = off - 0x009000;
        if (r == ptimer::TIME_0) return (uint32_t)(nv->ptimer_ns & 0xFFFFFFE0u);
        if (r == ptimer::TIME_1) return (uint32_t)(nv->ptimer_ns >> 32);
        if (r / 4 < Nv2aState::PTIMER_COUNT) return nv->ptimer_regs[r / 4];
        return 0;
    }
    // --- PFB (0x100000) ---
    if (off >= 0x100000 && off < 0x101000) {
        uint32_t r = off - 0x100000;
        if (r / 4 < Nv2aState::PFB_COUNT) return nv->pfb_regs[r / 4];
        return 0;
    }
    // --- PGRAPH control (0x400000) ---
    if (off >= 0x400000 && off < 0x401000) {
        uint32_t r = off - 0x400000;
        if (r / 4 < Nv2aState::PGRAPH_COUNT) return nv->pgraph_regs[r / 4];
        return 0;
    }
    // --- PCRTC (0x600000) ---
    if (off >= 0x600000 && off < 0x601000) {
        uint32_t r = off - 0x600000;
        // RASTER: current scanline (computed from vblank_counter).
        // NTSC: 525 lines total, 480 visible. Map counter to scanline.
        if (r == pcrtc::RASTER) {
            uint32_t line = (nv->vblank_counter * 525u) / Nv2aState::VBLANK_PERIOD;
            return line;
        }
        if (r / 4 < Nv2aState::PCRTC_COUNT) return nv->pcrtc_regs[r / 4];
        return 0;
    }
    // --- PRAMDAC (0x680000) ---
    if (off >= 0x680000 && off < 0x681000) {
        uint32_t r = off - 0x680000;
        if (r == pramdac::FP_DEBUG0) return 0x00000101;  // hardwired
        if (r / 4 < Nv2aState::PRAMDAC_COUNT) return nv->pramdac_regs[r / 4];
        return 0;
    }
    // --- PRAMIN (0x700000) ---
    if (off >= 0x700000 && off < 0x800000) {
        uint32_t r = off - 0x700000;
        if (r + 3 < Nv2aState::PRAMIN_SIZE) {
            uint32_t v;
            memcpy(&v, nv->pramin + r, 4);
            return v;
        }
        return 0;
    }

    return 0;
}

static void nv2a_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    // --- PMC (0x000000) ---
    if (off < 0x001000) {
        if (off / 4 < Nv2aState::PMC_COUNT) nv->pmc_regs[off / 4] = val;
        return;
    }
    // --- PBUS (0x001000) ---
    if (off < 0x002000) {
        uint32_t r = off - 0x001000;
        if (r == pbus::INTR) { nv->pbus_regs[r / 4] &= ~val; return; }
        if (r / 4 < Nv2aState::PBUS_COUNT) nv->pbus_regs[r / 4] = val;
        return;
    }
    // --- PFIFO (0x002000) ---
    if (off < 0x004000) {
        uint32_t r = off - 0x002000;
        // W1C interrupt status
        if (r == pfifo::INTR) {
            nv->pfifo_regs[r / 4] &= ~val;
            return;
        }
        // Registers that trigger fifo_notify on write
        bool notify = (r == pfifo::CACHE1_DMA_PUT  ||
                       r == pfifo::CACHE1_PUSH0     ||
                       r == pfifo::CACHE1_DMA_PUSH  ||
                       r == pfifo::CACHE1_DMA_GET);
        if (r / 4 < Nv2aState::PFIFO_COUNT) nv->pfifo_regs[r / 4] = val;
        if (notify && nv->fifo_notify)
            nv->fifo_notify(nv->fifo_notify_user);
        return;
    }
    // --- PVIDEO (0x008000) ---
    if (off >= 0x008000 && off < 0x009000) {
        uint32_t r = off - 0x008000;
        if (r == pvideo::INTR) { nv->pvideo_regs[r / 4] &= ~val; return; }
        if (r / 4 < Nv2aState::PVIDEO_COUNT) nv->pvideo_regs[r / 4] = val;
        return;
    }
    // --- PTIMER (0x009000) ---
    if (off >= 0x009000 && off < 0x00A000) {
        uint32_t r = off - 0x009000;
        if (r == ptimer::INTR) { nv->ptimer_regs[r / 4] &= ~val; return; }
        if (r / 4 < Nv2aState::PTIMER_COUNT) nv->ptimer_regs[r / 4] = val;
        return;
    }
    // --- PFB (0x100000) ---
    if (off >= 0x100000 && off < 0x101000) {
        uint32_t r = off - 0x100000;
        if (r / 4 < Nv2aState::PFB_COUNT) nv->pfb_regs[r / 4] = val;
        return;
    }
    // --- PGRAPH control (0x400000) ---
    if (off >= 0x400000 && off < 0x401000) {
        uint32_t r = off - 0x400000;
        if (r == pgraph_ctl::INTR) { nv->pgraph_regs[r / 4] &= ~val; return; }
        if (r / 4 < Nv2aState::PGRAPH_COUNT) nv->pgraph_regs[r / 4] = val;
        return;
    }
    // --- PCRTC (0x600000) ---
    if (off >= 0x600000 && off < 0x601000) {
        uint32_t r = off - 0x600000;
        if (r == pcrtc::INTR) { nv->pcrtc_regs[r / 4] &= ~val; return; }
        if (r / 4 < Nv2aState::PCRTC_COUNT) nv->pcrtc_regs[r / 4] = val;
        return;
    }
    // --- PRAMDAC (0x680000) ---
    if (off >= 0x680000 && off < 0x681000) {
        uint32_t r = off - 0x680000;
        if (r / 4 < Nv2aState::PRAMDAC_COUNT) nv->pramdac_regs[r / 4] = val;
        return;
    }
    // --- PRAMIN (0x700000) ---
    if (off >= 0x700000 && off < 0x800000) {
        uint32_t r = off - 0x700000;
        if (r + 3 < Nv2aState::PRAMIN_SIZE)
            memcpy(nv->pramin + r, &val, 4);
        return;
    }
}

} // namespace xbox
