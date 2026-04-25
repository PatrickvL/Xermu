# Fastmem-4GB Branch — Progress Tracker

**Branch**: `fastmem-4gb`
**Base**: `657ca77` (main)
**Started**: 2026-04-24

---

## Completed Steps

### Step 0: 4GB Fastmem + VEH Implementation (0a9e1e5)
- Reserved 4 GB virtual memory window covering full 32-bit guest PA space
- Replaced 10-instruction CMP/JAE/slow-path pattern with single `[R12+R14]`
- VEH handler: fault → set bitmap → rebuild trace → redirect
- Freed R15 (no longer holds ram_size)
- 45/46 tests pass; `smc` expected failure (page versions removed)
- **Files**: platform.hpp, emitter.hpp, trace_builder.cpp, trace.hpp,
  trace_builder.hpp, executor.hpp, executor.cpp, dispatch_trace.asm,
  fault_handler.hpp, code_cache.hpp, DESIGN.md

---

## Remaining Steps

### Step 1: Fix VEH restart-to-start bug — pfifo regression (bd78d89)
- **Problem**: VEH redirected RIP to start of rebuilt trace, replaying
  side-effecting MMIO writes (DMA_PUT before DMA_GET), causing PFIFO
  thread to double-count JUMP commands.
- **Fix**: After rebuilding, redirect RIP to the faulting instruction's
  position in the new trace via `lookup_host_offset()`.  Added
  `e.add_mem_site()` to all slow-path branches so rebuilt traces have
  offset mappings.
- **Files**: trace.hpp, trace_builder.cpp, executor.cpp
- **Result**: 45/46 pass (smc expected failure)
- **Status**: DONE

### Step 2: Fix SMC detection via page protection (3ff9331)
- **Problem**: Page-version bumps removed; `smc` test failed because
  self-modifying code wasn't detected.
- **Fix**: VirtualProtect marks code pages read-only after trace build.
  VEH catches write faults, unprotects, invalidates traces + outbound
  block links, bumps page version, clears fault bitmaps, continues.
  Also fixed: outbound links from invalidated traces not reset (caused
  stale block-link JMP to old trace code).
- **Files**: executor.hpp, executor.cpp
- **Result**: 46/46 pass (all tests green!)
- **Status**: DONE

### Step 3: Fix DESIGN.md contradictions
- **From audit**: duplicate section numbers, stale FPU offsets, test count
  mismatches, stale instruction status entries, missing test files, etc.
- **Status**: NOT STARTED

---

## Test Results

| Commit  | Pass | Fail | Notes                    |
|---------|------|------|--------------------------|
| 0a9e1e5 | 44   | 2    | smc (expected), pfifo    |
| bd78d89 | 45   | 1    | smc (expected)           || 3ff9331 | 46   | 0    | ALL PASS                 |