#pragma once
// ---------------------------------------------------------------------------
// nv2a_thread.hpp — NV2A PFIFO processing thread.
//
// Decouples push buffer parsing from the guest CPU tick callback.  The real
// NV2A's PFIFO is an independent DMA engine that runs in parallel with the
// CPU — this thread mirrors that architecture.  The guest CPU's only
// interaction is writing DMA_PUT; the thread wakes up, parses the push
// buffer, and updates DMA_GET independently.
//
// Synchronisation: the DMA_PUT write (via nv2a_write MMIO handler) signals
// a condition variable.  The thread drains the push buffer until GET == PUT,
// then goes back to sleep.  On x86, aligned uint32_t loads/stores are
// naturally atomic, and the CV provides happens-before ordering.
// ---------------------------------------------------------------------------

#include "nv2a.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace xbox {

struct Nv2aThread {
    Nv2aState*     nv2a     = nullptr;
    uint8_t*       ram      = nullptr;
    uint32_t       ram_size = 0;

    void start(Nv2aState* nv, uint8_t* r, uint32_t rs) {
        nv2a     = nv;
        ram      = r;
        ram_size = rs;
        stop_flag.store(false, std::memory_order_relaxed);
        worker = std::thread([this] { run(); });
    }

    // Signal the thread that PFIFO registers have been updated.
    // Called from nv2a_write on DMA_PUT / DMA_PUSH / PUSH0 / DMA_GET writes.
    void notify() { cv.notify_one(); }

    void stop() {
        stop_flag.store(true, std::memory_order_release);
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }

    ~Nv2aThread() { stop(); }

private:
    std::thread             worker;
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       stop_flag{false};

    bool needs_processing() const {
        return (nv2a->pfifo_regs[pfifo::CACHE1_DMA_PUSH / 4] & 1) &&
               (nv2a->pfifo_regs[pfifo::CACHE1_PUSH0 / 4] & 1) &&
               (nv2a->pfifo_regs[pfifo::CACHE1_DMA_GET / 4] !=
                nv2a->pfifo_regs[pfifo::CACHE1_DMA_PUT / 4]);
    }

    void run() {
        std::unique_lock<std::mutex> lock(mtx);
        while (!stop_flag.load(std::memory_order_acquire)) {
            cv.wait(lock, [this] {
                return stop_flag.load(std::memory_order_acquire) ||
                       needs_processing();
            });
            if (stop_flag.load(std::memory_order_acquire)) break;

            // Release lock while processing — allows notify() to be called
            // concurrently without blocking the guest CPU's MMIO writes.
            lock.unlock();

            // Drain the push buffer.  tick_fifo processes up to
            // MAX_DWORDS_PER_TICK dwords per call; loop until fully drained.
            while (needs_processing() &&
                   !stop_flag.load(std::memory_order_relaxed)) {
                nv2a->tick_fifo(ram, ram_size);
            }

            lock.lock();
        }
    }
};

// Static callback for wiring to Nv2aState::fifo_notify.
static void nv2a_thread_notify(void* user) {
    static_cast<Nv2aThread*>(user)->notify();
}

} // namespace xbox
