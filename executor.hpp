#pragma once
#include "context.hpp"
#include "code_cache.hpp"
#include "trace.hpp"
#include "trace_builder.hpp"
#include "platform.hpp"
#include <cstdint>
#include <cstddef>

// Maximum guest RAM for this rudimentary executor: 128 MB.
// The Xbox had 64 MB retail / 128 MB devkit.
static constexpr uint32_t GUEST_RAM_SIZE = 128u * 1024u * 1024u;

struct XboxExecutor {
    GuestContext   ctx {};
    uint8_t*       ram  = nullptr;   // guest physical RAM [0 .. GUEST_RAM_SIZE)

    CodeCache      cc;
    TraceCache     tcache;
    TraceArena     arena;
    TraceBuilder   builder;
    PageVersions   pv;

    // -----------------------------------------------------------------------
    bool init(MmioMap* mmio = nullptr);
    void destroy();

    // Load `size` bytes at guest PA `pa` from the host buffer `src`.
    void load_guest(uint32_t pa, const void* src, size_t size);

    // Run the executor loop until ctx.virtual_if is cleared or `max_steps`
    // traces have been dispatched (0 = unlimited).
    void run(uint32_t entry_eip, uint64_t max_steps = 0);

    // -----------------------------------------------------------------------
    // Privileged-instruction thunks (called from JIT via UD2 trap or directly).
    void handle_privileged();
};

// ---------------------------------------------------------------------------
// Assembly trampoline: load guest registers, call trace, save back.
//
// Calling convention (System V AMD64):
//   arg0 (RDI) = GuestContext*
//   arg1 (RSI) = void*  (host_code pointer into code cache)
//
// Windows x64 differs (args in RCX, RDX; RDI/RSI are callee-saved).
// A MASM implementation with the same logic is needed for MSVC builds.
// ---------------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
extern "C" void dispatch_trace(GuestContext* ctx, void* host_code);
#else
#  error "dispatch_trace() requires GCC or Clang. Provide a MASM .asm for MSVC."
#endif
