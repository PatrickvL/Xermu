#pragma once
// Xbox hardware setup — XboxHardware struct, tick callback, xbox_setup().
#include "address_map.hpp"
#include "nv2a.hpp"
#include "nv2a_thread.hpp"
#include "pgraph.hpp"
#include "apu.hpp"
#include "ide.hpp"
#include "usb.hpp"
#include "ioapic.hpp"
#include "ram_mirror.hpp"
#include "flash.hpp"
#include "pci.hpp"
#include "smbus.hpp"
#include "pic.hpp"
#include "pit.hpp"
#include "cmos.hpp"
#include "misc_io.hpp"
#include "acpi.hpp"
#include "cpu/executor.hpp"
#include <cstring>

namespace xbox {

struct XboxHardware {
    MmioMap     mmio;
    Nv2aState    nv2a;
    Nv2aThread   nv2a_thread;
    PgraphState  pgraph;
    ApuState    apu;
    IdeState    ide;
    OhciState   usb0;
    OhciState   usb1;
    IoApicState ioapic;
    FlashState  flash;
    PciState    pci;
    SmbusState  smbus;
    PicPair     pic;
    PitState    pit;
    CmosState   cmos;
    MiscIoState misc;
    AcpiState   acpi;
    uint8_t*    ram = nullptr;
    uint32_t    ram_size = 0;
    uint32_t    tick_accum = 0;     // accumulator for KeTickCount timing
};

static void hw_tick_callback(void* user) {
    auto* hw = static_cast<XboxHardware*>(user);
    hw->pit.tick();
    hw->nv2a.tick_timer();
    hw->usb0.tick_frame();
    hw->usb1.tick_frame();

    // CMOS/RTC periodic interrupt → IRQ 8 via PIC.
    if (hw->cmos.tick()) {
        hw->pic.raise_irq(8);
    }

    // NV2A PFIFO: sync USER channel writes from fastmem-committed NV2A memory.
    // When NV2A range is committed (no VEH faults), the game's writes to
    // USER PUT (0xFD800040) and PFIFO DMA_PUT (0xFD003240) go directly to RAM.
    // We poll them here to trigger PFIFO processing.
    if (hw->ram && hw->nv2a.fastmem_nv2a) {
        uint8_t* nv2a_mem = hw->nv2a.fastmem_nv2a;
        // Check USER channel PUT (offset 0x800040)
        uint32_t user_put;
        memcpy(&user_put, nv2a_mem + 0x800040, 4);
        uint32_t cur_put = hw->nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUT / 4];
        if (user_put != 0 && user_put != cur_put) {
            // Simulate the DMA_PUT write handler
            hw->nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUT / 4] = user_put;
            hw->nv2a.pfifo_regs[pfifo::CACHE1_PUSH0 / 4] |= 1;
            hw->nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUSH / 4] |= pfifo::DMA_PUSH_ENABLE;
            static int put_diag = 0;
            if (put_diag < 3) { put_diag++; fprintf(stderr, "[diag] USER PUT sync: 0x%X\n", user_put); }
        }
        // Sync DMA_GET back to committed memory so game reads see progress
        uint32_t get_val = hw->nv2a.pfifo_regs[pfifo::CACHE1_DMA_GET / 4];
        memcpy(nv2a_mem + 0x800044, &get_val, 4);
        // Also sync to PFIFO register space (game may read 0xFD003244 directly)
        memcpy(nv2a_mem + 0x002000 + pfifo::CACHE1_DMA_GET, &get_val, 4);
        // Sync CACHE1_REF to USER REF (offset 0x800048) and PFIFO space
        uint32_t ref_val = hw->nv2a.pfifo_regs[pfifo::CACHE1_REF / 4];
        memcpy(nv2a_mem + 0x800048, &ref_val, 4);
        memcpy(nv2a_mem + 0x002000 + pfifo::CACHE1_REF, &ref_val, 4);
        // Sync CACHE1_DMA_PUT so game can verify PUT was accepted
        uint32_t put_reg = hw->nv2a.pfifo_regs[pfifo::CACHE1_DMA_PUT / 4];
        memcpy(nv2a_mem + 0x002000 + pfifo::CACHE1_DMA_PUT, &put_reg, 4);
        // Sync CACHE1_STATUS: idle if GET==PUT
        uint32_t status_val = (get_val == put_reg) ? pfifo::STATUS_IDLE : 0u;
        memcpy(nv2a_mem + 0x002000 + pfifo::CACHE1_STATUS, &status_val, 4);
        // Sync PGRAPH_STATUS: 0 = idle (game checks before submitting)
        uint32_t pgraph_status = 0;
        memcpy(nv2a_mem + 0x400000 + 0x0700, &pgraph_status, 4);  // PGRAPH_STATUS

        // Also sync PCRTC_INTR acknowledgment: if game wrote to 0x600100, handle W1C
        uint32_t pcrtc_intr_mem;
        memcpy(&pcrtc_intr_mem, nv2a_mem + 0x600100, 4);
        if (pcrtc_intr_mem != 0) {
            // Game acknowledged interrupt — clear the bit(s) and update IRQ
            hw->nv2a.pcrtc_regs[pcrtc::INTR / 4] &= ~pcrtc_intr_mem;
            memset(nv2a_mem + 0x600100, 0, 4);  // clear committed copy
            hw->nv2a.update_irq();
        }

        hw->nv2a.tick_fifo(hw->ram, hw->ram_size);
    } else if (hw->ram) {
        // Fallback: no committed NV2A, just drain PFIFO
        hw->nv2a.tick_fifo(hw->ram, hw->ram_size);
    }

    // Increment KeTickCount approximately every ~1ms.
    // Each tick callback fires once per tick_period instructions (1024).
    // ~733 MHz / 1024 ≈ 716K ticks/s → 716 ticks per ms.
    if (hw->ram && ++hw->tick_accum >= 716) {
        hw->tick_accum = 0;
        // KDATA_BASE (0x81000) + offset 0 = KeTickCount
        constexpr uint32_t KE_TICK_ADDR = 0x81000;
        uint32_t tick = 0;
        memcpy(&tick, hw->ram + KE_TICK_ADDR, 4);
        ++tick;
        memcpy(hw->ram + KE_TICK_ADDR, &tick, 4);
    }

    // Advance ACPI PM Timer (~3.58 ticks per µs callback).
    hw->acpi.tick();
}

// NV2A IRQ callback: assert/deassert PIC IRQ 3 (PCI INTA# routing on Xbox).
static void nv2a_irq_callback(void* user, bool level) {
    auto* pic = static_cast<PicPair*>(user);
    static int irq_diag = 0;
    if (level && irq_diag < 5) { irq_diag++; fprintf(stderr, "[diag] NV2A IRQ assert (PIC IRQ 3) count=%d imr=0x%02X isr=0x%02X irr=0x%02X\n",
        irq_diag, pic->master.imr, pic->master.isr, pic->master.irr); }
    if (level)
        pic->raise_irq(3);
    else
        pic->lower_irq(3);
}

inline XboxHardware* xbox_setup(Executor& exec) {
    auto* hw = new XboxHardware();

    hw->pci.init_xbox_devices();
    exec.init(&hw->mmio);

    hw->ram = exec.ram;
    hw->ram_size = GUEST_RAM_SIZE;

    hw->mmio.add(RAM_MIRROR_BASE, RAM_MIRROR_SIZE,
                 ram_mirror_read, ram_mirror_write, exec.ram);
    hw->mmio.add(FLASH_BASE, FLASH_SIZE,
                 flash_read, flash_write, &hw->flash);
    hw->mmio.add(NV2A_BASE, NV2A_SIZE,
                 nv2a_read, nv2a_write, &hw->nv2a);

    // Wire PFIFO → PGRAPH: push buffer methods dispatch to pgraph state shadow.
    hw->nv2a.method_handler = pgraph_method_handler;
    hw->nv2a.method_user    = &hw->pgraph;
    hw->nv2a.pgraph_ptr     = &hw->pgraph;

    // Wire PFIFO notification → NV2A thread.
    hw->nv2a.fifo_notify      = nv2a_thread_notify;
    hw->nv2a.fifo_notify_user = &hw->nv2a_thread;

    // Wire NV2A PCI INTA# → PIC IRQ 3.
    hw->nv2a.irq_callback = nv2a_irq_callback;
    hw->nv2a.irq_user     = &hw->pic;

    // Wire PRAMIN to the Xbox instance memory pages in guest RAM.
    // On Xbox, NV2A instance memory lives at physical 0x03FE0000 (16 pages).
    // The D3D runtime writes RAMFC/DMA objects here via normal RAM access,
    // and MmClaimGpuInstanceMemory may shrink it later.  Wire early so
    // PRAMIN reads see the correct RAMFC context from the start.
    hw->nv2a.pramin_ram      = exec.ram;
    hw->nv2a.pramin_ram_base = 0x03FE0000;
    hw->nv2a.pramin_ram_size = 16 * 4096;  // 64 KB

    // If the 4GB fastmem window committed NV2A pages, wire the pointer so
    // update_irq() can sync computed registers to fastmem for direct reads.
    if (exec.nv2a_committed_) {
        hw->nv2a.fastmem_nv2a = exec.ram + 0xFD000000u;
    }

    hw->mmio.add(APU_BASE, APU_SIZE,
                 apu_read, apu_write, &hw->apu);
    hw->mmio.add(IOAPIC_BASE, IOAPIC_SIZE,
                 ioapic_read, ioapic_write, &hw->ioapic);
    hw->usb0.init(2);
    hw->usb1.init(2);
    hw->mmio.add(USB0_BASE, USB0_SIZE,
                 ohci_read, ohci_write, &hw->usb0);
    hw->mmio.add(USB1_BASE, USB1_SIZE,
                 ohci_read, ohci_write, &hw->usb1);
    hw->mmio.add(BIOS_BASE, BIOS_SIZE,
                 flash_read, flash_write, &hw->flash);

    exec.register_io(0x20, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0x21, pic_master_read, pic_master_write, &hw->pic);
    exec.register_io(0xA0, pic_slave_read, pic_slave_write, &hw->pic);
    exec.register_io(0xA1, pic_slave_read, pic_slave_write, &hw->pic);

    exec.irq_check = pic_irq_check;
    exec.irq_ack   = pic_irq_ack;
    exec.irq_user  = &hw->pic;

    exec.register_io(0x40, pit_io_read, pit_io_write, &hw->pit);
    exec.register_io(0x41, pit_io_read, pit_io_write, &hw->pit);
    exec.register_io(0x42, pit_io_read, pit_io_write, &hw->pit);
    exec.register_io(0x43, pit_io_read, pit_io_write, &hw->pit);

    hw->pit.pic  = &hw->pic;

    hw->cmos.pic = &hw->pic;
    hw->cmos.init();
    exec.register_io(0x70, cmos_io_read, cmos_io_write, &hw->cmos);
    exec.register_io(0x71, cmos_io_read, cmos_io_write, &hw->cmos);
    exec.register_io(0x4D0, elcr_io_read, elcr_io_write, &hw->cmos);
    exec.register_io(0x4D1, elcr_io_read, elcr_io_write, &hw->cmos);

    exec.tick_fn     = hw_tick_callback;
    exec.tick_user   = hw;
    exec.tick_period = 1024;

    exec.register_io(0x61, sysctl_read, sysctl_write, &hw->misc);
    exec.register_io(0x80, post_code_read, post_code_write, &hw->misc);
    exec.register_io(0xE9, debug_console_read, debug_console_write);
    exec.register_io(0xCF8, pci_io_read_cf8, pci_io_write_cf8, &hw->pci);
    exec.register_io(0xCFC, pci_io_read_cfc, pci_io_write_cfc, &hw->pci);

    for (uint16_t p = 0xC000; p <= 0xC00F; p++)
        exec.register_io(p, smbus_io_read, smbus_io_write, &hw->smbus);
    hw->smbus.pic = &hw->pic;

    for (uint16_t p = 0x1F0; p <= 0x1F7; p++)
        exec.register_io(p, ide_io_read, ide_io_write, &hw->ide);
    exec.register_io(0x3F6, ide_io_read, ide_io_write, &hw->ide);

    for (uint16_t p = 0x170; p <= 0x177; p++)
        exec.register_io(p, ide_io_read, ide_io_write, &hw->ide);
    exec.register_io(0x376, ide_io_read, ide_io_write, &hw->ide);

    // IDE Bus Master DMA (BAR4 = 0xFF60, 16 ports)
    for (uint16_t p = 0xFF60; p <= 0xFF6F; p++)
        exec.register_io(p, ide_bm_read, ide_bm_write, &hw->ide);

    // ACPI PM1a event registers (0x8000-0x8001)
    exec.register_io(0x8000, acpi_pm1a_read, acpi_pm1a_write, &hw->acpi);
    exec.register_io(0x8001, acpi_pm1a_read, acpi_pm1a_write, &hw->acpi);
    // ACPI PM Timer (0x8008, 32-bit 3.579545 MHz counter)
    exec.register_io(0x8008, acpi_pmtmr_read, nullptr, &hw->acpi);
    // ACPI GPE0 block (0x80C0-0x80C1): status/enable registers
    exec.register_io(0x80C0, acpi_gpe0_read, acpi_gpe0_write, &hw->acpi);
    exec.register_io(0x80C1, acpi_gpe0_read, acpi_gpe0_write, &hw->acpi);

    // NV2A PFIFO: The GPU compute shader is the sole method dispatcher.
    // The NV2A thread is disabled — it would race with the compute shader
    // by advancing DMA_GET before the shader can see push buffer data.
    // Guest-visible DMA_GET is maintained by reading back from the GPU buffer.
    // hw->nv2a_thread.start(&hw->nv2a, hw->ram, hw->ram_size);

    return hw;
}

} // namespace xbox
