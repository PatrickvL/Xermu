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

    // Process NV2A PFIFO push buffer commands so DMA_GET advances.
    if (hw->ram)
        hw->nv2a.tick_fifo(hw->ram, hw->ram_size);

    // Increment KeTickCount approximately every ~1ms.
    // Each tick callback fires once per trace; ~1000 traces ≈ 1ms.
    if (hw->ram && ++hw->tick_accum >= 1000) {
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

    exec.tick_fn     = hw_tick_callback;
    exec.tick_user   = hw;
    exec.tick_period = 1;

    exec.register_io(0x61, sysctl_read, sysctl_write, &hw->misc);
    exec.register_io(0x80, post_code_read, post_code_write, &hw->misc);
    exec.register_io(0xE9, debug_console_read, debug_console_write);
    exec.register_io(0xCF8, pci_io_read_cf8, pci_io_write_cf8, &hw->pci);
    exec.register_io(0xCFC, pci_io_read_cfc, pci_io_write_cfc, &hw->pci);

    for (uint16_t p = 0xC000; p <= 0xC00E; p += 2)
        exec.register_io(p, smbus_io_read, smbus_io_write, &hw->smbus);

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

    // Start the NV2A PFIFO processing thread — must be last, after all
    // hardware wiring is complete.
    hw->nv2a_thread.start(&hw->nv2a, hw->ram, hw->ram_size);

    return hw;
}

} // namespace xbox
