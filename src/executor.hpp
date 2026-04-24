#pragma once
#include "context.hpp"
#include "code_cache.hpp"
#include "trace.hpp"
#include "trace_builder.hpp"
#include "platform.hpp"
#include <cstdint>
#include <cstddef>

// Maximum guest RAM: 128 MB (e.g. Xbox devkit had 128 MB, retail 64 MB).
static constexpr uint32_t GUEST_RAM_SIZE = 128u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// I/O port dispatch — callbacks for IN/OUT handled by the executor.
// ---------------------------------------------------------------------------

using IoReadFn  = uint32_t(*)(uint16_t port, unsigned size, void* user);
using IoWriteFn = void(*)(uint16_t port, uint32_t value, unsigned size, void* user);

struct IoPortEntry {
    uint16_t   port;
    IoReadFn   read;
    IoWriteFn  write;
    void*      user;
};

struct Executor {
    GuestContext   ctx {};
    uint8_t*       ram  = nullptr;   // guest physical RAM [0 .. GUEST_RAM_SIZE)

    CodeCache      cc;
    TraceCache     tcache;
    TraceArena     arena;
    TraceBuilder   builder;
    PageVersions   pv;

    // I/O port dispatch table (small fixed table — only a handful needed).
    static constexpr int MAX_IO_PORTS = 16;
    IoPortEntry io_ports[MAX_IO_PORTS] {};
    int         n_io_ports = 0;

    // Pending hardware IRQ bitmap (set by device callbacks, checked in run loop).
    uint32_t    pending_irq = 0;

    // -----------------------------------------------------------------------
    bool init(MmioMap* mmio = nullptr);
    void destroy();

    // Load `size` bytes at guest PA `pa` from the host buffer `src`.
    void load_guest(uint32_t pa, const void* src, size_t size);

    // Run the executor loop until halted or `max_steps`
    // traces have been dispatched (0 = unlimited).
    void run(uint32_t entry_eip, uint64_t max_steps = 0);

    // Register an I/O port handler (read and/or write callback).
    void register_io(uint16_t port, IoReadFn read, IoWriteFn write,
                     void* user = nullptr);
    uint32_t io_read(uint16_t port, unsigned size);
    void     io_write(uint16_t port, uint32_t val, unsigned size);

    // -----------------------------------------------------------------------
    // Privileged-instruction thunks (called from run loop on STOP_PRIVILEGED).
    void handle_privileged();

    // Deliver an interrupt/exception through the IDT.
    //   vector       — IDT vector number (0..255)
    //   return_eip   — EIP pushed onto guest stack (instruction to resume)
    //   has_error    — if true, push error_code after EFLAGS/CS/EIP frame
    //   error_code   — hardware error code (only if has_error == true)
    void deliver_interrupt(uint8_t vector, uint32_t return_eip,
                           bool has_error = false, uint32_t error_code = 0);

    // Raise a pending hardware IRQ line (checked at trace boundaries).
    void raise_irq(unsigned irq_line) { pending_irq |= (1u << irq_line); }
};

// ---------------------------------------------------------------------------
// Assembly trampoline: load guest registers, call trace, save back.
//
// GCC/Clang: naked function with inline asm in executor.cpp.
// MSVC:      MASM implementation in dispatch_trace.asm.
//
// System V AMD64: args in RDI, RSI.
// Windows x64:    args in RCX, RDX; RDI/RSI are callee-saved.
// ---------------------------------------------------------------------------

extern "C" void dispatch_trace(GuestContext* ctx, void* host_code);
