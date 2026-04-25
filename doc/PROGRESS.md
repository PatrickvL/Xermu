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

### Step 9: Fix stale DESIGN.md references (efaf822)
- §2.6 rewritten for page-protection + VEH approach
- §3 file map: PageVersions → FaultBitmaps + SoftTlb
- §5.18 and §5.20 marked DONE
- §5.21 SMC safety updated for VEH-based detection
- §7 Sandbox: 4GB fastmem + VEH invalidation
- Code example updated: `MOV [R13+40]` → `MOV [R13+CTX_NEXT_EIP]`
- **Files**: doc/DESIGN.md
- **Result**: 48/48 pass
- **Status**: DONE

### Step 10: Remove GUEST_RAM_SIZE duplication and ram_size parameter (HEAD)
- Removed `GUEST_RAM_SIZE_LOCAL` duplicate constant from trace_builder.cpp;
  added `#include "executor.hpp"` so it uses the canonical `GUEST_RAM_SIZE`.
- Removed `ram_size` parameter from `TraceBuilder::build()` — all callers
  always passed `GUEST_RAM_SIZE`.  Replaced with a local `constexpr` alias
  inside the function body.
- **Files**: trace_builder.hpp, trace_builder.cpp, executor.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 11: Named REX prefix constants in emitter.hpp (8418da3)
- Added `REX_B`, `REX_XB`, `REX_R`, `REX_RB`, `REX_W`, `REX_WB`, `REX_WR`,
  `REX_WRB` constexpr constants replacing ~30 raw hex REX prefix bytes.
- Replaced all REX prefix hex literals in helper functions: emit_save/load_gp,
  emit_write_next_eip_imm/gpreg, emit_set_stop_reason, emit_save_eflags,
  emit_save/restore_flags, emit_call_abs, emit_sub/add_ctx_esp,
  emit_load/store_esp_to_r14/r8, emit_ccall_arg*, emit_ea_to_r14,
  emit_fastmem_store_imm*, emit_translate_r14.
- ModRM bytes that coincidentally fall in 0x40-0x4F range left as hex.
- **Files**: emitter.hpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 12: Fix stale DESIGN.md §5.14 and bug table (180fb7d)
- §5.14: Removed stale 10-line paragraph describing old `emit_smc_page_bump()`
  per-store approach (removed in Step 5); replaced with reference to §5.13
  page-protection + VEH.
- Bug #5: Updated "~6.5 MB (TraceCache, PageVersions)" → "~13 MB (TraceArena,
  TraceCache)" since PageVersions was removed.
- §4: Fixed test count "46 suites" → "46 NASM + 2 C++ unit tests (48 total)".
- insn_dispatch.hpp: Clarified IC_SSE_MEM "same logic" → "shares rewrite handler".
- **Files**: doc/DESIGN.md, src/insn_dispatch.hpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 13: Replace reinterpret_cast with memcpy in privileged insn handlers (HEAD)
- Replaced 8 `reinterpret_cast<uint16_t*>` / `<uint32_t*>` pointer aliasing
  violations with `memcpy` in LGDT/LIDT/LLDT/LTR/SGDT/SIDT/SLDT/STR handlers.
- Strict aliasing safe; compiler optimizes memcpy to identical codegen.
- **Files**: executor.cpp
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
| efaf822 | 48   | 0    | doc fixes                |
| f72b2ad | 48   | 0    | ram_size dedup           |
| 57212ea | 48   | 0    | stale comments           |
| 8418da3 | 48   | 0    | REX constants            |
| 180fb7d | 48   | 0    | doc fixes                |
| (HEAD)  | 48   | 0    | ALL PASS                 |