#pragma once
// NV2A GPU register stubs — PMC, PFIFO, PTIMER, PCRTC, PGRAPH, PRAMDAC.
#include "address_map.hpp"
#include <cstdint>

namespace xbox {

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
    uint32_t pfifo_intr      = 0;
    uint32_t pfifo_intr_en   = 0;
    uint32_t pfifo_caches    = 0;
    uint32_t pfifo_mode      = 0;
    uint32_t pfifo_cache1_push0 = 0;
    uint32_t pfifo_cache1_pull0 = 0;
    uint32_t pfifo_cache1_push1 = 0;
    uint32_t pfifo_cache1_status = 0x10;
    uint32_t pfifo_cache1_dma_push = 0;
    uint32_t pfifo_cache1_dma_put = 0;
    uint32_t pfifo_cache1_dma_get = 0;
    uint32_t pfifo_runout_status = 0x10;

    // PGRAPH registers
    uint32_t pgraph_intr     = 0;
    uint32_t pgraph_intr_en  = 0;
    uint32_t pgraph_fifo     = 0;
    uint32_t pgraph_channel_ctx_status = 0;

    // PRAMDAC registers — PLL / video clock configuration
    uint32_t pramdac_nvpll   = 0x00011C01;
    uint32_t pramdac_mpll    = 0x00011801;
    uint32_t pramdac_vpll    = 0x00031801;
    uint32_t pramdac_pll_test = 0;

    // PTIMER: 56-bit freerunning nanosecond counter.
    uint64_t ptimer_ns = 0;
    static constexpr uint64_t NS_PER_TICK = 100;

    // PCRTC vblank
    uint32_t vblank_counter = 0;
    static constexpr uint32_t VBLANK_PERIOD = 16667;
    bool     vblank_irq_pending = false;

    void tick_timer() {
        ptimer_ns += NS_PER_TICK;
        if (++vblank_counter >= VBLANK_PERIOD) {
            vblank_counter = 0;
            if (pcrtc_intr_en & 1)
                pcrtc_intr |= 1;
            if ((pmc_intr_en & 0x01000000) && (pcrtc_intr & 1))
                vblank_irq_pending = true;
        }
    }

    static constexpr uint32_t MAX_DWORDS_PER_TICK = 128;

    void tick_fifo(const uint8_t* /*ram*/, uint32_t /*ram_size_bytes*/) {
        if (!(pfifo_cache1_dma_push & 1)) return;
        if (!(pfifo_cache1_push0 & 1)) return;
        if (pfifo_cache1_dma_get == pfifo_cache1_dma_put) {
            pfifo_cache1_status = 0x10;
            return;
        }
        pfifo_cache1_status = 0;
        uint32_t get = pfifo_cache1_dma_get;
        uint32_t put = pfifo_cache1_dma_put;
        uint32_t consumed = 0;
        while (get != put && consumed < MAX_DWORDS_PER_TICK) {
            get += 4;
            consumed++;
        }
        pfifo_cache1_dma_get = get;
        if (get == put)
            pfifo_cache1_status = 0x10;
    }

    uint32_t pmc_intr_0() const {
        uint32_t val = 0;
        if (pcrtc_intr & 1) val |= 0x01000000;
        return val;
    }
};

static uint32_t nv2a_read(uint32_t pa, unsigned size, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    if (off == 0x000000) return nv->pmc_boot_0;
    if (off == 0x000100) return nv->pmc_intr_0();
    if (off == 0x000140) return nv->pmc_intr_en;
    if (off == 0x000200) return nv->pmc_enable;
    if (off == 0x001200) return nv->pbus_0;
    if (off == 0x001214) return 0;
    if (off == 0x002100) return nv->pfifo_intr;
    if (off == 0x002140) return nv->pfifo_intr_en;
    if (off == 0x002500) return nv->pfifo_caches;
    if (off == 0x002504) return nv->pfifo_mode;
    if (off == 0x003200) return nv->pfifo_cache1_push0;
    if (off == 0x003210) return nv->pfifo_cache1_push1;
    if (off == 0x003220) return nv->pfifo_cache1_dma_push;
    if (off == 0x003240) return nv->pfifo_cache1_dma_put;
    if (off == 0x003244) return nv->pfifo_cache1_dma_get;
    if (off == 0x003250) return nv->pfifo_cache1_pull0;
    if (off == 0x003214) return nv->pfifo_cache1_status;
    if (off == 0x002400) return nv->pfifo_runout_status;
    if (off == 0x003228) {
        bool busy = (nv->pfifo_cache1_dma_push & 1) &&
                    (nv->pfifo_cache1_dma_get != nv->pfifo_cache1_dma_put);
        return busy ? 1u : 0u;
    }
    if (off == 0x009200) return nv->ptimer_num;
    if (off == 0x009210) return nv->ptimer_den;
    if (off == 0x009400) return (uint32_t)(nv->ptimer_ns & 0xFFFFFFE0u);
    if (off == 0x009410) return (uint32_t)(nv->ptimer_ns >> 32);
    if (off == 0x009100) return 0;
    if (off == 0x100200) return nv->pfb_cfg0;
    if (off == 0x100204) return 0;
    if (off == 0x600100) return nv->pcrtc_intr;
    if (off == 0x600140) return nv->pcrtc_intr_en;
    if (off == 0x600800) return nv->pcrtc_start;
    if (off == 0x008100) return nv->pvideo_intr;
    if (off == 0x400100) return nv->pgraph_intr;
    if (off == 0x400140) return nv->pgraph_intr_en;
    if (off == 0x400720) return nv->pgraph_fifo;
    if (off == 0x400170) return nv->pgraph_channel_ctx_status;
    if (off == 0x680500) return nv->pramdac_nvpll;
    if (off == 0x680504) return nv->pramdac_mpll;
    if (off == 0x680508) return nv->pramdac_vpll;
    if (off == 0x680514) return nv->pramdac_pll_test;
    if (off == 0x680600) return 0x00000101;
    if (off == 0x6808C0) return 0;

    return 0;
}

static void nv2a_write(uint32_t pa, uint32_t val, unsigned /*size*/, void* user) {
    auto* nv = static_cast<Nv2aState*>(user);
    uint32_t off = pa - NV2A_BASE;

    if (off == 0x000140) { nv->pmc_intr_en = val; return; }
    if (off == 0x000200) { nv->pmc_enable = val; return; }
    if (off == 0x001200) { nv->pbus_0 = val;     return; }
    if (off == 0x009200) { nv->ptimer_num = val;  return; }
    if (off == 0x009210) { nv->ptimer_den = val;  return; }
    if (off == 0x600100) { nv->pcrtc_intr &= ~val; return; }
    if (off == 0x600140) { nv->pcrtc_intr_en = val; return; }
    if (off == 0x600800) { nv->pcrtc_start = val; return; }
    if (off == 0x008100) { nv->pvideo_intr &= ~val; return; }
    if (off == 0x002100) { nv->pfifo_intr &= ~val; return; }
    if (off == 0x002140) { nv->pfifo_intr_en = val; return; }
    if (off == 0x002500) { nv->pfifo_caches = val; return; }
    if (off == 0x002504) { nv->pfifo_mode = val; return; }
    if (off == 0x003200) { nv->pfifo_cache1_push0 = val; return; }
    if (off == 0x003210) { nv->pfifo_cache1_push1 = val; return; }
    if (off == 0x003220) { nv->pfifo_cache1_dma_push = val; return; }
    if (off == 0x003240) { nv->pfifo_cache1_dma_put = val; return; }
    if (off == 0x003244) { nv->pfifo_cache1_dma_get = val; return; }
    if (off == 0x003250) { nv->pfifo_cache1_pull0 = val; return; }
    if (off == 0x400100) { nv->pgraph_intr &= ~val; return; }
    if (off == 0x400140) { nv->pgraph_intr_en = val; return; }
    if (off == 0x400720) { nv->pgraph_fifo = val; return; }
    if (off == 0x400170) { nv->pgraph_channel_ctx_status = val; return; }
    if (off == 0x680500) { nv->pramdac_nvpll = val; return; }
    if (off == 0x680504) { nv->pramdac_mpll = val; return; }
    if (off == 0x680508) { nv->pramdac_vpll = val; return; }
    if (off == 0x680514) { nv->pramdac_pll_test = val; return; }
}

} // namespace xbox
