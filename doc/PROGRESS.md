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

### Step 13: Replace reinterpret_cast with memcpy in privileged insn handlers (e39e04b)
- Replaced 8 `reinterpret_cast<uint16_t*>` / `<uint32_t*>` pointer aliasing
  violations with `memcpy` in LGDT/LIDT/LLDT/LTR/SGDT/SIDT/SLDT/STR handlers.
- Strict aliasing safe; compiler optimizes memcpy to identical codegen.
- **Files**: executor.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 14: Fix stale xbox device comments (106c906)
- flash.hpp: Clarified MCPX ROM comment ("currently unused, HLE mode only")
- ide.hpp: Clarified DATA register comment ("not stored here; PIO via helper")
- **Files**: src/xbox/flash.hpp, src/xbox/ide.hpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 15: Minor consistency fixes (885eba9)
- executor.cpp: `0xFFFFFFFF` → `0xFFFFFFFFu` (unsigned suffix consistency)
- executor.cpp: Fixed stale halt comment ("EIP == 0" → "EIP == 0xFFFFFFFF")
- trace_builder.cpp: 2× "VEH intercepts faults" → "VEH intercepts faults
  and sets bitmap" (match header comment)
- **Files**: executor.cpp, trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 16: Replace reinterpret_cast with memcpy in trace_builder.cpp (4b0fdd3)
- Replaced 5 more `reinterpret_cast<uint32_t*>` / `<uint16_t*>` aliasing
  violations in guest_read, guest_write, popfd_helper, read_guest_mem32,
  write_guest_mem32.
- **Files**: trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 17: Fix all stale CMP R14,R15 references (750a582)
- Removed all remaining references to the old pre-fastmem `CMP R14, R15; JAE`
  bounds-check pattern across the codebase.
- **Files**: README.md, src/insn_dispatch.hpp, src/trace_builder.cpp,
  src/main.cpp, doc/DESIGN.md
- **Result**: 48/48 pass
- **Status**: DONE

### Step 18: MSR named constants (c55ed9f)
- Added 14 `MSR_*` constants to context.hpp for SYSENTER, TSC, and MTRR MSR indices.
- Replaced all raw hex MSR indices in executor.cpp RDMSR/WRMSR handlers.
- Updated GuestContext field comments to reference constant names.
- **Files**: src/context.hpp, src/executor.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 19: PAGE_SIZE/PAGE_MASK named constants (86ef544)
- Added `GUEST_PAGE_SIZE` (0x1000) and `GUEST_PAGE_MASK` (0xFFFFF000) to executor.hpp.
- Replaced all raw page-size/mask hex literals in executor.cpp (load_guest,
  protect_code_page, invalidate_code_page, translate_va page walks) and
  trace_builder.cpp (translate_va_jit page walks).
- **Files**: src/executor.hpp, src/executor.cpp, src/trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 20: Fix flaky pfifo test — DMA pusher race condition (678128d)
- Root cause: Phases 2 and 3 wrote CACHE1_DMA_PUT before CACHE1_DMA_GET
  while the DMA pusher thread was still enabled. The thread could wake up
  between the two writes and process from the old GET through uninitialised
  memory, double-counting stats and causing assertion failures.
- Fix: disable DMA_PUSH before reprogramming GET/PUT, re-enable after.
  Also switched to writing GET before PUT for additional safety.
- Verified: 50 consecutive runs with zero failures.
- **Files**: tests/pfifo.asm
- **Result**: 48/48 pass (pfifo 50/50 stress test)
- **Status**: DONE

### Step 21: Page table bit named constants (eedec47)
- Added `PTE_PRESENT`, `PTE_RW`, `PTE_ACCESSED`, `PTE_DIRTY`, `PDE_PS`,
  `PDE_4MB_BASE`, `PDE_4MB_OFF` to executor.hpp.
- Replaced all raw hex PDE/PTE bit constants in executor.cpp `translate_va()`
  and trace_builder.cpp `translate_va_jit()`.
- **Files**: src/executor.hpp, src/executor.cpp, src/trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 22: FPU/SSE init + BUS_FLOAT constants (HEAD)
- Added `FPU_CW_INIT` (0x037F), `SSE_MXCSR_INIT` (0x1F80) as local constexpr
  in executor.cpp `init()`, replacing mutable temporaries.
- Added `MmioMap::BUS_FLOAT` constant (0xFFFFFFFF) in mmio.hpp; replaced
  raw hex in mmio.hpp `read()` and trace_builder.cpp MMIO fallback paths.
- **Files**: src/executor.cpp, src/mmio.hpp, src/trace_builder.cpp
- **Result**: 48/48 pass
- **Status**: DONE

### Step 23: DESIGN.md accuracy audit — stale page-version, offsets, counts (HEAD)
- Removed all stale "page-version" references; replaced with VEH-based
  page-protection + trace invalidation descriptions (§2.6, §4, §5.13).
- §5.13: rewritten to describe VEH fast→slow path patching, not version bumps.
- §5.16a: "page-version system (§2.6)" → "page-protection system (§2.6)" (×2).
- §4 checklist: "SMC page-version validation" → "SMC page-protection + VEH-based
  trace invalidation".
- §2.2: R15 description → "(unused — callee-saved, reserved for ABI compliance)".
- §5.2: guest_fpu offset 128→112, host_fpu offset 640→624.
- §5.4: fs_base offset 108→96, gs_base offset 112→100.
- §5.15: MAX_IO_PORTS "increased to 32" → "increased to 64".
- §5.16: "14-assertion" → "58-assertion" for test_pgraph.cpp.
- PTIMER_INTR_0: "stub: always 0" → "alarm fires bit 0".
- **Files**: doc/DESIGN.md
- **Result**: 48/48 pass
- **Status**: DONE

### Step 24: BIOS boot infrastructure — flash ROM execution + real-mode boot (HEAD)
- **Code from flash ROM**: TraceBuilder::build() now accepts (code_ptr, code_len)
  buffer; run loop fetches code via MMIO for addresses ≥ GUEST_RAM_SIZE.
  handle_privileged() also fetches instruction bytes from flash.
- **16 MB BIOS mirror**: BIOS_SIZE changed from 1 MB to 16 MB to cover the
  entire 0xFF000000–0xFFFFFFFF range including reset vector at 0xFFFFFFF0.
  MMIO range check fixed to avoid uint32_t overflow.
- **Real-mode boot interpreter**: `interpret_real_mode_boot()` handles the
  16-bit prologue (JMP rel8, LGDT, LIDT, MOV CR0, far JMP) to reach 32-bit PM.
- **Segment register MOV**: `MOV DS/ES/SS/FS/GS, r/m16` and `MOV r16, sreg`
  handled as privileged instructions; selectors stored in GuestContext.
  FS/GS base loaded from GDT via `load_segment_base()`.
- **String I/O**: REP INSB/INSW/INSD and REP OUTSB/OUTSW/OUTSD implemented
  in handle_privileged() with direction flag and paging support.
- **BIOS boot result**: Successfully enters 32-bit PM at 0xFFFFFE00, processes
  init table commands (I/O, PCI config, MMIO read/write) until max_steps.
- **Files**: executor.cpp, executor.hpp, context.hpp, trace_builder.cpp,
  trace_builder.hpp, mmio.hpp, xbox/address_map.hpp, test_runner.cpp,
  tests/segment.asm, doc/DESIGN.md (§5.27–5.31)
- **Result**: 51/51 pass
- **Status**: DONE

### Step 25: XBE dashboard execution — HLE stubs + JIT fixes (HEAD)
- **KPCR address fix**: Moved FS_BASE from 0x70000 to 0xD00000 for XBE mode.
  Old address overlapped XBE .text section (0x12000–0xCF408), zeroing code
  that included the `__SEH_prolog` function.
- **RET imm16 fix**: `emit_ret_exit()` now correctly adds the immediate
  operand to ESP (`ESP += 4 + extra_pop`) instead of just `ESP += 4`.
- **High-byte register MOV fix**: Added C-helper path for MOV with AH/CH/DH/BH
  and memory operands. These registers conflict with REX prefix in x86-64,
  so the fastmem rewrite can't use R12/R14 directly.
- **MOVZX 16-bit destination fix**: MOVZX AX/CX/DX/BX with memory source
  now correctly preserves upper 16 bits of the 32-bit register.
- **~30 new HLE kernel stubs**: IoCreateDevice/File/SymbolicLink,
  IoDeleteDevice/SymbolicLink, IoDismountVolumeByName, HalReadSMCTrayState,
  HalGetInterruptVector, NtCreateFile/Event/Mutant, NtOpenFile, NtReadFile,
  NtSetInformationFile, NtDeviceIoControlFile, NtQueryFullAttributesFile,
  NtClearEvent, NtWaitForMultipleObjectsEx, ObReferenceObjectByHandle,
  ObfDereferenceObject, KeGetCurrentIrql/Thread, KeDelayExecutionThread,
  KeEnterCriticalRegion, KeConnectInterrupt, KeInitializeInterrupt,
  KeStallExecutionProcessor, KeRaiseIrqlToDpcLevel, KfLowerIrql,
  KeBugCheck/Ex, MmLockUnlockBufferPages, ExInitializeReadWriteLock,
  FscSetCacheSize.
- **ESP/LEA test**: New esp_lea.asm with 8 test cases verifying LEA/MOV
  with ESP base work correctly in JIT.
- **XBE execution**: xboxdash.xbe now executes 305 HLE calls with zero
  unhandled kernel ordinals. Performs file I/O (NtOpenFile/NtReadFile/
  NtSetInformationFile/NtClose cycles), interrupt setup, critical section
  operations, timer management. All spawned threads complete normally.
- **Files**: src/executor.cpp, src/trace_builder.cpp, src/trace_builder.hpp,
  src/test_runner.cpp, src/xbe_loader.hpp, tests/esp_lea.asm, tests/hle.asm,
  CMakeLists.txt
- **Result**: 52/52 pass
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
| e39e04b | 48   | 0    | memcpy aliasing          |
| 106c906 | 48   | 0    | xbox comments            |
| 885eba9 | 48   | 0    | consistency fixes        |
| 4b0fdd3 | 48   | 0    | memcpy trace_builder     |
| 750a582 | 48   | 0    | stale CMP R14,R15 refs   |
| c55ed9f | 48   | 0    | MSR named constants      |
| 86ef544 | 48   | 0    | PAGE_SIZE/MASK constants  |
| 678128d | 48   | 0    | pfifo race fix (50/50)   |
| eedec47 | 48   | 0    | PTE/PDE bit constants    |
| (HEAD)  | 48   | 0    | FPU/SSE + BUS_FLOAT      |
| (HEAD)  | 51   | 0    | BIOS boot infrastructure |
| (HEAD)  | 52   | 0    | XBE HLE stubs + JIT fixes|