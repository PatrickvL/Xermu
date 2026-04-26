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
#include "xbox/pe_loader.hpp"
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
    std::string xbe_path;     // XBE file to load (HLE or LLE-kernel mode)
    std::string bios_path;    // BIOS image for LLE boot
    std::string mcpx_path;    // optional MCPX ROM for LLE boot
    std::string xiso_path;    // XISO image to mount as D:
    std::string kernel_path;  // xboxkrnl.exe for LLE-kernel mode

    bool is_hle()  const { return !xbe_path.empty() && kernel_path.empty(); }
    bool is_lle()  const { return !bios_path.empty(); }
    bool is_lle_kernel() const { return !xbe_path.empty() && !kernel_path.empty(); }
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
// boot_lle_kernel — load the real xboxkrnl.exe as a PE, then load an XBE
// and resolve its kernel thunks against the real kernel's export table.
// Ordinals not found in the kernel fall back to HLE stubs.
// Returns false on error; fills sys.entry_eip on success.
// ---------------------------------------------------------------------------
inline bool boot_lle_kernel(XboxSystem& sys, const BootConfig& cfg,
                            std::function<void(const char*)> log = nullptr)
{
    auto say = [&](const char* msg) { if (log) log(msg); else fprintf(stderr, "%s\n", msg); };

    sys.hw = xbox_setup(*sys.exec);

    // ---- Load xboxkrnl.exe into guest RAM ----
    pe::LoadResult krnl = pe::load_pe(sys.exec->ram, GUEST_RAM_SIZE, cfg.kernel_path.c_str());
    if (!krnl.ok) {
        say("[boot] Failed to load xboxkrnl.exe");
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "[boot] Kernel loaded: base=0x%08X entry=0x%08X size=0x%08X sections=%u",
             krnl.image_base, krnl.entry_point, krnl.image_size, krnl.num_sections);
    say(msg);

    // ---- Load XBE file ----
    auto xbe_data = read_file_to_vec(cfg.xbe_path);
    if (xbe_data.empty()) {
        say("[boot] Failed to read XBE file");
        return false;
    }

    // Copy raw XBE into RAM for load_xbe to parse
    uint32_t load_pa = 0x1000;
    sys.exec->load_guest(load_pa, xbe_data.data(), xbe_data.size());

    // Parse XBE (this also writes HLE stubs and resolves thunks to HLE)
    sys.xbe_info = xbe::load_xbe(sys.exec->ram, xbe_data.data(), xbe_data.size());
    if (!sys.xbe_info.valid) {
        say("[boot] XBE parsing failed");
        return false;
    }

    snprintf(msg, sizeof(msg), "[boot] XBE entry=0x%08X thunks=0x%08X sections=%u",
             sys.xbe_info.entry_point, sys.xbe_info.kernel_thunk_va, sys.xbe_info.num_sections);
    say(msg);

    // ---- Re-resolve kernel thunks against real kernel exports ----
    // load_xbe already resolved all thunks to HLE stubs.  Now re-patch
    // each one: if the real kernel exports that ordinal, use the real VA;
    // otherwise keep the HLE stub (fallback).
    uint32_t thunk_va = sys.xbe_info.kernel_thunk_va;
    uint32_t resolved_lle = 0, fallback_hle = 0;
    for (uint32_t off = thunk_va; off + 4 <= GUEST_RAM_SIZE; off += 4) {
        uint32_t current_va;
        memcpy(&current_va, sys.exec->ram + off, 4);
        if (current_va == 0) break;

        // Determine which ordinal this thunk was for by checking if it
        // points into the HLE stub region
        if (current_va >= xbe::HLE_STUB_BASE &&
            current_va < xbe::HLE_STUB_BASE + xbe::MAX_KERNEL_ORDINALS * xbe::HLE_STUB_SIZE) {
            uint32_t ordinal = (current_va - xbe::HLE_STUB_BASE) / xbe::HLE_STUB_SIZE;

            // Also check if it's a data export (those point to KDATA_BASE area)
            // — skip re-resolving those, they're fine as-is
            uint32_t data_va = xbe::kernel_data_addr(ordinal);
            if (data_va) continue;  // data export — keep kernel data pointer

            // Try to resolve from real kernel
            uint32_t real_va = pe::resolve_export_by_ordinal(
                sys.exec->ram, krnl.image_base, ordinal);
            if (real_va != 0) {
                memcpy(sys.exec->ram + off, &real_va, 4);
                ++resolved_lle;
            } else {
                ++fallback_hle;
            }
        }
        // If it points to KDATA area, it's a data export — leave it
    }

    snprintf(msg, sizeof(msg), "[boot] Thunks: %u resolved to kernel, %u HLE fallback",
             resolved_lle, fallback_hle);
    say(msg);

    // ---- Stack and KPCR setup (same as HLE mode) ----
    constexpr uint32_t STACK_TOP = 0x00B0'0000u;
    constexpr uint32_t SENTINEL  = 0xFFFF'FFFFu;
    sys.exec->ctx.gp[GP_ESP] = STACK_TOP;
    memcpy(sys.exec->ram + STACK_TOP, &SENTINEL, 4);
    sys.exec->ctx.eflags = 0x0000'0202;

    constexpr uint32_t FS_BASE = 0x00D0'0000u;
    sys.exec->ctx.fs_base = FS_BASE;
    sys.exec->ctx.gs_base = 0;
    setup_kpcr(*sys.exec, FS_BASE, STACK_TOP);

    // ---- Install HLE handler for fallback stubs ----
    // HLE stubs are still present in RAM — if the guest calls an ordinal
    // that wasn't in the real kernel, INT 0x20 fires and we handle it.
    sys.hle_heap.reset();
    sys.hle_heap.set_xbe_path(cfg.xbe_path);

    if (!cfg.xiso_path.empty()) {
        size_t sep = cfg.xiso_path.find_last_of("/\\");
        std::string xiso_dir = (sep != std::string::npos)
            ? cfg.xiso_path.substr(0, sep) : ".";
        sys.hle_heap.mounts.push_back({"\\??\\D:", xiso_dir});
        sys.hle_heap.mounts.push_back({"\\Device\\CdRom0", xiso_dir});
    }

    sys.exec->hle_handler = xbe::default_hle_handler;
    sys.exec->hle_user    = &sys.hle_heap;

    // ---- Choose entry point ----
    // The real Xbox boot has the kernel run first (KiSystemStartup), which
    // then loads and calls the XBE.  For now, we start at the XBE entry
    // point since the kernel's init depends on hardware state we don't
    // fully emulate.  The thunks point into real kernel code, so when the
    // XBE calls kernel functions they execute natively.
    sys.entry_eip = sys.xbe_info.entry_point;
    sys.running = true;

    say("[boot] LLE-kernel mode ready — starting at XBE entry point");
    return true;
}

// ---------------------------------------------------------------------------
// run_step — run the executor for a batch of steps.  Returns true if still
// running (not halted).  Dispatches pending HLE threads non-blockingly:
// if a thread is pending after the main code halts, it gets set up for the
// *next* frame's run_step call (never blocks the UI thread).
//
// Wait/delay HLE stubs set halted=true as a "yield" — the guest is still
// alive but wants to give up its timeslice.  Only a halt at the sentinel
// EIP (0xFFFFFFFF) or the HalReturnToFirmware stub is a real stop.
// ---------------------------------------------------------------------------
inline bool run_step(XboxSystem& sys, uint32_t max_steps = 500'000) {
    if (!sys.running) return false;

    sys.exec->ctx.halted = false;
    sys.exec->ctx.stop_reason = STOP_NONE;
    sys.exec->run(sys.entry_eip, max_steps);

    // Check for error stops — these are always permanent
    uint32_t sr = sys.exec->ctx.stop_reason;
    if (sr == STOP_INVALID_OPCODE || sr == STOP_DIVIDE_ERROR) {
        sys.running = false;
        return false;
    }

    // Check if this is a permanent halt (sentinel or HalReturnToFirmware)
    uint32_t eip = sys.exec->ctx.eip;
    bool real_halt = sys.exec->ctx.halted &&
        (eip == 0xFFFFFFFF ||
         eip == xbe::hle_stub_addr(xbe::ORD_HalReturnToFirmware));

    // If permanently halted and there are pending threads, dispatch one
    // for the *next* frame (never block the UI thread).
    if (real_halt && !sys.hle_heap.pending_threads.empty()) {
        xbe::PendingThread t = sys.hle_heap.pending_threads.front();
        sys.hle_heap.pending_threads.erase(sys.hle_heap.pending_threads.begin());

        uint32_t esp = sys.exec->ctx.gp[GP_ESP];
        // Push start_context as argument
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &t.start_context, 4);
        // Push halt return address (will stop execution when thread returns)
        uint32_t halt_ret = xbe::hle_stub_addr(xbe::ORD_HalReturnToFirmware);
        esp -= 4;
        if (esp + 4 <= GUEST_RAM_SIZE)
            memcpy(sys.exec->ram + esp, &halt_ret, 4);
        sys.exec->ctx.gp[GP_ESP] = esp;

        // Set entry EIP for next frame — don't run it now
        sys.entry_eip = t.start_routine;
        return true;  // still running — will dispatch thread next frame
    }

    // Permanent halt with no pending threads → done
    if (real_halt) {
        sys.running = false;
        return false;
    }

    // Either max_steps exhausted or a yield-halt (wait/delay stub).
    // Continue from current EIP next frame.
    sys.entry_eip = sys.exec->ctx.eip;
    return true;
}

} // namespace xbox
