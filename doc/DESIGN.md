# Xbox Guided Executor — Working Document

## 1. Project Goal

Build a **JIT-based x86-32 CPU emulator** targeting the original Xbox (Pentium III,
733 MHz, IA-32). The host is also x86-64 (Intel), so guest instructions can execute
almost natively — only privileged and memory-sensitive instructions need rewriting.
This is the "guided executor" model: host registers *are* guest registers, with
surgical interception points.

The executor provides a **sandbox** for the Xbox kernel: all ring-0 privileges
(CR writes, LGDT, CLI/STI, IN/OUT) are emulated in userspace. Guest code cannot
escape the fastmem window or affect real host hardware.

---

## 2. Architecture Overview

```
Guest EIP
    │
    ▼
┌────────────────┐
│  TraceBuilder   │  Zydis decode → classify → emit
│  (trace_builder │  per-instruction rewrite or verbatim copy
│   .hpp/.cpp)    │
└────────┬───────┘
         │ emits into
         ▼
┌────────────────┐
│   CodeCache     │  32 MB RWX slab (VirtualAlloc / mmap)
│  (code_cache    │
│   .hpp)         │
└────────┬───────┘
         │ dispatched by
         ▼
┌────────────────┐
│ dispatch_trace  │  ASM trampoline: load guest regs, CALL trace, save back
│ (executor.cpp   │  GCC/Clang: inline asm  |  MSVC: dispatch_trace.asm
│  / .asm)        │
└────────┬───────┘
         │ orchestrated by
         ▼
┌────────────────┐
│  XboxExecutor   │  Run loop: lookup → validate → build → dispatch → repeat
│  (executor      │  SMC detection via page-version tags
│   .hpp/.cpp)    │
└─────────────────┘
```

### 2.1 Core Principle: State Identity

Since guest ISA ≈ host ISA:

| Guest         | Host                                              |
|---------------|---------------------------------------------------|
| EAX–EDI       | Live in host EAX–EDI during trace execution       |
| ESP           | NOT in host RSP — lives in `ctx->gp[GP_ESP]`     |
| EIP           | Implicit (host instruction pointer within trace)  |
| EFLAGS        | Host EFLAGS (direct, with caveats for INC/DEC)    |
| FPU / SSE     | Host FPU/SSE via FXSAVE/FXRSTOR in trampoline     |
| CR0/CR3/CR4   | Emulated fields in `GuestContext`                 |
| GDT/IDT       | Emulated fields in `GuestContext`                 |
| IF            | Virtual (`ctx->virtual_if` bool)                  |

### 2.2 Host Register Reservation

During trace execution, four host registers are pinned:

| Register | Purpose                                              |
|----------|------------------------------------------------------|
| R12      | `fastmem_base` — host pointer to guest PA 0          |
| R13      | `GuestContext*` — executor context pointer            |
| R14      | EA scratch — used for address computation staging     |
| R15      | `ram_size` — comparison threshold for fastmem check   |

This eliminates bounds-check overhead: every memory access is a CMP + conditional
branch to a slow path, with the fast path being a single `[R12 + R14]` dereference.

### 2.3 Memory Access Dispatch (No Guard Pages)

Every memory instruction is decoded and rewritten *before* execution. No VEH, no
SIGSEGV, no exception handling in the hot path:

```
Synthesize EA → R14D
[PUSHFQ + LEA RSP,-8]  (only if backward liveness says arithmetic flags are live)
CMP R14, R15           (PA < ram_size?)
JAE slow_path          (predicted not-taken)
OP reg, [R12 + R14]    ← fastmem: direct host dereference, ~3 cycle overhead
JMP done
slow_path:
  save GP regs → ctx
  call mmio_dispatch_{read,write}(ctx, PA, gp_idx, size)
  load GP regs ← ctx
done:
[LEA RSP,+8 + POPFQ]  (only if saved above)
```

The PUSHFQ/POPFQ wrapping ensures that the CMP R14, R15 bounds check does not
clobber guest EFLAGS when they are live. The wrapping is conditional: a backward
flag-liveness analysis during trace building determines which dispatch sites
actually need it. A flag is "live" if it was set by a prior guest instruction
and will be tested by a later instruction (Jcc, CMOVcc, SETcc, etc.) before
being overwritten. Dispatches where no arithmetic flags are live skip the
PUSHFQ/POPFQ entirely, saving ~22 bytes and two stack round-trips per site.

### 2.4 Instruction Dispatch Table (`insn_dispatch.hpp`)

All instruction classification is driven by a **two-level O(1) dispatch table**:

- **Level 1**: `uint8_t MNEMONIC_CLASS[ZYDIS_MNEMONIC_MAX_VALUE+1]` — maps every
  Zydis mnemonic enum to a compact `InsnClassId` (0–18). Initialized once by
  `init_mnemonic_table()` at startup. Cost: ~1.8 KB, single array lookup.

- **Level 2**: `InsnClass INSN_CLASS_TABLE[IC_MAX]` — maps `InsnClassId` to an
  `{EmitHandler, InsnClassFlags}` pair. `InsnClassFlags` encode properties:
  - `ICF_HAS_DISPATCH` — the handler emits CMP R14,R15 (may clobber EFLAGS)
  - `ICF_CLEAN_COPY` — verbatim copy or re-encode, no memory access
  - `ICF_TERMINATOR` — trace exit (branch/call/ret)
  - `ICF_PRIVILEGED` — privileged instruction (trap/UD2)

Class IDs and their handlers:

| InsnClassId   | Mnemonics                                | Handler                |
|---------------|------------------------------------------|------------------------|
| IC_CLEAN      | (no mem operand fallback via ALU)        | `emit_handler_clean`   |
| IC_LEA        | LEA                                      | `emit_handler_lea`     |
| IC_MOV_MEM    | MOV                                      | `emit_handler_mov_mem` |
| IC_ALU_MEM    | ADD/SUB/AND/OR/XOR/CMP/ADC/SBB/INC/DEC/NEG/NOT/SHL/SHR/SAR/ROL/ROR/RCL/RCR | `emit_handler_alu_mem` |
| IC_TEST_MEM   | TEST                                     | `emit_handler_test_mem`|
| IC_PUSH       | PUSH (reg + imm)                         | `emit_handler_push`    |
| IC_POP        | POP                                      | `emit_handler_pop`     |
| IC_LEAVE      | LEAVE                                    | `emit_handler_leave`   |
| IC_MOVZX_MEM  | MOVZX                                    | `emit_handler_movzx_mem` |
| IC_MOVSX_MEM  | MOVSX                                    | `emit_handler_movsx_mem` |
| IC_FPU_MEM    | FLD/FST/FISTP/FADD/FSUB/FMUL/FDIV/FCOM/FLDCW/... (27 x87 mnemonics) | `emit_handler_fpu_mem` |
| IC_SSE_MEM    | MOVAPS/ADDPS/MULPS/XORPS/SHUFPS/... (36 SSE1 + 37 MMX; SSE2 gated) | `emit_handler_fpu_mem` (shared) |
| IC_PUSHFD     | PUSHFD                                   | `emit_handler_pushfd`  |
| IC_POPFD      | POPFD                                    | `emit_handler_popfd`   |
| IC_STRING     | MOVSB/W/D, STOSB/W/D, LODSB/W/D, CMPSB/W/D, SCASB/W/D (15 mnemonics) | `emit_handler_string` |
| IC_TERMINATOR | JMP/CALL/RET/Jcc/LOOP/LOOPE/LOOPNE/IRETD | (inline in build loop) |
| IC_PRIVILEGED | HLT/LGDT/CLI/STI/IN/OUT/RDMSR/WRMSR/... | (stop_reason + RET)    |

The `build()` loop in Phase 1 uses `lookup_flags()` for dispatch detection and
termination. Phase 3 uses `lookup_insn_class()` to fetch the handler callback —
no switch/if chains on mnemonics remain in the hot path.

### 2.5 Clean Instruction Handling

Instructions without memory operands or privilege requirements are copied verbatim
into the code cache (`memcpy`). Exception: short-form INC/DEC r32 (opcodes
`0x40`–`0x4F`) which collide with REX prefixes on x86-64 — these are re-encoded
to the two-byte `FF /0` / `FF /1` forms.

### 2.6 Self-Modifying Code Detection

Page-granular version tags (`PageVersions`). Each guest page has a `uint32_t`
version counter. Before executing a cached trace, the run loop compares the
trace's recorded page version against the current version. On mismatch: invalidate
and rebuild.

> **Not yet implemented**: bumping page versions on fastmem writes. Currently only
> the lookup/validate/rebuild cycle is wired; stores don't increment versions.

---

## 3. File Map

| File                  | Purpose                                        | Status        |
|-----------------------|------------------------------------------------|---------------|
| `CMakeLists.txt`      | Build config, Zydis fetch, MASM for MSVC       | ✅ Working    |
| `platform.hpp`        | OS memory allocation (RWX + RW)                | ✅ Working    |
| `context.hpp`         | `GuestContext` struct, register indices         | ✅ Working    |
| `mmio.hpp`            | MMIO region dispatch table                     | ✅ Working    |
| `code_cache.hpp`      | 32 MB executable slab allocator                | ✅ Working    |
| `trace.hpp`           | `Trace` struct, `TraceCache` hash table        | ✅ Working    |
| `emitter.hpp`         | Byte emitter, EA synthesis, fastmem helpers, generic mem rewriter | ✅ Working    |
| `trace_builder.hpp`   | `TraceBuilder`, `TraceArena`, `PageVersions`   | ✅ Working    |
| `insn_dispatch.hpp`   | Two-level O(1) mnemonic dispatch table          | ✅ Working    |
| `trace_builder.cpp`   | Decode → classify → emit loop (table-driven)   | ✅ Working    |
| `executor.hpp`        | `XboxExecutor` struct, `dispatch_trace` decl   | ✅ Working    |
| `executor.cpp`        | ASM trampoline (GCC/Clang), run loop           | ✅ Working    |
| `dispatch_trace.asm`  | MASM trampoline for MSVC                       | ✅ Working    |
| `main.cpp`            | Self-tests: sum loop, EFLAGS, LEA/PUSH/MOV, x87 | ✅ ALL PASS   |
| `test_runner.cpp`     | NASM test binary loader (flat 32-bit .bin)       | ✅ Working    |
| `tests/harness.inc`   | NASM test macros (ASSERT_EQ, ASSERT_FLAGS, PASS) | ✅ Working    |
| `tests/alu.asm`       | ALU test suite (52 assertions)                   | ✅ ALL PASS   |
| `tests/memory.asm`    | Memory/XCHG/CMPXCHG/XADD/BT/BSF/SHLD (76)       | ✅ ALL PASS   |
| `tests/flow.asm`      | Control flow: LOOP/Jcc/CALL/RET/recursion (27)   | ✅ ALL PASS   |
| `tests/fpu.asm`       | x87 FPU test suite (17 assertions)               | ✅ ALL PASS   |
| `tests/sse.asm`       | SSE1 float ops test suite (48 assertions)        | ✅ ALL PASS   |
| `tests/advanced.asm`  | CMOVcc/SETcc/BSWAP/IMUL (42)                     | ✅ ALL PASS   |
| `tests/string.asm`    | MOVS/STOS/LODS/CMPS/SCAS + REP (36 assertions)  | ✅ ALL PASS   |
| `tests/segment.asm`   | FS/GS segment override tests (22 assertions)     | ✅ ALL PASS   |
| `tests/indirect.asm`  | JMP/CALL [mem], PUSH/POP [mem] (20 assertions)   | ✅ ALL PASS   |
| `tests/privileged.asm` | CLI/STI/CPUID/RDTSC/LGDT/LIDT/CR0-4/IRET (17)   | ✅ ALL PASS   |

---

## 4. What Works Today

- [x] Zydis-based instruction decode (LEGACY_32 mode, full operand detail)
- [x] Trace building: linear scan to first branch/call/ret, page-boundary stop
- [x] Verbatim copy of clean instructions (with INC/DEC re-encoding)
- [x] Two-level O(1) dispatch table for mnemonic → handler classification
- [x] Inline fastmem dispatch for MOV r32←mem, MOV mem←r32, MOV [mem]←imm (8/16/32-bit)
- [x] MMIO slow-path call-out (platform-aware ABI: SysV and Windows x64)
- [x] PUSH r32 / PUSH imm8/imm32 / POP r32 with inline ESP management
- [x] LEA — address computation without memory access (no fastmem dispatch)
- [x] ALU reg/mem forms (ADD/SUB/AND/OR/XOR/CMP/ADC/SBB/INC/DEC/NEG/NOT/shifts/rotates)
- [x] MOVZX/MOVSX from memory (byte/word → dword zero/sign extension)
- [x] LEAVE (MOV ESP,EBP + POP EBP)
- [x] FPU/SSE state save/restore via FXSAVE/FXRSTOR in dispatch trampoline
- [x] x87 register-only instructions run natively (FLD/FADD/FMUL/FCOM/FNSTSW/etc.)
- [x] x87 memory-operand forms via generic `emit_rewrite_mem_to_fastmem()` rewriter
- [x] Generic memory operand rewriter (rewrites any insn to use `[R12+R14]` fastmem)
- [x] CALL near (direct + register-indirect) trace exit
- [x] RET trace exit (stack read → `next_eip`, preserves live EAX)
- [x] JMP direct/indirect/memory trace exit
- [x] Conditional branch (all Jcc) trace exit with taken/fallthrough paths
- [x] Trace cache with open-addressed hash lookup
- [x] SMC page-version validation on trace re-entry
- [x] ASM trampoline for both GCC/Clang (inline asm) and MSVC (MASM)
- [x] Shadow space / calling convention handling for Windows x64 JIT→C calls
- [x] EFLAGS preservation across memory dispatch (PUSHFQ/POPFQ, liveness-gated)
- [x] Self-tests pass: sum loop, EFLAGS preservation, LEA/PUSH/POP/MOV[mem]imm, x87 reg ops, x87 mem store
- [x] SSE1/MMX memory-operand dispatch (36 SSE1 + 37 MMX mnemonics, `IC_SSE_MEM`)
- [x] SSE2+ mnemonics conditionally compiled (`XBOX_TARGET_SSE` flag)
- [x] PUSHFD/POPFD guest-stack handlers (`IC_PUSHFD`/`IC_POPFD`)
- [x] LOOP/LOOPE/LOOPNE terminator handling (DEC ECX + conditional exit)
- [x] MOVZX/MOVSX register-register forms (verbatim copy)
- [x] Privileged instruction stop_reason mechanism (replaces UD2 with RET to run loop)
- [x] I/O port dispatch (IN/OUT emulation with IoPortEntry table)
- [x] Debug console (port 0xE9 Bochs-style character output)
- [x] String instructions: MOVS/STOS/LODS/CMPS/SCAS with REP/REPE/REPNE (`IC_STRING`)
- [x] FS/GS segment override support (emit_ea_to_r14 adds segment base from ctx)
- [x] CALL [mem] / JMP [mem] via C helpers (read_guest_mem32 / call_mem_helper)
- [x] PUSH [mem] / POP [mem] via C helpers (push_mem_helper / pop_mem_helper)
- [x] 8/16-bit register MOV memory forms (AL/CL/DL/BL, AX-DI via guest_reg_enc)
- [x] PUSHAD/POPAD via C helpers (IC_PUSHAD/IC_POPAD dispatch classes)
- [x] Privileged insn emulation: CLI/STI/CPUID/RDTSC/RDMSR/WRMSR/LGDT/LIDT
- [x] MOV CRn / MOV r,CRn emulation (CR0/CR2/CR3/CR4 read/write in handle_privileged)
- [x] INVLPG/WBINVD/INVD/CLTS/LMSW privileged stubs
- [x] IRETD via C helper (pop EIP/CS/EFLAGS from guest stack)
- [x] XCHG/CMPXCHG/XADD [mem] via generic rewriter (IC_ALU_MEM)
- [x] BT/BTS/BTR/BTC [mem] bit test instructions
- [x] BSF/BSR bit scan instructions
- [x] SHLD/SHRD double-precision shift instructions
- [x] NASM test infrastructure: 10 suites, 357 total assertions, CMake integration

---

## 5. What's Missing — Prioritized

### Phase 1: Run Real x86-32 Code (No Xbox Hardware)

These are needed to execute non-trivial x86-32 programs beyond the self-test.

#### 5.1 ~~EFLAGS Preservation Across Slow Paths~~ ✅ DONE
**Resolved.** The CMP R14, R15 bounds check in every memory dispatch was
clobbering guest EFLAGS. Fixed with conditional PUSHFQ/POPFQ wrapping gated by
a backward flag-liveness analysis: the trace builder pre-scans all instructions
to extract per-instruction flags-tested/flags-written (from Zydis `cpu_flags`),
then sweeps backward to determine which dispatch sites have live arithmetic flags
that would be clobbered. Only those sites emit the save/restore pair. Applied to
`emit_mem_dispatch`, `emit_push`, and `emit_pop`. Verified by Test 2 (CMP+MOV+JE
sequence where ZF must survive a memory load).

#### 5.2 ~~FPU / x87 / MMX / SSE State~~ ✅ DONE (x87 core)
**Resolved for x87.** Guest FPU/SSE state is saved/restored via FXSAVE/FXRSTOR
in the dispatch trampoline (both MASM and GCC/Clang inline asm). `GuestContext`
now contains two 512-byte FXSAVE areas: `guest_fpu` (offset 112) and `host_fpu`
(offset 624), both 16-byte aligned.

- **Register-only x87 instructions** (FLD1, FADD ST(i), FCOMPP, FNSTSW AX, etc.)
  run natively — verbatim-copied by the trace builder.
- **Memory-operand x87 instructions** (FLD [mem], FSTP [mem], FISTP [mem], FLDCW
  [mem], etc.) use the generic `emit_rewrite_mem_to_fastmem()` rewriter which
  reconstructs any instruction to use `[R12+R14]` addressing. 27 x87 mnemonics
  registered in the dispatch table under `IC_FPU_MEM`.
- **Guest FPU defaults**: FCW = 0x037F (all exceptions masked, double precision),
  MXCSR = 0x1F80 (all SSE exceptions masked).
- Verified by Test 4 (FLD1+FADDP+FCOMPP+FNSTSW AX) and Test 5 (FISTP [mem]).

**SSE1/MMX now registered.** 36 SSE1 single-precision mnemonics (MOVAPS, ADDPS,
MULPS, SHUFPS, COMISS, CVTSI2SS, LDMXCSR, etc.) and 37 MMX mnemonics (PADDB,
PSHUFW, MASKMOVQ, etc.) are registered under `IC_SSE_MEM` and share the same
`emit_handler_fpu_mem` handler (generic rewriter for memory forms, verbatim copy
for register-only). SSE2+ mnemonics are gated behind `#if XBOX_TARGET_SSE >= 2`
(default: 1, matching the Xbox Pentium III). Verified by `tests/sse.asm` (48
assertions covering all major SSE1 instruction categories).

**PUSHFD/POPFD** now have dedicated JIT handlers (`IC_PUSHFD`/`IC_POPFD`).
PUSHFD captures host EFLAGS via PUSHFQ+POP R14, then calls a C helper to store
onto the guest stack (ctx->gp[ESP], not host RSP). POPFD calls a C helper to read
from the guest stack, then sets host EFLAGS via PUSH RAX + POPFQ.

**MOVZX/MOVSX register-register** forms now verbatim-copy instead of returning
failure. The memory-operand path is unchanged.

#### 5.3 ~~String Instructions (REP MOVS/STOS/CMPS/SCAS/LODS)~~ ✅ DONE
**Resolved.** 15 string mnemonics (MOVSB/W/D, STOSB/W/D, LODSB/W/D, CMPSB/W/D,
SCASB/W/D) are handled by `IC_STRING` / `emit_handler_string`. The handler
captures host EFLAGS (for DF direction bit) via PUSHFQ+POP R14, saves GP regs,
and calls per-mnemonic C helpers that operate directly on fastmem. REP, REPE,
and REPNE prefixes are detected via `insn.attributes & ZYDIS_ATTRIB_HAS_REP*`.
CMPS/SCAS helpers compute arithmetic flags in software; MOVS/STOS/LODS pass
flags through unchanged. Verified by `tests/string.asm` (36 assertions).

#### 5.4 ~~Segment Register Support (FS/GS Overrides)~~ ✅ DONE
**Resolved.** `emit_ea_to_r14` now handles FS and GS segment overrides.
`GuestContext` has `fs_base` (offset 108) and `gs_base` (offset 112) fields.
When a memory operand has an FS or GS segment prefix, `emit_ea_to_r14` computes
the flat EA as usual, then emits `ADD R14D, [R13 + seg_offset]` to add the
segment base from the context. Works with all addressing modes (disp-only,
base, base+disp, base+index*scale+disp). The Xbox kernel uses FS for KPCR.
Verified by `tests/segment.asm` (22 assertions).

#### 5.5 ~~8-bit and 16-bit Register Operands in Memory Dispatch~~ ✅ DONE
**Resolved.** MOVZX/MOVSX r32,[mem8/mem16] have dedicated handlers. Direct
8/16-bit register forms (`MOV AL, [mem]`, `MOV [mem], CX`, etc.) now handled via
`guest_reg_enc()` which maps 8-bit low (AL/CL/DL/BL), 16-bit (AX-DI), and 32-bit
registers to their 3-bit encoding for `emit_fastmem_dispatch`. Note: AH/CH/DH/BH
not supported (REX prefix conflict). Verified by `tests/memory.asm` (assertions
42-51).

#### 5.6 ~~Immediate-to-Memory Instructions~~ ✅ DONE
**Resolved.** `emit_handler_mov_mem` detects `ops[other_idx].type == IMMEDIATE`
and routes to `emit_fastmem_dispatch_store_imm` which writes the immediate
directly to `[R12+R14]` (fastmem) or calls `mmio_dispatch_write_imm` (slow path).
Verified by Test 3 (`MOV DWORD PTR [0x4000], 0x42`).

#### 5.7 ~~LEA / Multi-Memory-Operand Classification~~ ✅ DONE
**Resolved.** LEA is classified as `IC_LEA` with `ICF_CLEAN_COPY` — it computes
EA into R14 and moves the result to the destination register, with no memory
access. `XCHG [mem], reg` / `CMPXCHG [mem], reg` still need read-modify-write
support.

#### 5.8 ~~CALL/JMP Indirect Through Memory~~ ✅ DONE
`JMP [mem]`, `CALL [mem]`, `PUSH [mem]`, `POP [mem]` all implemented via
C helper functions (`read_guest_mem32`, `write_guest_mem32`, `call_mem_helper`,
`push_mem_helper`, `pop_mem_helper`). The helpers handle both fastmem and MMIO
paths transparently. Verified by `tests/indirect.asm` (20 assertions).

#### 5.9 ~~PUSH/POP Immediate~~ ✅ DONE + ~~Memory Forms~~ ✅ DONE + ~~PUSHAD/POPAD~~ ✅ DONE
**All PUSH/POP forms resolved.** PUSH imm8/imm32 via inline fastmem store.
`PUSH [mem]` and `POP [mem]` via C helpers. `PUSHAD` and `POPAD` via C helpers
(`pushad_helper`/`popad_helper`) that operate directly on `ctx->gp[]` and guest
stack. Dispatch table classes: `IC_PUSHAD`/`IC_POPAD`.

#### 5.10 ~~Privileged Instruction Handling~~ ✅ DONE
**Priority: MEDIUM** (for kernel boot) — `handle_privileged()` handles all
common privileged instructions via the `stop_reason` field. The JIT detects
privileged mnemonics (and MOV CRn which shares ZYDIS_MNEMONIC_MOV) and emits
a trap to the run loop.

| Instruction       | Status                                             |
|-------------------|----------------------------------------------------|
| RDMSR / WRMSR     | ✅ Stub (advance EIP)                              |
| MOV CRn, r / r, CRn | ✅ Read/write ctx->cr0/cr2/cr3/cr4              |
| IN / OUT          | ✅ IoPortEntry dispatch table in XboxExecutor       |
| LGDT / LIDT       | ✅ Update ctx->gdtr/idtr base/limit                |
| LLDT / LTR        | Stub needed (not yet encountered)                  |
| CLI / STI         | ✅ Toggle ctx->virtual_if                          |
| PUSHF / POPF      | ✅ Guest-stack handlers (IC_PUSHFD/IC_POPFD)       |
| IRET              | ✅ Pop EIP/CS/EFLAGS via iret_helper               |
| INVLPG            | ✅ Stub (no paging yet)                            |
| WBINVD / INVD     | ✅ Stub                                            |
| CLTS / LMSW       | ✅ Stub                                            |
| CPUID             | ✅ Leaves 0 and 1 (vendor + features)              |
| RDTSC             | ✅ Host __rdtsc()                                  |
| HLT               | ✅ Idle until next interrupt                       |

---

### Phase 2: Address Translation & Interrupts

Required to boot the Xbox kernel, which enables paging early in its startup.

#### 5.11 Paging / Virtual Address Translation (CR0.PG)
**Priority: CRITICAL for kernel** — The Xbox kernel runs with paging enabled.
Every guest memory access must go through VA→PA translation when `CR0.PG=1`.

Design options:
- **Software TLB**: maintain a `TLB[N]` of `{guest_va → guest_pa}` entries.
  On every memory access, look up the TLB; on miss, walk guest page tables in
  fastmem. The trace-emitted EA synthesis path becomes: compute guest VA → TLB
  lookup → PA → fastmem/MMIO check. Adds latency but is correct and portable.
- **Shadow page tables**: map a host VA region such that
  `host_va = FASTMEM_BASE + shadow_pt(guest_va)`. When guest writes CR3 or
  modifies PTEs, rebuild the shadow mapping. Guest VA accesses go through host
  MMU — zero per-access overhead after shadow PT rebuild.

The software TLB approach is simpler to implement first; shadow PT can be added
as an optimization later.

#### 5.12 Interrupt / Exception Delivery
**Priority: CRITICAL for kernel** — The Xbox kernel depends on:
- Hardware IRQs (GPU vsync, APU, USB, IDE, timer)
- Page faults (#PF → cr2 + IDT dispatch)
- General protection faults (#GP)
- Breakpoint/debug exceptions (#BP, #DB)

Implementation:
1. Device threads set `pending_irq_bitmap |= (1 << irq_line)`
2. Run loop checks at trace boundaries: if `pending && virtual_if`, deliver
   highest-priority interrupt
3. `deliver_interrupt()`: push `{EFLAGS, CS, EIP}` onto guest stack, clear
   virtual IF, look up IDT entry, set `ctx->eip` to ISR entry point

For exceptions (synchronous): the instruction emitter can detect fault
conditions (e.g., page-not-present during TLB walk) and emit an inline exception
delivery sequence, or return to the run loop with an exception code that triggers
delivery before the next trace.

#### 5.13 SMC Write-Side Version Bumping
**Priority: MEDIUM** — The read-side validation (check page version before trace
execution) is implemented. The write-side (bump `page_versions[pa >> 12]++` on
every store) is not. For correctness, every fastmem store must also increment the
version of the target page. This is a single `INC DWORD PTR [page_versions + ...]`
per store — emitted inline or done via a thin wrapper.

---

### Phase 3: Xbox Hardware

#### 5.14 Xbox Physical Address Map
```
Guest PA          Size       Type
──────────────────────────────────────────
0x00000000       64/128 MB   RAM (retail/devkit)
0x0C000000       128 MB      RAM mirror (NV2A tiling)
0xF0000000         8 MB      Flash ROM
0xFD000000        16 MB      NV2A GPU registers (MMIO)
0xFE800000         4 MB      APU / AC97 (MMIO)
0xFEC00000         4 MB      I/O APIC (MMIO)
0xFF000000         1 MB      BIOS shadow (MMIO/ROM)
```

The MMIO dispatch table (`MmioMap`) needs to be populated with handlers for each
device region. The current stub handler logs and returns `0xDEADBEEF`.

#### 5.15 NV2A GPU
**Priority: CRITICAL for games** — 16 MB of MMIO registers at `0xFD000000`.
Responsible for all graphics. The NV2A is an NV20-class GPU (GeForce 3 derivative).
This is by far the largest single implementation effort.

#### 5.16 APU (Audio Processing Unit)
**Priority: HIGH for games** — AC97-compatible audio + DSP at `0xFE800000`.

#### 5.17 Other Devices
- SMBus (EEPROM, temperature sensor, video encoder)
- IDE controller (HDD, DVD)
- USB (controllers)
- PCI configuration space
- MCPX boot ROM

#### 5.18 XBE Loader
**Priority: HIGH** — Parse Xbox executable format (.xbe) headers, set up initial
memory layout, resolve kernel imports. Without this, can't load actual games.

#### 5.19 Kernel HLE or LLE
Two paths:
- **HLE (High-Level Emulation)**: intercept Xbox kernel API calls at the symbol
  boundary; stub or reimplement each export. Faster to develop, less accurate.
  This is what Cxbx-Reloaded uses.
- **LLE (Low-Level Emulation)**: boot the actual Xbox kernel ROM. Requires all
  of Phase 2 + accurate hardware emulation. More accurate, much harder.

A hybrid approach is likely: LLE for the CPU mechanics (this executor), HLE for
kernel API stubs, with gradual replacement as hardware emulation matures.

---

### Phase 4: Performance

#### 5.20 Block Linking (Trace Chaining)
**Priority: HIGH** — Currently every trace exits back to the C++ run loop for
the next dispatch. Direct trace-to-trace chaining (patch the exit jump to point
at the next trace's host code) eliminates the run-loop overhead for hot paths.
This is the single largest performance win available.

#### 5.21 Fastmem Window (4 GB VA Reservation)
**Priority: MEDIUM** — Currently using a flat 128 MB `VirtualAlloc` slab. The
design calls for reserving the full 4 GB guest PA space as a contiguous host VA
region, with only RAM pages committed and MMIO regions left as guard pages. This
would allow `host_va = FASTMEM_BASE + guest_pa` without any bounds check,
eliminating the CMP+JAE pair from every memory access.

#### 5.22 Trace Cache Improvements
- Two-level cache: `page_map[guest_page][offset >> 1]` for O(1) direct-mapped
  lookup instead of hash table
- Trace linking between direct-branch targets

---

## 6. Bugs Fixed During Development

| # | Bug | Root Cause | Fix |
|---|-----|-----------|-----|
| 1 | `ZydisDecoderDecodeFull` error `0x8010000D` | `ZYDIS_MINIMAL_MODE=ON` strips operand tables | Set `ZYDIS_MINIMAL_MODE OFF` |
| 2 | DEC ECX silently becomes REX prefix | Short-form INC/DEC `0x40`–`0x4F` are REX in x64 | `emit_clean_insn()` re-encodes as `FF /0` / `FF /1` |
| 3 | Host RSP clobbers `ctx->gp[ESP]` | `emit_save_all_gp` saved all 8 regs including ESP | Skip `GP_ESP` in save/load loops |
| 4 | `ctx->gp[EAX]` = return address, not sum | `emit_ret_exit` loaded `[ESP]` into EAX before saving | Save GP regs first, write retaddr directly to `ctx->next_eip` |
| 5 | Stack overflow on `XboxExecutor` construction | ~6.5 MB of embedded arrays (TraceCache, PageVersions) | Heap-allocate via `std::make_unique` |
| 6 | MSVC: `#error` on `dispatch_trace` | GCC `__attribute__((naked))` not available on MSVC x64 | MASM `.asm` trampoline for MSVC |
| 7 | MMIO slow-path calls corrupt args on Windows | JIT emitted SysV ABI (RDI/RSI/RDX/RCX) on Windows | Platform-aware `emit_ccall_arg*` helpers + shadow space |
| 8 | Memory dispatch clobbers guest EFLAGS | CMP R14, R15 bounds check + SUB/ADD RSP for ctx ESP management destroy flags | Conditional PUSHFQ/POPFQ wrapping gated by backward flag-liveness analysis |

---

## 7. Sandbox Properties

The executor provides containment of the Xbox kernel essentially for free:

| Escape Vector | Mitigation |
|---|---|
| Raw memory write to host VA | Fastmem window containment; PA >= ram_size → MMIO dispatch |
| RDMSR/WRMSR to control host MSRs | All MSR access dispatched through thunk |
| LGDT/LIDT to install real IDT | Thunk updates `ctx->idtr_base` only |
| CLI/STI to mask host interrupts | Toggle `ctx->virtual_if` (virtual) |
| IN/OUT to access host hardware | I/O port dispatch table |
| INVD / cache poisoning | Thunk is a no-op |
| Self-modifying code patching thunks | Page-version SMC detection triggers re-scan |
| Forged return address from thunk | Thunk stack lives on host RSP, outside fastmem |

Guest code runs in host ring 3. All ring-0 operations are emulated. The guest
kernel believes it controls the machine — all silently intercepted.

---

## 8. Build & Run

```
cmake -B build -A x64
cmake --build build --config Release
.\build\Release\xbox_executor.exe
```

Expected output:
```
=== Test 1: Sum loop + fastmem round-trip ===
  EAX  = 55      (expected 55)
  EBX  = 0xDEADBEEF      (expected 0xDEADBEEF)
  ECX  = 0       (expected 0)
  [0x4000] = 55          (expected 55)
  [0x4004] = 55          (expected 55)
  PASS

=== Test 2: EFLAGS preservation across memory dispatch ===
  EBX  = 1       (expected 1 = JE taken)
  EDX  = 42      (expected 42 = loaded from RAM)
  PASS

=== Test 3: LEA, PUSH imm, PUSH/POP regs, MOV [mem] imm ===
  EDX  = 55      (expected 55  = LEA result)
  EBX  = 55      (expected 55  = POP after PUSH EDX)
  ESI  = 0x12345678      (expected 0x12345678 = POP after PUSH imm)
  EDI  = 0x00000042      (expected 0x00000042 = MOV EDI,[0x4000])
  ESP  = 0x00080004      (expected 0x00080004 = STACK_TOP+4 after RET)
  PASS

=== Test 4: x87 register-only operations ===
  EBX  = 1       (expected 1 = FCOMPP equal)
  PASS

=== Test 5: x87 memory store (FISTP to RAM) ===
  EAX  = 5       (expected 5)
  [0x4000] = 5           (expected 5)
  PASS

ALL PASS
```

---

## 9. Dependencies

| Dependency | Version | Purpose | License |
|---|---|---|---|
| Zydis | v4.1.0 | x86 instruction decode | MIT |
| CMake | ≥ 3.20 | Build system | BSD |
| MSVC / GCC / Clang | C++20 | Compiler | — |

No other external dependencies. The emitter is hand-rolled byte sequences.
No AsmJit, no LLVM, no DBI framework.

---

## 10. Reference Material

| Resource | Relevance |
|---|---|
| DynamoRIO `core/arch/x86/mangle.c` | Trace cache design, SMC handling, memory operand rewriting |
| QEMU TCG `softmmu_template.h` | Per-access fastmem+MMIO inline dispatch pattern |
| FEX-Emu softmmu | Clean modern JIT + memory dispatch implementation |
| Unicorn Engine | Validation oracle — run same code in parallel to verify |
| Cxbx-Reloaded | Existing Xbox HLE emulator; integration target |
| Intel SDM Vol. 3 | Paging, privilege levels, interrupt delivery |
| Xbox kernel RE docs | Hardware register maps, kernel structure layouts |
