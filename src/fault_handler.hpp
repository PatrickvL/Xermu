#pragma once
// ---------------------------------------------------------------------------
// VEH fault handler declarations for 4 GB fastmem window.
//
// The actual handler is defined in executor.cpp (which has full Executor access).
// This header provides the global executor pointer and registration helpers.
// ---------------------------------------------------------------------------

#include <cstdint>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// Forward-declare Executor to avoid circular include.
struct Executor;

// Global pointer set by Executor::init(), cleared by destroy().
// VEH is process-global, so only one Executor can be active at a time.
inline Executor* g_active_executor = nullptr;

#ifdef _WIN32
// The VEH callback — defined in executor.cpp.
LONG CALLBACK fastmem_veh_handler(EXCEPTION_POINTERS* ep);
#endif

