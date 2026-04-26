#pragma once
// ---------------------------------------------------------------------------
// bootstrap.hpp — Xbox system bootstrap (shared between GUI and test runner).
//
// Provides XBE loading, BIOS boot, HLE kernel setup, and KPCR initialisation.
// Used by both the interactive emulator (main.cpp) and the headless test
// runner (test_runner.cpp).
// ---------------------------------------------------------------------------

#include "cpu/executor.hpp"
#include "xbox/xbox.hpp"
#include "xbox/hle/hle_kernel.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <functional>

namespace xbox {

// ---------------------------------------------------------------------------
// BootConfig — describes what to boot and how.
// ---------------------------------------------------------------------------
struct BootConfig {
    std::string xbe_path;     // XBE file to load (HLE mode)
    std::string bios_path;    // BIOS image for LLE boot
    std::string mcpx_path;    // optional MCPX ROM for LLE boot
    std::string xiso_path;    // XISO image to mount as D:

    bool is_hle()  const { return !xbe_path.empty(); }
    bool is_lle()  const { return !bios_path.empty(); }
};

// ---------------------------------------------------------------------------
// XboxSystem — owns the executor, hardware, and HLE heap.
// ---------------------------------------------------------------------------
struct XboxSystem {
    std::unique_ptr<Executor> exec;
    XboxHardware*             hw       = nullptr;
    xbe::XbeHeap              hle_heap;
    xbe::XbeInfo              xbe_info {};
    bool                      running  = false;
    uint32_t                  entry_eip = 0;

    XboxSystem() : exec(std::make_unique<Executor>()) {}

    ~XboxSystem() { shutdown(); }

    // Non-copyable, movable
    XboxSystem(const XboxSystem&) = delete;
    XboxSystem& operator=(const XboxSystem&) = delete;
    XboxSystem(XboxSystem&&) = default;
    XboxSystem& operator=(XboxSystem&&) = default;

    void shutdown() {
        if (exec) exec->destroy();
        delete hw;
        hw = nullptr;
        running = false;
    }
};

// ---------------------------------------------------------------------------
// read_file_to_vec — load a host file into a byte vector.
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> read_file_to_vec(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f);
    return buf;
}

// ---------------------------------------------------------------------------
// setup_kpcr — initialise a minimal KPCR + KTHREAD at the given FS base.
// ---------------------------------------------------------------------------
inline void setup_kpcr(Executor& exec, uint32_t fs_base, uint32_t stack_top) {
    uint32_t kthread_base = fs_base + 0x1000;
    memset(exec.ram + fs_base, 0, 0x2000);

    // KPCR.NtTib.ExceptionList = -1
    uint32_t neg1 = 0xFFFFFFFF;
    memcpy(exec.ram + fs_base + 0x00, &neg1, 4);
    // KPCR.NtTib.StackBase
    memcpy(exec.ram + fs_base + 0x04, &stack_top, 4);
    // KPCR.NtTib.StackLimit
    uint32_t stack_limit = stack_top - 0x10000;
    memcpy(exec.ram + fs_base + 0x08, &stack_limit, 4);
    // KPCR.NtTib.Self = fs_base
    memcpy(exec.ram + fs_base + 0x18, &fs_base, 4);
    // KPCR.SelfPcr = fs_base
    memcpy(exec.ram + fs_base + 0x1C, &fs_base, 4);
    // KPCR.Prcb (offset 0x20) — points into KPCR
    uint32_t prcb = fs_base + 0x28;
    memcpy(exec.ram + fs_base + 0x20, &prcb, 4);
    // KPCR.CurrentThread (offset 0x28)
    memcpy(exec.ram + fs_base + 0x28, &kthread_base, 4);

    // KTHREAD.TlsData (offset 0x28)
    uint32_t tls_data = kthread_base + 0x200;
    memcpy(exec.ram + kthread_base + 0x28, &tls_data, 4);
    memset(exec.ram + kthread_base + 0x200, 0, 0x100);
}

// ---------------------------------------------------------------------------
// boot_hle — load an XBE and set up HLE stubs.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_hle(XboxSystem& sys, const BootConfig& cfg,
                     std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    // Load XBE file.
    auto xbe_data = read_file_to_vec(cfg.xbe_path);
    if (xbe_data.empty()) {
        say("[boot] Failed to read XBE file");
        return false;
    }

    // Copy raw file into RAM at the load PA for load_xbe to parse.
    uint32_t load_pa = 0x1000;
    sys.exec->load_guest(load_pa, xbe_data.data(), xbe_data.size());

    // Parse XBE headers and map sections.
    sys.xbe_info = xbe::load_xbe(sys.exec->ram, xbe_data.data(), xbe_data.size());
    if (!sys.xbe_info.valid) {
        say("[boot] XBE parsing failed");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[boot] XBE entry=0x%08X sections=%u",
             sys.xbe_info.entry_point, sys.xbe_info.num_sections);
    say(msg);

    // Stack at 11 MB (above HLE stubs + XBE image)
    constexpr uint32_t STACK_TOP = 0x00B0'0000u;
    constexpr uint32_t SENTINEL  = 0xFFFF'FFFFu;
    sys.exec->ctx.gp[GP_ESP] = STACK_TOP;
    memcpy(sys.exec->ram + STACK_TOP, &SENTINEL, 4);
    sys.exec->ctx.eflags = 0x0000'0202;

    // KPCR at 13 MB
    constexpr uint32_t FS_BASE = 0x00D0'0000u;
    sys.exec->ctx.fs_base = FS_BASE;
    sys.exec->ctx.gs_base = 0;
    setup_kpcr(*sys.exec, FS_BASE, STACK_TOP);

    // Write HLE stubs and install handler.
    xbe::write_hle_stubs(sys.exec->ram);
    sys.hle_heap.reset();
    sys.hle_heap.set_xbe_path(cfg.xbe_path);

    // If an XISO is provided, mount it as D:
    if (!cfg.xiso_path.empty()) {
        // Extract directory of the XISO for basic mount
        size_t sep = cfg.xiso_path.find_last_of("/\\");
        std::string xiso_dir = (sep != std::string::npos)
            ? cfg.xiso_path.substr(0, sep) : ".";
        sys.hle_heap.mounts.push_back({"\\??\\D:", xiso_dir});
        sys.hle_heap.mounts.push_back({"\\Device\\CdRom0", xiso_dir});
    }

    sys.exec->hle_handler = xbe::default_hle_handler;
    sys.exec->hle_user    = &sys.hle_heap;

    sys.entry_eip = sys.xbe_info.entry_point;
    sys.running = true;
    return true;
}

// ---------------------------------------------------------------------------
// boot_lle — load BIOS (+ optional MCPX) and enter via reset vector.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_lle(XboxSystem& sys, const BootConfig& cfg,
                     std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    if (!sys.hw->flash.load_bios(cfg.bios_path.c_str())) {
        say("[boot] Failed to load BIOS image");
        return false;
    }
    say("[boot] BIOS loaded into flash");

    if (!cfg.mcpx_path.empty()) {
        if (!sys.hw->flash.load_mcpx(cfg.mcpx_path.c_str())) {
            say("[boot] Failed to load MCPX ROM");
            return false;
        }
        say("[boot] MCPX ROM loaded");
    } else {
        // Patch init table to skip encrypted commands
        constexpr uint32_t INIT_TABLE_OFF = 0xC0000;
        sys.hw->flash.data[INIT_TABLE_OFF] = 0xEE;
    }

    // Real-mode prologue → protected mode
    if (!sys.exec->interpret_real_mode_boot()) {
        say("[boot] Real-mode boot failed");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[boot] PM entry EIP=0x%08X CR0=0x%08X",
             sys.exec->ctx.eip, sys.exec->ctx.cr0);
    say(msg);

    sys.entry_eip = sys.exec->ctx.eip;
    sys.running = true;
    return true;
}

// ---------------------------------------------------------------------------
// run_step — run the executor for a batch of steps.  Returns true if still
// running (not halted).  Dispatches pending HLE threads automatically.
// ---------------------------------------------------------------------------
inline bool run_step(XboxSystem& sys, uint32_t max_steps = 500'000) {
    if (!sys.running) return false;

    sys.exec->ctx.halted = false;
    sys.exec->ctx.stop_reason = STOP_NONE;
    sys.exec->run(sys.entry_eip, max_steps);

    // Dispatch pending HLE threads
    while (!sys.hle_heap.pending_threads.empty()) {
        xbe::PendingThread t = sys.hle_heap.pending_threads.front();
        sys.hle_heap.pending_threads.erase(sys.hle_heap.pending_threads.begin());

        sys.exec->ctx.halted = false;
        sys.exec->ctx.stop_reason = STOP_NONE;
        uint32_t esp = sys.exec->ctx.gp[GP_ESP];
        // Push context arg
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &t.start_context, 4);
        // Push halt return address
        uint32_t halt_ret = xbe::hle_stub_addr(xbe::ORD_HalReturnToFirmware);
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &halt_ret, 4);
        sys.exec->ctx.gp[GP_ESP] = esp;

        sys.exec->run(t.start_routine, max_steps);
    }

    if (sys.exec->ctx.halted) {
        sys.running = false;
        return false;
    }

    // Update entry EIP for next call (continue from where we stopped)
    sys.entry_eip = sys.exec->ctx.eip;
    return true;
}

} // namespace xbox
