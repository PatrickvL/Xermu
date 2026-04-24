# Guided Executor — Working Document

## 1. Project Goal

Build a **JIT-based x86-32 CPU emulator** targeting Pentium III class CPUs (IA-32,
SSE1 + MMX). The host is also x86-64, so guest instructions can execute almost
natively — only privileged and memory-sensitive instructions need rewriting.
This is the "guided executor" model: host registers *are* guest registers, with
surgical interception points.

The executor provides a **sandbox** for ring-0 guest code: all privileges
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
|│  Executor        │  Run loop: lookup → validate → build → dispatch → repeat
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
  Zydis mnemonic enum to a compact `InsnClassId` (24 classes). Initialized once
  by `init_mnemonic_table()` at startup. Cost: ~1.8 KB, single array lookup.

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
| IC_PUSH       | PUSH reg                                 | `emit_handler_push`    |
| IC_PUSH_IMM   | PUSH imm8/imm32                          | `emit_handler_push`    |
| IC_POP        | POP                                      | `emit_handler_pop`     |
| IC_LEAVE      | LEAVE                                    | `emit_handler_leave`   |
| IC_MOVZX_MEM  | MOVZX                                    | `emit_handler_movzx_mem` |
| IC_MOVSX_MEM  | MOVSX                                    | `emit_handler_movsx_mem` |
| IC_FPU_MEM    | FLD/FST/FISTP/FADD/FSUB/FMUL/FDIV/FCOM/FLDCW/... (27 x87 mnemonics) | `emit_handler_fpu_mem` |
| IC_SSE_MEM    | MOVAPS/ADDPS/MULPS/XORPS/SHUFPS/... (36 SSE1 + 37 MMX; SSE2 gated) | `emit_handler_fpu_mem` (shared) |
| IC_PUSHFD     | PUSHFD                                   | `emit_handler_pushfd`  |
| IC_POPFD      | POPFD                                    | `emit_handler_popfd`   |
| IC_STRING     | MOVSB/W/D, STOSB/W/D, LODSB/W/D, CMPSB/W/D, SCASB/W/D (15 mnemonics) | `emit_handler_string` |
| IC_ENTER      | ENTER                                    | `emit_handler_enter`   |
| IC_XLATB      | XLAT                                     | `emit_handler_xlatb`   |
| IC_FLAGMEM    | SETcc/CMOVcc [mem]                       | `emit_handler_flagmem` |
| IC_PUSHAD     | PUSHAD                                   | `emit_handler_pushad`  |
| IC_POPAD      | POPAD                                    | `emit_handler_popad`   |
| IC_TERMINATOR | JMP/CALL/RET/Jcc/LOOP/LOOPE/LOOPNE/IRETD | (inline in build loop) |
| IC_PRIVILEGED | HLT/LGDT/CLI/STI/IN/OUT/RDMSR/WRMSR/INT/INT3/... | (stop_reason + RET)    |

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
version counter. Every inline fastmem store emits a page-version bump sequence
(`emit_smc_page_bump`). The C-helper slow path (`write_guest_mem32`) also bumps
the version. Before executing a cached trace, the run loop compares the trace's
recorded page version against the current version. On mismatch: invalidate and
rebuild.

The bump sequence per store (12 or 16 bytes):
```
[PUSHFQ]                     ; only when guest insn produces live flags
PUSH RAX                     ; save guest EAX
MOV RAX, [R13+120]           ; load page_versions pointer
SHR R14D, 12                 ; page index from PA in R14
INC DWORD [RAX+R14*4]        ; bump version
POP RAX                      ; restore
[POPFQ]                      ; only when guest insn produces live flags
```

**EFLAGS handling**: This JIT does **not** use lazy flag evaluation — guest
instructions execute natively, so guest EFLAGS live in host RFLAGS. The bump's
SHR+INC clobbers arithmetic flags. Whether PUSHFQ/POPFQ are needed depends on
the call site:

| Call site             | Guest produces flags? | PUSHFQ/POPFQ? | Bytes |
|-----------------------|-----------------------|---------------|-------|
| MOV [mem], reg/imm    | No                    | Skipped       | 12    |
| ALU [mem] (ADD/SUB…)  | Yes                   | `!save_flags` | 12–16 |
| SETcc/CMOVcc [mem]    | No (reads only)       | Emitted†      | 16    |
| PUSH imm/reg          | No                    | Skipped       | 12    |
| FPU/SSE [mem] store   | No (x86 flags)        | Skipped       | 12    |
| CALL exit (retaddr)   | N/A (trace exit)      | Skipped       | 12    |

† SETcc/CMOVcc: flags are live (just read by the instruction) and must survive
  to any subsequent flag-reading instruction in the trace.

For ALU [mem] writes, `preserve_flags = !save_flags`: when the outer flag-save
bracket is already active (protecting prior live flags from the CMP R14,R15
clobber), the bump's own PUSHFQ/POPFQ is redundant — the outer POPFQ will
restore flags anyway. When no outer bracket is present (full flag writers like
ADD/SUB where prior flags are dead), the bump emits its own PUSHFQ/POPFQ to
protect the new ALU result from SHR+INC. This avoids nested flag saves.

**CMP/TEST [mem]**: These ALU instructions read memory but don't write back.
The bump is correctly skipped for them (they are not stores).

---

## 3. File Map

| File                  | Purpose                                        | Status        |
|-----------------------|------------------------------------------------|---------------|
| `CMakeLists.txt`      | Build config, Zydis fetch, MASM for MSVC       | ✅ Working    |
| `platform.hpp`        | OS memory allocation (RWX + RW)                | ✅ Working    |
| `context.hpp`         | `GuestContext` struct, register indices         | ✅ Working    |
| `mmio.hpp`            | MMIO region dispatch table                     | ✅ Working    |
| `code_cache.hpp`      | 32 MB executable slab allocator                | ✅ Working    |
| `trace.hpp`           | `Trace` struct, two-level `TraceCache`         | ✅ Working    |
| `emitter.hpp`         | Byte emitter, EA synthesis, fastmem helpers, generic mem rewriter | ✅ Working    |
| `trace_builder.hpp`   | `TraceBuilder`, `TraceArena`, `PageVersions`   | ✅ Working    |
| `insn_dispatch.hpp`   | Two-level O(1) mnemonic dispatch table          | ✅ Working    |
| `trace_builder.cpp`   | Decode → classify → emit loop (table-driven)   | ✅ Working    |
| `executor.hpp`        | `Executor` struct, `dispatch_trace` decl       | ✅ Working    |
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
| `tests/advanced.asm`  | CMOVcc/SETcc/BSWAP/IMUL/MUL/DIV (54)             | ✅ ALL PASS   |
| `tests/string.asm`    | MOVS/STOS/LODS/CMPS/SCAS + REP (36 assertions)  | ✅ ALL PASS   |
| `tests/segment.asm`   | FS/GS segment override tests (22 assertions)     | ✅ ALL PASS   |
| `tests/indirect.asm`  | JMP/CALL [mem], PUSH/POP [mem] (20 assertions)   | ✅ ALL PASS   |
| `tests/privileged.asm` | CLI/STI/CPUID/RDTSC/LGDT/LIDT/CR0-4/IRET (17)   | ✅ ALL PASS   |
| `tests/misc.asm`      | ENTER/LEAVE/XLATB/CBW/CDQ/LAHF/SAHF/CLC (33)    | ✅ ALL PASS   |
| `tests/smc.asm`       | Self-modifying code: patch imm/opcode/jmp/alu (11) | ✅ ALL PASS   |
| `tests/interrupt.asm`  | INT→IDT→IRETD, nested, CLI/STI, INT3, EFLAGS (9) | ✅ ALL PASS   |
| `tests/paging.asm`    | Page table walk, 4KB/4MB pages, INVLPG (9)         | ✅ ALL PASS   |
| `tests/xbox.asm`      | Xbox address map: RAM mirror, PCI, NV2A, Flash (9) | ✅ ALL PASS   |
| `tests/hle.asm`       | HLE kernel stubs: thread, memory, handle (4)       | ✅ ALL PASS   |
| `tests/linking.asm`   | Block linking: tight loops, Jcc, nested, CALL (7)  | ✅ ALL PASS   |
| `tests/pic.asm`       | 8259A PIC: ICW init, IMR, IRQ delivery, EOI (7)    | ✅ ALL PASS   |

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
- [x] Trace cache with two-level direct-mapped lookup (O(1), no hashing)
- [x] SMC page-version validation on trace re-entry
- [x] ASM trampoline for both GCC/Clang (inline asm) and MSVC (MASM)
- [x] Shadow space / calling convention handling for Windows x64 JIT→C calls
- [x] EFLAGS preservation across memory dispatch (PUSHFQ/POPFQ, liveness-gated)
- [x] Self-tests pass: sum loop, EFLAGS preservation, LEA/PUSH/POP/MOV[mem]imm, x87 reg ops, x87 mem store
- [x] SSE1/MMX memory-operand dispatch (36 SSE1 + 37 MMX mnemonics, `IC_SSE_MEM`)
- [x] SSE2+ mnemonics conditionally compiled (`TARGET_SSE` flag)
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
- [x] ENTER imm16, imm8 stack frame creation via C helper (IC_ENTER)
- [x] XLATB table lookup [EBX+AL] via C helper (IC_XLATB)
- [x] INT/INT3/INT1/INTO software interrupt traps (IC_PRIVILEGED stubs)
- [x] SMC write-side page-version bumping (inline per store + C-helper slow path)
- [x] CALL direct/register: return address stored via imm-to-mem (no ECX clobber)
- [x] NASM test infrastructure: 18 suites, CMake integration

---

## 5. What's Missing — Prioritized

### Phase 1: Run Real x86-32 Code (No Hardware Emulation)

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
now contains two 512-byte FXSAVE areas: `guest_fpu` (offset 128) and `host_fpu`
(offset 640), both 16-byte aligned.

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
for register-only). SSE2+ mnemonics are gated behind `#if TARGET_SSE >= 2`
(default: 1, matching Pentium III Coppermine). Verified by `tests/sse.asm` (48
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
base, base+disp, base+index*scale+disp). Common in kernels that use FS for
thread-local state (e.g. Xbox KPCR, Windows KPCR).
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
access. `XCHG [mem], reg` / `CMPXCHG [mem], reg` / `XADD [mem], reg` are
handled by the generic memory rewriter (`IC_ALU_MEM`).

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
| IN / OUT          | ✅ IoPortEntry dispatch table in Executor          |
| LGDT / LIDT       | ✅ Update ctx->gdtr/idtr base/limit                |
| LLDT / LTR        | Stub needed (not yet encountered)                  |
| CLI / STI         | ✅ Toggle ctx->virtual_if                          |
| PUSHF / POPF      | ✅ Guest-stack handlers (IC_PUSHFD/IC_POPFD)       |
| IRET              | ✅ Pop EIP/CS/EFLAGS via iret_helper               |
| INVLPG            | ✅ Flushes TLB entry for VA (§5.11)                |
| WBINVD / INVD     | ✅ Stub                                            |
| CLTS / LMSW       | ✅ Stub                                            |
| CPUID             | ✅ Leaves 0 and 1 (vendor + features)              |
| RDTSC             | ✅ Host __rdtsc()                                  |
| HLT               | ✅ Idle until next interrupt                       |
| INT n             | ✅ IDT delivery (§5.12)                            |
| INT3 / INT1       | ✅ IDT delivery (vector 3 / vector 1)              |
| INTO              | ✅ IDT delivery (vector 4 if OF set)               |

---

### Phase 2: Address Translation & Interrupts

Required to boot the Xbox kernel, which enables paging early in its startup.

#### 5.11 ~~Paging / Virtual Address Translation (CR0.PG)~~ ✅ DONE
**Resolved.** Full 32-bit non-PAE page-table walk with 4 KB and 4 MB page support.
Every guest memory access goes through VA→PA translation when `CR0.PG=1`.

**Implementation approach: JIT inline translate + C helper page walk**

When `CR0.PG` is set at trace-build time, the emitter inserts an inline
`translate_va_jit()` C call after EA synthesis (before the CMP R14, R15 bounds
check). The translate call:
1. Saves guest RFLAGS and GP regs
2. Calls `translate_va_jit(ctx, VA, is_write)` — a C-linkage page walker
3. On success: `R14 = PA`, restores GP regs + RFLAGS, continues to fastmem path
4. On fault: writes `STOP_PAGE_FAULT` + faulting EIP to ctx, restores state, RETs

The run loop handles `STOP_PAGE_FAULT` by delivering `#PF` (vector 14) through IDT.

**Key components:**

- **`translate_va(va, is_write)`** (Executor member): Walks CR3 page directory →
  page table in guest RAM. Supports 4 KB pages (PDE→PTE→PA) and 4 MB pages
  (PDE.PS=1→PA). Checks Present and R/W bits. Sets Accessed/Dirty bits. Sets
  CR2 to faulting VA on fault. Returns PA or `~0u` on fault.

- **`translate_va_jit(ctx, va, is_write)`** (C-linkage, called from JIT): Same
  page walk logic but reads CR3 and RAM pointer from GuestContext fields so it
  can be called from JIT code with only a ctx pointer.

- **`emit_translate_r14(e, is_write, fault_eip)`**: Emitter helper that generates
  the inline PUSHFQ → save_gp → C call → check fault → MOV R14D,EAX → load_gp →
  POPFQ sequence. Placed BEFORE `emit_save_flags` at each call site to keep the
  stack clean on the fault exit path.

- **`emit_paging_translate(e, is_write)`**: Wrapper; no-op when `e.paging` is false.

- **CR0 write**: Flushes TLB and invalidates all cached traces (mode change makes
  all emitted code stale — traces are built with a specific paging state).

- **CR3 write**: Flushes TLB (`tlb.flush()`).

- **INVLPG**: Computes EA from the instruction operand and calls `tlb.flush_va(ea)`.

- **C helpers paging-aware**: All C-linkage helpers that access guest memory
  (`pushfd_helper`, `popfd_helper`, `read_guest_mem32`, `write_guest_mem32`,
  `xlatb_helper`, all string helpers, `deliver_interrupt` stack pushes, IDT reads)
  call `guest_translate(ctx, addr, is_write)` which invokes `translate_va_jit`
  when `CR0.PG` is set.

- **EIP translation**: Run loop translates EIP VA→PA when paging is on before
  passing to `builder.build()`. Instruction fetch page faults deliver `#PF`
  (vector 14) with error code bit 4 (instruction fetch).

**Performance note:** When paging is on, every JIT memory access incurs a C call
for translation (PUSHFQ + save_gp + call + load_gp + POPFQ). This is correct
but slower than the non-paging fast path. A future optimization could add an
inline software TLB lookup in the JIT that avoids the C call on TLB hit.

Test suite: `tests/paging.asm` — 9 assertions covering page table setup, paging
enable, identity-mapped read/write, remapped page read/write (VA→different PA),
PUSH/POP through paged stack, CALL/RET through paged stack, INVLPG + remap,
and paging disable returning to flat mode.

#### 5.12 ~~Interrupt / Exception Delivery~~ ✅ DONE
**Resolved.** Full IDT-based interrupt delivery for software interrupts (INT n),
debug traps (INT3/INT1), and hardware IRQs.

**Key changes:**
- **`halted` vs `virtual_if`**: Separated the two concepts. `halted` controls the
  run loop (`while (!ctx.halted)`); `virtual_if` is the guest IF flag (cleared by
  CLI, set by STI, cleared by interrupt gates, restored by IRETD). Previously
  both used `virtual_if`, so CLI halted the executor.
- **`deliver_interrupt(vector, return_eip, has_error, error_code)`**: Reads the
  8-byte gate descriptor from guest IDT (`idtr_base + vector*8`), pushes an
  interrupt frame (EFLAGS, CS, EIP, optional error code) onto the guest stack,
  clears IF for interrupt gates (type 0xE), and sets `ctx.eip` to the handler.
  The saved EFLAGS includes the correct IF state (merged from `virtual_if`).
- **Trampoline EFLAGS save/restore**: `dispatch_trace` now saves host RFLAGS to
  `ctx->eflags` on trace exit and restores it on trace entry. This ensures flags
  survive across trace boundaries (critical for IRETD restoring CF/ZF/etc. that
  the next trace may read). Both MASM and GCC inline-asm trampolines updated.
- **`emit_save_eflags()`**: New emitter helper; used before STOP_PRIVILEGED traps
  to snapshot host RFLAGS into `ctx->eflags` so `deliver_interrupt()` pushes the
  correct guest flags (e.g. CF set by STC before INT).
- **`iret_helper`**: Now restores `ctx->virtual_if` from the IF bit of the popped
  EFLAGS, so interrupt enable state is correctly restored on IRETD.
- **Hardware IRQs**: Optional `IrqCheckFn`/`IrqAckFn` callbacks on Executor.
  When set (xbox mode), the 8259A PIC pair provides vectoring: device raises
  IRQ on PIC → PIC evaluates masking/priority → executor calls `irq_ack()` at
  trace boundary → PIC returns vector and moves IRQ to in-service register →
  guest sends EOI via OUT 0x20/0xA0.  Legacy `pending_irq` bitmap retained for
  non-xbox mode (vectors `0x20 + irq`).
- **INT handlers**: INT n delivers through IDT (was stub/halt). INT3 → vector 3.
  INTO → vector 4 if OF set. CLI/STI only toggle `virtual_if` (don't halt).

Test suite: `tests/interrupt.asm` — 9 assertions covering INT→ISR→IRETD roundtrip,
chained interrupts, nested interrupts (ISR calls INT), CLI/STI continuation,
INT3 dispatch, stack integrity, and EFLAGS (CF) preservation across interrupt.

#### 5.13 ~~SMC Write-Side Version Bumping~~ ✅ DONE

#### 5.14 ~~Correctness audit~~ ✅ DONE
Full codebase audit for correctness, efficiency, and consistency.  Fixes:

| Bug | Impact | Fix |
|-----|--------|-----|
| `TraceCache::invalidate()` sets slot to EMPTY, breaking open-addressing probe chains for subsequent entries | Stale / duplicate traces after SMC invalidation | Tombstone sentinel (`DELETED = 0xFFFFFFFE`); `lookup` already skips non-matching keys; `insert` now reuses tombstone slots |
| IRETD exit missing `emit_load_all_gp` after C call | EAX/ECX/EDX corrupted by volatile-register clobber; trampoline saves junk to `ctx->gp[]` | Added `emit_load_all_gp(e)` + EFLAGS restore via `MOV R14D,[R13+36]; PUSH R14; POPFQ` before RET |
| LOOPE/LOOPNE always branch to `taken_eip` | Incorrect control flow — ZF and ECX conditions ignored | Check original ZF (JNZ/JZ rel32) before DEC ECX; each path decrements ECX and branches correctly |
| CALL direct/register write return address with no ESP bounds check | Host memory corruption if guest ESP is out of range | Added `CMP R14, R15; JAE` → UD2 guard before `emit_fastmem_store_imm32` |
**Resolved.** Every inline fastmem store emits `emit_smc_page_bump()` — a
12-or-16 byte sequence that loads the `page_versions` pointer from `GuestContext`
(offset 120), right-shifts R14D (PA) by 12, and increments the version counter.
The PUSHFQ/POPFQ wrapper is conditional: emitted only when the guest instruction
produces arithmetic flags that may be live (ALU read-modify-write, SETcc/CMOVcc
stores). Skipped for MOV stores, FPU stores, PUSH, and trace exit paths (12
bytes). CMP/TEST [mem] are correctly excluded (read-only, no bump needed).
The C-helper slow path (`write_guest_mem32`) also bumps.

Nine JIT call sites: `emit_fastmem_dispatch`, `emit_fastmem_dispatch_store_imm`,
`emit_handler_alu_mem`, `emit_handler_flagmem`, `emit_handler_push` (imm/reg),
`emit_handler_fpu_mem`, `emit_call_exit` (direct/register).

Additionally fixed: CALL direct/register clobbered guest ECX via `MOV ECX, retaddr`
scratch — replaced with `emit_fastmem_store_imm32` (imm-to-mem, no clobber).

> **Design note**: This JIT does not use lazy flag evaluation. Guest instructions
> run natively, so guest EFLAGS live in host RFLAGS. A `mprotect`/SIGSEGV-based
> approach (zero per-store cost, fault on actual SMC) would eliminate the bump
> overhead entirely but adds implementation complexity. The current per-store
> bump is correct and sufficient; the conditional PUSHFQ/POPFQ avoids the worst
> overhead on non-flag-producing stores (MOV, PUSH, FPU — the common case).

---

### Phase 3: Xbox Hardware

#### 5.15 Xbox Physical Address Map ✅
```
Guest PA          Size       Type
──────────────────────────────────────────
0x00000000       64/128 MB   RAM (retail/devkit)
0x0C000000       128 MB      RAM mirror (NV2A tiling)
0xF0000000         1 MB      Flash ROM
0xFD000000        16 MB      NV2A GPU registers (MMIO)
0xFE800000         4 MB      APU / AC97 (MMIO)
0xFEC00000         4 KB      I/O APIC (MMIO)
0xFF000000         1 MB      BIOS shadow (Flash alias)
```

Implemented in `src/xbox.hpp` with `xbox_setup()` function:
- **RAM mirror** at 0x0C000000: reads/writes alias main RAM (modulo 64 MB).
- **Flash ROM** at 0xF0000000: 1 MB read-only, defaults to 0xFF (empty).
- **BIOS shadow** at 0xFF000000: maps to Flash ROM.
- **NV2A GPU stub**: register reads return realistic defaults (PMC_BOOT_0 chip
  ID, PFB_CFG0 memory config, PTIMER, PCRTC, PVIDEO). Writes update state.
- **APU stub**: all-zero reads, writes silently dropped.
- **I/O APIC**: index/data register pair with 24 redirection entries.
- **PCI configuration space**: I/O ports 0xCF8/0xCFC with Xbox device table
  (Host Bridge, LPC, SMBus, NV2A, APU, USB×2, IDE, AGP bridge).
- **SMBus**: I/O ports 0xC000–0xC00E, status/control/address/data registers,
  auto-complete transactions, EEPROM read stub (device 0x54).
- **PIC/PIT/System Port B**: stub handlers for basic Xbox hardware.
- **Debug console**: I/O port 0xE9 (bochs-style putchar).

Test runner supports `--xbox` flag to use the full Xbox address map.
`MAX_IO_PORTS` increased from 16 to 32.

#### 5.16 NV2A GPU
**Priority: CRITICAL for games** — 16 MB of MMIO registers at `0xFD000000`.
Responsible for all graphics. The NV2A is an NV20-class GPU (GeForce 3 derivative).
This is by far the largest single implementation effort.

#### 5.17 APU (Audio Processing Unit)
**Priority: HIGH for games** — AC97-compatible audio + DSP at `0xFE800000`.

#### 5.18 Other Devices

**8259A PIC (Programmable Interrupt Controller)** ✅ DONE

Dual 8259A PIC (master at 0x20-0x21, slave at 0xA0-0xA1) with full
initialization and operation:

- **ICW1-4 init sequence**: cascade/single mode, vector base programming,
  cascade identity, 8086 mode, auto-EOI option.
- **IRR/ISR/IMR registers**: interrupt request, in-service, and mask registers
  with proper read-back via OCW3.
- **OCW2 EOI**: non-specific and specific end-of-interrupt commands.
- **Cascade**: slave on master IRQ 2; slave provides vector on cascade ack.
- **Integration**: `PicPair` struct in `xbox.hpp`, wired to executor via
  `irq_check`/`irq_ack` function pointers.  Devices call `pic.raise_irq(N)`
  to assert lines.  Test-only I/O port 0xEB triggers IRQs for testing.

Remaining devices:
- SMBus (EEPROM, temperature sensor, video encoder) — stub
- IDE controller (HDD, DVD)
- USB (controllers)
- PCI configuration space — implemented
- MCPX boot ROM

#### 5.19 XBE Loader ✅

Implemented in `src/xbe_loader.hpp`:

- **XBE header parsing**: validates magic (`XBEH`), extracts base address
  (typically `0x00010000`), section count, entry point, kernel thunk address.
- **Section loading**: copies raw data to virtual addresses, zero-fills padding.
- **XOR key decoding**: entry point and kernel thunk address are XOR-encoded
  with retail (`0xA8FC57AB` / `0x5B6D40B6`) or debug keys. Loader tries retail
  first, falls back to debug.
- **Kernel thunk resolution**: the thunk table is an array of ordinal entries
  (bit 31 set + ordinal number). Each is replaced with the address of an HLE
  stub. Stubs live at guest PA `0x80000`: `MOV EAX, ordinal / INT 0x20 / RET`.
- **TLS directory**: reads TLS raw data range, writes TLS index = 0.
- **HLE kernel handler**: `INT 0x20` traps are intercepted by the executor
  (new `hle_handler` callback on `Executor`). A default handler implements:
  - Memory: `ExAllocatePool`, `MmAllocateContiguousMemory[Ex]`,
    `MmFreeContiguousMemory`, `MmGetPhysicalAddress`,
    `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` (bump allocator at 16 MB+)
  - Threading: `KeGetCurrentThread` (fake KTHREAD ptr), `PsCreateSystemThread`
    (stub), `PsTerminateSystemThread`
  - I/O: `NtClose` (stub)
  - Display: `AvSetDisplayMode`, `AvGetSavedDataAddress` (stubs)
  - System: `HalReturnToFirmware` (halts guest), `DbgPrint`, `RtlInitAnsiString`
  - Unhandled ordinals: log + return `STATUS_NOT_IMPLEMENTED`
- **stdcall cleanup**: args are removed from guest stack by relocating the
  return address before the stub's RET instruction.

Test runner `--xbox` mode writes HLE stubs to guest RAM and installs the handler.
`tests/hle.asm` exercises 4 kernel calls: `KeGetCurrentThread`,
`MmGetPhysicalAddress`, `NtClose`, `ExAllocatePool`.

#### 5.20 Kernel HLE or LLE
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

#### 5.21 Block Linking (Trace Chaining)
**Status: DONE**

Each trace exit with a statically known target EIP now emits a **linkable
JMP rel32** before the cold save-all-GP / write-next-EIP / RET fallback:

```
JMP rel32          ; 5 bytes — initially rel32=0 (falls through to cold path)
; --- cold exit path ---
save_all_gp        ; 7× MOV [R13+off], reg
write_next_eip     ; MOV R14D, imm32 / MOV [R13+40], R14D
RET                ; return to dispatch_trace trampoline
```

**Trace struct** carries up to 2 `LinkSlot`s (for Jcc: taken + fallthrough, or
1 for unconditional exits).  Each slot records the address of the rel32 field
and the target guest EIP.

**Linking** happens eagerly in the run loop.  After a trace `t` is found or
built, `try_link_trace(t)` patches every unlinked exit whose target trace is
already in the cache (forward linking).  The previously executed trace
`prev_trace` is also re-checked, handling the common backward-edge case (a loop
branch back to its head).

When linked, the JMP bypasses the entire save / trampoline / run-loop / lookup
path — guest registers stay live in host registers, and execution continues
directly into the next trace's first instruction.

**Unlinking** on SMC: `unlink_trace(t)` scans all traces in the arena and
resets any JMP rel32 that targets `t->guest_eip` back to 0 (cold fallthrough).
This is called before invalidating a trace due to a page-version mismatch.

**SMC safety**: traces that contain page-version bumps (`emit_smc_page_bump`)
set `Emitter::smc_written = true`, which suppresses link-slot creation entirely.
Additionally, `try_link_trace` validates the target's page version before
patching, preventing links to stale traces.

Link slot layout per trace:

| Field        | Type      | Description                              |
|-------------|-----------|------------------------------------------|
| `jmp_rel32` | `uint8_t*`| Address of the rel32 in the JMP opcode   |
| `target_eip`| `uint32_t`| Guest EIP this exit targets              |
| `linked`    | `bool`    | `true` if patched to a real target trace |

#### 5.22 ~~Fastmem Window (4 GB VA Reservation)~~ — Rejected
Rejected: guard-page / VEH-based MMIO dispatch contradicts the core design
principle (§2.3 — "No Guard Pages"). The inline CMP R14,R15 + JAE slow_path
pattern is the correct approach: every memory access is rewritten at JIT time
with no OS exception handling in the hot path.

#### 5.23 ~~Trace Cache Improvements~~ ✅ DONE
- Two-level direct-mapped cache: `page_map[(eip>>12) & L1_MASK][(eip & 0xFFF) >> 1]`
  for O(1) lookup with no hashing, no probing, no tombstones.  L1 = 32768 entries
  (128 MB coverage, modular-mapped for larger VA spaces).  L2 = 2048 Trace* per
  page, allocated on demand.  Full `guest_eip` verified on lookup to handle L1
  aliasing.
- Trace linking between direct-branch targets — already done in §5.21.

---

## 6. Bugs Fixed During Development

| # | Bug | Root Cause | Fix |
|---|-----|-----------|-----|
| 1 | `ZydisDecoderDecodeFull` error `0x8010000D` | `ZYDIS_MINIMAL_MODE=ON` strips operand tables | Set `ZYDIS_MINIMAL_MODE OFF` |
| 2 | DEC ECX silently becomes REX prefix | Short-form INC/DEC `0x40`–`0x4F` are REX in x64 | `emit_clean_insn()` re-encodes as `FF /0` / `FF /1` |
| 3 | Host RSP clobbers `ctx->gp[ESP]` | `emit_save_all_gp` saved all 8 regs including ESP | Skip `GP_ESP` in save/load loops |
| 4 | `ctx->gp[EAX]` = return address, not sum | `emit_ret_exit` loaded `[ESP]` into EAX before saving | Save GP regs first, write retaddr directly to `ctx->next_eip` |
| 5 | Stack overflow on `Executor` construction | ~6.5 MB of embedded arrays (TraceCache, PageVersions) | Heap-allocate via `std::make_unique` |
| 6 | MSVC: `#error` on `dispatch_trace` | GCC `__attribute__((naked))` not available on MSVC x64 | MASM `.asm` trampoline for MSVC |
| 7 | MMIO slow-path calls corrupt args on Windows | JIT emitted SysV ABI (RDI/RSI/RDX/RCX) on Windows | Platform-aware `emit_ccall_arg*` helpers + shadow space |
| 8 | Memory dispatch clobbers guest EFLAGS | CMP R14, R15 bounds check + SUB/ADD RSP for ctx ESP management destroy flags | Conditional PUSHFQ/POPFQ wrapping gated by backward flag-liveness analysis |

---

## 7. Sandbox Properties

The executor provides containment of ring-0 guest code essentially for free:

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
.\build\Release\guided_executor.exe
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
