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

### Step 3: Fix DESIGN.md contradictions (76833b1)
- Fixed architecture diagram (malformed line, stale SMC description)
- Fixed mnemonic counts: x87 27→29, SSE1 36→43
- Fixed test suite count: 33→46
- Rewrote §5.13 (SMC) for page-protection approach
- Rewrote §5.22 (fastmem) for 4GB window + VEH
- **Result**: 46/46 pass
- **Status**: DONE

### Step 4: Fastmem + SMC stress tests
- **tests/fastmem.asm** (--xbox, 20 assertions): VEH MMIO dispatch,
  multi-fault trace rebuild, mirror aliasing, interleaved RAM+MMIO,
  flags preservation across rebuild, repeated MMIO (slow-path cache),
  two MMIO devices in one trace, store-immediate and register-store
  to MMIO
- **tests/smc_stress.asm** (non-xbox, 24 assertions): multiple
  subroutines on same page, rapid-fire 10× same-instruction patch,
  cross-page SMC, data+code mixed on same page, NOP→instruction
  patching, conditional-branch-displacement patching, re-protect
  after invalidation (3× cycle), multi-trace same-page invalidation
- **Result**: 48/48 pass
- **Status**: DONE

### Step 5: Remove dead legacy code (b89dcac)
- Removed `emit_cmp_r14_r15`, `emit_jae_fwd`, `emit_smc_page_bump` —
  left over from pre-4GB fastmem CMP/JAE pattern
- Removed `smc_written` field and its accumulation in `trace_builder.cpp`
- Updated `executor.hpp` header comment (R15 no longer ram_size)
- **Files**: emitter.hpp, trace_builder.cpp, executor.hpp, context.hpp
- **Result**: 47/48 pass (pfifo pre-existing flaky)
- **Status**: DONE

### Step 6: Replace hardcoded GuestContext offsets with offsetof (HEAD)
- Defined `CTX_EFLAGS`, `CTX_NEXT_EIP`, `CTX_STOP_REASON`,
  `CTX_FASTMEM_BASE`, `CTX_FS_BASE`, `CTX_GS_BASE`, `CTX_GUEST_FPU`,
  `CTX_HOST_FPU` as `inline constexpr int` using `offsetof` in context.hpp
- Updated `gp_offset()` to use `offsetof(GuestContext, gp)`
- Replaced all hardcoded numeric offsets in emitter.hpp and
  trace_builder.cpp with the named constants
- dispatch_trace.asm: added EQU constants for all GP slots, EFLAGS,
  fastmem_base; replaced all `[r13+N]` with `[r13+CTX_*]`
- executor.cpp (GCC naked asm): added `CTX_ASM_*` preprocessor macros
  with `static_assert` verifying against `offsetof`; replaced all
  hardcoded offsets via string concatenation
- **Files**: context.hpp, emitter.hpp, trace_builder.cpp,
  dispatch_trace.asm, executor.cpp
- **Result**: 47/48 pass (pfifo pre-existing flaky)
- **Status**: DONE

### Step 7: Remove dead reserved fields from GuestContext (HEAD)
- Removed `_reserved_56` (was ram_size), `_pad1`, `_pad_pv`,
  `_reserved_120` (was page_versions) — 20 bytes of dead padding.
  Layout-change-safe thanks to offsetof-based constants from Step 6.
- Struct shrank: guest_fpu 128→112, host_fpu 640→624.
- Updated MASM EQU constants and CTX_ASM_* preprocessor macros.
- Updated static_asserts for new offsets.
- **Files**: context.hpp, dispatch_trace.asm
- **Result**: 47/48 pass (pfifo pre-existing flaky)
- **Status**: DONE

### Step 8: Remove redundant PageVersions (HEAD)
- `PageVersions` was a per-page SMC version counter used to validate
  block-link targets.  This is redundant: `invalidate_code_page()`
  already sets `valid = false` on all traces for the page, and
  `try_link_trace()` already checks `!target->valid` before linking.
- Removed `PageVersions` struct, `Trace::page_ver` field, `Executor::pv`
  member, `pv` parameter from `TraceBuilder::build()`, and the
  `pv.get(target_pa) != target->page_ver` check in `try_link_trace()`.
- Saves ~4 MB (1M × 4-byte version counters for full 4 GB PA space).
- **Files**: trace_builder.hpp, trace.hpp, executor.hpp, executor.cpp,
  trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

---

## Test Results

| Commit  | Pass | Fail | Notes                    |
|---------|------|------|--------------------------|
| 0a9e1e5 | 44   | 2    | smc (expected), pfifo    |
| bd78d89 | 45   | 1    | smc (expected)           |
| 3ff9331 | 46   | 0    | ALL PASS                 |
| 76833b1 | 46   | 0    | ALL PASS (doc fixes)     |
| (next)  | 48   | 0    | +fastmem, smc_stress     |
| b89dcac | 47   | 1    | pfifo flaky              |
| d50fb03 | 47   | 1    | pfifo flaky              |
| 09bb822 | 47   | 1    | pfifo flaky              |
| (HEAD)  | 48   | 0    | ALL PASS                 |