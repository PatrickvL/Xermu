#pragma once
// Dual 8259A PIC — master (0x20-0x21), slave (0xA0-0xA1).
#include <cstdint>

namespace xbox {

struct Pic8259 {
    uint8_t irr         = 0;
    uint8_t imr         = 0xFF;
    uint8_t isr         = 0;
    uint8_t vector_base = 0;
    uint8_t icw3        = 0;
    bool    icw4_needed = false;
    bool    auto_eoi    = false;
    bool    read_isr    = false;
    int     init_step   = 0;
    bool    single_mode = false;

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
            if (val & 0x10) {
                init_step   = 1;
                irr = 0; isr = 0; imr = 0;
                icw4_needed = (val & 0x01) != 0;
                single_mode = (val & 0x02) != 0;
                read_isr    = false;
                return;
            }
            if ((val & 0x18) == 0x00) {
                int cmd = (val >> 5) & 7;
                if (cmd == 1) eoi();
                else if (cmd == 3) eoi_specific(val & 7);
                return;
            }
            if ((val & 0x18) == 0x08) {
                if (val & 0x02) read_isr = (val & 0x01) != 0;
                return;
            }
            return;
        }
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
        imr = val;
    }

    uint8_t read(int port_off) const {
        if (port_off == 0) return read_isr ? isr : irr;
        return imr;
    }
};

struct PicPair {
    Pic8259 master, slave;

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
            master.irr &= ~(1 << 2);
            master.isr |=  (1 << 2);
            if (master.auto_eoi) master.isr &= ~(1 << 2);
            return slave.ack();
        }
        return master.ack();
    }
};

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

static bool pic_irq_check(void* user) {
    return static_cast<PicPair*>(user)->has_pending();
}
static uint8_t pic_irq_ack(void* user) {
    return static_cast<PicPair*>(user)->ack();
}

} // namespace xbox
