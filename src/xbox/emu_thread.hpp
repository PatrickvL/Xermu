#pragma once
// ---------------------------------------------------------------------------
// emu_thread.hpp — Emulation host thread.
//
// Runs the guest CPU and HLE scheduler on a dedicated thread, decoupling
// emulation from the main (UI / rendering) thread.  The main thread can
// read shared state (RAM, NV2A regs, PGRAPH) at any time — aligned 32-bit
// loads/stores are naturally atomic on x86.
// ---------------------------------------------------------------------------

#include "hle/bootstrap.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace xbox {

struct EmuThread {
    // Owning pointers — set before start().
    XboxSystem*    sys       = nullptr;
    NboxkrnlState* nbox      = nullptr;       // only used in Nboxkrnl mode
    BootMode       boot_mode = BootMode::None;

    // --------------- Lifecycle ---------------

    void start(XboxSystem* s, NboxkrnlState* nb, BootMode mode) {
        sys       = s;
        nbox      = nb;
        boot_mode = mode;
        stop_flag.store(false, std::memory_order_relaxed);
        paused_flag.store(false, std::memory_order_relaxed);
        halted_flag.store(false, std::memory_order_relaxed);
        worker = std::thread([this] { run(); });
    }

    void stop() {
        stop_flag.store(true, std::memory_order_release);
        wake();
        if (worker.joinable())
            worker.join();
    }

    ~EmuThread() { stop(); }

    // --------------- Control ---------------

    /// Pause emulation (the thread spins down to a CV wait).
    void pause()  { paused_flag.store(true,  std::memory_order_release); }

    /// Resume from pause.
    void resume() { paused_flag.store(false, std::memory_order_release); wake(); }

    /// True if the guest CPU halted (fatal or HLT with no more work).
    bool halted() const { return halted_flag.load(std::memory_order_acquire); }

    /// True if the thread is alive and not paused.
    bool running() const {
        return worker.joinable() &&
               !stop_flag.load(std::memory_order_acquire) &&
               !paused_flag.load(std::memory_order_acquire) &&
               !halted_flag.load(std::memory_order_acquire);
    }

    /// Snapshot: last halt EIP (valid when halted() == true).
    uint32_t halt_eip() const { return halt_eip_.load(std::memory_order_relaxed); }
    uint32_t halt_eax() const { return halt_eax_.load(std::memory_order_relaxed); }

private:
    std::thread             worker;
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       stop_flag{false};
    std::atomic<bool>       paused_flag{false};
    std::atomic<bool>       halted_flag{false};
    std::atomic<uint32_t>   halt_eip_{0};
    std::atomic<uint32_t>   halt_eax_{0};

    void wake() { cv.notify_one(); }

    void run() {
        // Steps per iteration.  Large enough for meaningful progress,
        // small enough that pause/stop respond promptly.
        constexpr uint32_t STEPS_PER_ITER = 2'000'000;

        while (!stop_flag.load(std::memory_order_acquire)) {
            // Handle pause.
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this] {
                    return stop_flag.load(std::memory_order_acquire) ||
                           !paused_flag.load(std::memory_order_acquire);
                });
            }
            if (stop_flag.load(std::memory_order_acquire)) break;
            if (!sys || !sys->running) {
                // Nothing to do — sleep briefly and re-check.
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }

            if (boot_mode == BootMode::Nboxkrnl) {
                sys->exec->ctx.halted = false;
                sys->exec->run(sys->exec->ctx.eip, STEPS_PER_ITER);

                if (sys->exec->ctx.halted) {
                    halt_eip_.store(sys->exec->ctx.eip, std::memory_order_relaxed);
                    halt_eax_.store(sys->exec->ctx.gp[GP_EAX], std::memory_order_relaxed);
                    halted_flag.store(true, std::memory_order_release);
                    sys->running = false;
                    break;
                }
            } else {
                // HLE / LLE-kernel / LLE modes use run_step.
                if (!run_step(*sys, STEPS_PER_ITER)) {
                    halt_eip_.store(sys->exec->ctx.eip, std::memory_order_relaxed);
                    halt_eax_.store(sys->exec->ctx.gp[GP_EAX], std::memory_order_relaxed);
                    halted_flag.store(true, std::memory_order_release);
                    sys->running = false;
                    break;
                }
            }
        }
    }
};

} // namespace xbox
