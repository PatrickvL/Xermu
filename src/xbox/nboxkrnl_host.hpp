#pragma once
// ---------------------------------------------------------------------------
// nboxkrnl_host.hpp — Host-side I/O port handlers for nboxkrnl communication.
//
// nboxkrnl (the guest kernel) uses custom I/O ports 0x200–0x210 to talk to
// the host emulator.  This file implements the read/write callbacks for those
// ports: debug output, timing, machine type, abort, XBE path, ACPI timer,
// and file I/O packet submission/query (stubs until nboxkrnl_io.hpp).
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace nboxkrnl {

// I/O port assignments (must match nboxkrnl kernel.hpp).
enum HostPort : uint16_t {
    PORT_DBG_STR            = 0x200,
    PORT_MACHINE_TYPE       = 0x201,
    PORT_ABORT              = 0x202,
    PORT_CLOCK_INC_LOW      = 0x203,
    PORT_CLOCK_INC_HIGH     = 0x204,
    PORT_BOOT_TIME_MS       = 0x205,
    PORT_IO_START           = 0x206,
    PORT_IO_RETRY           = 0x207,
    PORT_IO_QUERY           = 0x208,
    // 0x209 unused
    PORT_IO_CHECK_ENQUEUE   = 0x20A,
    // 0x20B–0x20C unused
    PORT_XBE_PATH_LENGTH    = 0x20D,
    PORT_XBE_PATH_ADDR      = 0x20E,
    PORT_ACPI_TIME_LOW      = 0x20F,
    PORT_ACPI_TIME_HIGH     = 0x210,

    PORT_FIRST              = 0x200,
    PORT_LAST               = 0x210,
};

// Console type constants (same as nboxkrnl ConsoleType enum).
enum ConsoleType : uint32_t {
    CONSOLE_XBOX    = 0,
    CONSOLE_CHIHIRO = 1,
    CONSOLE_DEVKIT  = 2,
};

// ---------------------------------------------------------------------------
// Host state — timing, XBE path, pointer to executor.
// ---------------------------------------------------------------------------

struct HostState {
    // Timing state.
    std::chrono::steady_clock::time_point boot_time;
    uint64_t last_clock_us       = 0;
    uint64_t lost_clock_increment = 0;
    uint64_t cached_clock_increment = 0;
    uint64_t cached_acpi_time    = 0;

    // XBE launch path in Xbox format, e.g. "\Device\CdRom0\default.xbe".
    std::string xbe_path;

    // File I/O state (set to true when pending packets exist).
    bool pending_io_packets = false;

    // Back-pointer to the executor (for read/write_guest_block).
    Executor* exec = nullptr;
};

// ---------------------------------------------------------------------------
// Clock increment calculation (matches nxbx kernel.cpp).
//
// The kernel reads ports 0x203/0x204/0x205 in quick succession with
// interrupts disabled.  Port 0x203 computes the new increment; 0x204 and
// 0x205 return the cached values from that computation.
// ---------------------------------------------------------------------------

static inline uint64_t calculate_clock_increment(HostState& s) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    uint64_t curr_us = duration_cast<microseconds>(now - s.boot_time).count();
    uint64_t elapsed_us = curr_us - s.last_clock_us;
    s.last_clock_us = curr_us;

    // Convert to 100 ns units.
    uint64_t elapsed_increment = elapsed_us * 10;
    s.lost_clock_increment += elapsed_increment;

    // Floor to nearest multiple of 10000 (= 1 ms clock tick).
    uint64_t actual = (s.lost_clock_increment / 10000) * 10000;
    s.lost_clock_increment -= actual;
    return actual;
}

// ---------------------------------------------------------------------------
// I/O port read callback.
// ---------------------------------------------------------------------------

static uint32_t nboxkrnl_host_read(uint16_t port, unsigned /*size*/, void* user) {
    auto& s = *static_cast<HostState*>(user);

    switch (port) {
    case PORT_MACHINE_TYPE:
        return CONSOLE_XBOX;

    case PORT_CLOCK_INC_LOW:
        // First port read: compute the increment and cache it.
        s.cached_clock_increment = calculate_clock_increment(s);
        return static_cast<uint32_t>(s.cached_clock_increment);

    case PORT_CLOCK_INC_HIGH:
        return static_cast<uint32_t>(s.cached_clock_increment >> 32);

    case PORT_BOOT_TIME_MS: {
        using namespace std::chrono;
        auto now = steady_clock::now();
        uint64_t us = duration_cast<microseconds>(now - s.boot_time).count();
        return static_cast<uint32_t>(us / 1000);
    }

    case PORT_IO_CHECK_ENQUEUE:
        return s.pending_io_packets ? 1u : 0u;

    case PORT_XBE_PATH_LENGTH:
        return static_cast<uint32_t>(s.xbe_path.size());

    case PORT_ACPI_TIME_LOW: {
        // Compute ACPI time (3.579545 MHz counter) and cache it.
        using namespace std::chrono;
        auto now = steady_clock::now();
        uint64_t us = duration_cast<microseconds>(now - s.boot_time).count();
        // 3.579545 ticks per microsecond.
        s.cached_acpi_time = static_cast<uint64_t>(us * 3.579545);
        return static_cast<uint32_t>(s.cached_acpi_time);
    }

    case PORT_ACPI_TIME_HIGH:
        return static_cast<uint32_t>(s.cached_acpi_time >> 32);

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// I/O port write callback.
// ---------------------------------------------------------------------------

static void nboxkrnl_host_write(uint16_t port, uint32_t val, unsigned /*size*/, void* user) {
    auto& s = *static_cast<HostState*>(user);

    switch (port) {
    case PORT_DBG_STR: {
        // Read up to 512 bytes of debug string from guest VA.
        char buf[512];
        memset(buf, 0, sizeof(buf));
        s.exec->read_guest_block(val, 512, buf);
        buf[511] = '\0';
        fprintf(stderr, "[nboxkrnl] %s", buf);
        break;
    }

    case PORT_ABORT:
        fprintf(stderr, "[nboxkrnl] ABORT requested\n");
        s.exec->ctx.halted = true;
        break;

    case PORT_IO_START:
        // TODO (M3): submit_io_packet(s, val);
        fprintf(stderr, "[nboxkrnl] IO_START: packet at VA 0x%08X (stub)\n", val);
        break;

    case PORT_IO_RETRY:
        // TODO (M3): flush_pending_packets(s);
        break;

    case PORT_IO_QUERY:
        // TODO (M3): query_io_packet(s, val);
        break;

    case PORT_XBE_PATH_ADDR:
        // Write the XBE path string to the guest VA.
        if (!s.xbe_path.empty()) {
            s.exec->write_guest_block(val,
                static_cast<uint32_t>(s.xbe_path.size()),
                s.xbe_path.c_str());
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Register all nboxkrnl host ports on an executor.
// ---------------------------------------------------------------------------

inline void register_host_ports(Executor& exec, HostState& state) {
    state.exec = &exec;
    state.boot_time = std::chrono::steady_clock::now();

    for (uint16_t p = PORT_FIRST; p <= PORT_LAST; ++p) {
        exec.register_io(p, nboxkrnl_host_read, nboxkrnl_host_write, &state);
    }
}

} // namespace nboxkrnl
