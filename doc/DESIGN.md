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
| R15      | `ram_size` — fastmem window size (320 MB with aliasing, 128 MB fallback) |

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
| `platform.hpp`        | OS memory allocation (RWX + RW), aliased fastmem window   | ✅ Working    |
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
| `pe_loader.hpp`       | Win32 PE loader (xboxkrnl.exe) + export resolver | ✅ Working    |
| `xbe_loader.hpp`      | Xbox XBE loader: sections, thunks, TLS, HLE     | ✅ Working    |
| `xbox.hpp`            | Umbrella header — includes all `src/xbox/` files | ✅ Working    |
| `xbox/address_map.hpp` | Xbox physical address map constants              | ✅ Working    |
| `xbox/nv2a.hpp`       | NV2A GPU: PMC, PFIFO, PTIMER, PCRTC, PRAMDAC stubs | ✅ Working |
| `xbox/nv2a_thread.hpp` | NV2A PFIFO dedicated host thread (CV-driven)    | ✅ Working    |
| `xbox/pgraph.hpp`     | PGRAPH state shadow (NV097 method → state)       | ✅ Working    |
| `xbox/apu.hpp`        | APU register stubs (VP/GP/EP)                    | ✅ Working    |
| `xbox/ide.hpp`        | IDE ATA: PIO sector read/write + IDENTIFY data   | ✅ Working    |
| `xbox/usb.hpp`        | USB OHCI controller stubs (2 ports each)         | ✅ Working    |
| `xbox/ioapic.hpp`     | I/O APIC register stubs                          | ✅ Working    |
| `xbox/ram_mirror.hpp` | RAM mirror (0x0C000000) read/write               | ✅ Working    |
| `xbox/flash.hpp`      | Flash ROM + MCPX: BIOS loading, shadow           | ✅ Working    |
| `xbox/pci.hpp`        | PCI configuration space (type 0, CF8/CFC)        | ✅ Working    |
| `xbox/smbus.hpp`      | SMBus: EEPROM, SMC, video encoder                | ✅ Working    |
| `xbox/pic.hpp`        | 8259A PIC pair (master + slave)                  | ✅ Working    |
| `xbox/pit.hpp`        | 8254 PIT (3 channels, rate gen + one-shot)       | ✅ Working    |
| `xbox/misc_io.hpp`    | System control port 0x61, debug console 0xE9     | ✅ Working    |
| `xbox/setup.hpp`      | XboxHardware struct, tick callback, xbox_setup()  | ✅ Working    |
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
| `tests/misc.asm`      | ENTER/LEAVE/XLATB/CBW/CDQ/LAHF/SAHF/CLC/ESP (50)   | ✅ ALL PASS   |
| `tests/smc.asm`       | Self-modifying code: patch imm/opcode/jmp/alu (11) | ✅ ALL PASS   |
| `tests/interrupt.asm`  | INT→IDT→IRETD, nested, CLI/STI, INT3, EFLAGS (9) | ✅ ALL PASS   |
| `tests/paging.asm`    | Page table walk, 4KB/4MB pages, INVLPG (9)         | ✅ ALL PASS   |
| `tests/xbox.asm`      | Xbox address map: RAM mirror, PCI, NV2A, Flash (9) | ✅ ALL PASS   |
| `tests/hle.asm`       | HLE kernel stubs: timing, sync, memory, interlocked (21) | ✅ ALL PASS   |
| `tests/linking.asm`   | Block linking: tight loops, Jcc, nested, CALL (7)  | ✅ ALL PASS   |
| `tests/pic.asm`       | 8259A PIC: ICW init, IMR, IRQ delivery, EOI (7)    | ✅ ALL PASS   |
| `tests/pit.asm`       | 8254 PIT: rate gen, one-shot, latch, IRQ delivery (5) | ✅ ALL PASS   |
| `tests/nv2a_timer.asm` | NV2A PTIMER: freerunning counter, num/den, readback (6) | ✅ ALL PASS   |
| `tests/pcrtc.asm`     | NV2A PCRTC: vblank poll, W1C clear, IRQ delivery (6) | ✅ ALL PASS   |
| `tests/smbus.asm`     | SMBus: EEPROM read/write, SMC queries, W1C (8)     | ✅ ALL PASS   |
| `tests/nv2a_gpu.asm`  | NV2A GPU: PFIFO+DMA pusher, PGRAPH, PRAMDAC, PFB (16) | ✅ ALL PASS   |
| `tests/esp_mem.asm`    | ESP+mem: ALU/MOVZX/MOVSX/MOV ESP with memory (11) | ✅ ALL PASS   |
| `tests/apu.asm`       | APU: VP/GP/EP base + status registers (7)        | ✅ ALL PASS   |
| `tests/ide.asm`       | IDE ATA: register defaults, IDENTIFY data, PIO (16) | ✅ ALL PASS   |
| `tests/usb.asm`       | USB OHCI: register defaults, reset, PCI (12)     | ✅ ALL PASS   |
| `tests/mtrr.asm`      | MTRR MSRs: variable + fixed range read/write (8) | ✅ ALL PASS   |
| `tests/gdt_tss.asm`   | GDT/TSS: LLDT/LTR/SLDT/STR/SGDT/SIDT (10)       | ✅ ALL PASS   |
| `tests/flash.asm`     | Flash ROM: BIOS shadow + flash base reads (4)    | ✅ ALL PASS   |
| `test_pe_loader.cpp`  | PE loader C++ unit test (12 assertions)           | ✅ ALL PASS   |
| `tests/pfifo.asm`     | PFIFO DMA pusher: command parsing + JUMP (5)      | ✅ ALL PASS   |
| `test_pgraph.cpp`     | PGRAPH state shadow C++ unit test (14 assertions) | ✅ ALL PASS   |

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
- [x] NASM test infrastructure: 33 suites, CMake integration
- [x] NV2A PFIFO on dedicated host thread (parallel with guest CPU, CV-driven)

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
| RDMSR / WRMSR     | ✅ SYSENTER MSRs, TSC, MTRR (19 MSRs)             |
| MOV CRn, r / r, CRn | ✅ Read/write ctx->cr0/cr2/cr3/cr4              |
| IN / OUT          | ✅ IoPortEntry dispatch table in Executor          |
| LGDT / LIDT       | ✅ Update ctx->gdtr/idtr base/limit                |
| SGDT / SIDT       | ✅ Store gdtr/idtr to guest memory                 |
| LLDT / LTR        | ✅ Load LDT/TSS selector into ctx                  |
| SLDT / STR        | ✅ Store LDT/TSS selector from ctx                 |
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
| PUSH ESP / POP ESP emit host RSP instead of guest ESP | Host stack corruption; wrong value pushed/popped since ESP lives in `ctx->gp[4]` not a host register | C-helper `push_esp_helper` / `pop_esp_helper` that read/write `ctx->gp[GP_ESP]` directly |
| `emit_ea_to_r14` with ESP base emits `LEA R14D,[RSP+disp]` | All `[ESP]` / `[ESP+N]` memory accesses use host RSP instead of guest ESP — widespread correctness failure | Load guest ESP from `ctx->gp[GP_ESP]` into R14, then `LEA R14D,[R14+disp]` for displacement |
| MOV/ALU/LEA with ESP register operand executed natively on host RSP | `SUB ESP,N` / `ADD ESP,N` / `MOV ESP,r32` / `LEA ESP,[r+d]` corrupt host stack or produce wrong guest ESP | MOV ESP: read/write `ctx->gp[GP_ESP]` via `[R13+16]`. ALU ESP: load into R14, re-encode with R14, store back. LEA ESP: `emit_ea_to_r14` + `emit_store_r14_to_esp` |
| ALU/MOVZX/MOVSX/CMOVcc with ESP in reg field + memory operand | `ADD [mem],ESP` / `ADD ESP,[mem]` / `MOVZX ESP,BYTE [mem]` use host RSP encoding (reg=4 in ModRM) | Load guest ESP into R8D, emit with R8 (REX.R+reg=0), store R8D back if ESP was destination. `MOV ESP,imm32` → `MOV DWORD [R13+16], imm32` |
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

Implemented in `src/xbox/` component files with `xbox_setup()` in `src/xbox/setup.hpp`:
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
- **PIC**: full 8259A dual PIC (§5.18).
- **PIT**: full 8254 timer, ch0 → IRQ 0 (§5.18).
- **System Port B**: stub (timer 2 output bit).
- **Debug console**: I/O port 0xE9 (bochs-style putchar).

Test runner supports `--xbox` flag to use the full Xbox address map.
`MAX_IO_PORTS` increased from 16 to 32.

#### 5.16 NV2A GPU
**Priority: CRITICAL for games** — 16 MB of MMIO registers at `0xFD000000`.
Responsible for all graphics. The NV2A is an NV20-class GPU (GeForce 3 derivative).
This is by far the largest single implementation effort.

**PTIMER (freerunning counter)** ✅ DONE

`Nv2aState::ptimer_ns` is a 64-bit monotonic nanosecond counter, advanced by
100 ns per executor tick (via `hw_tick_callback`).  MMIO reads:

| Register           | Offset     | Behaviour                                 |
|--------------------|-----------|-------------------------------------------|
| `PTIMER_NUMERATOR` | 0x009200  | Read/write clock multiplier               |
| `PTIMER_DENOMINATOR`| 0x009210 | Read/write clock divider                  |
| `PTIMER_TIME_0`    | 0x009400  | Low 32 bits of counter (bits [4:0] = 0)   |
| `PTIMER_TIME_1`    | 0x009410  | High 29 bits of counter (ns >> 32)        |
| `PTIMER_INTR_0`    | 0x009100  | Interrupt status (stub: always 0)         |

This prevents Xbox kernel and game busy-wait loops that spin on PTIMER.

**PCRTC (vblank interrupt)** ✅ DONE

Periodic vblank interrupt (~60 Hz) from the NV2A CRTC:

- `PCRTC_INTR_0` (0x600100): bit 0 = vblank pending.  Write-1-to-clear.
- `PCRTC_INTR_EN_0` (0x600140): bit 0 = vblank interrupt enable.
- `PMC_INTR_0` (0x000100): read-only summary; bit 24 = PCRTC pending.
- `PMC_INTR_EN_0` (0x000140): master interrupt enable; bit 24 = PCRTC.
- **IRQ delivery**: when vblank fires and both PCRTC and PMC enables are set,
  the combined tick callback raises IRQ 1 on the PIC.  Games receive the
  interrupt, clear PCRTC_INTR_0 via W1C, send EOI to PIC.
- **Tick rate**: VBLANK_PERIOD = 16667 ticks (~60 Hz at 1 tick/trace).

**PFIFO (command FIFO engine)** ✅ DONE (DMA pusher + command parser + dedicated thread)

The DMA pusher reads NV2A push buffer commands from guest RAM and dispatches
them through a pluggable `MethodHandler` callback.  PFIFO runs on a dedicated
host thread (`src/xbox/nv2a_thread.hpp`), mirroring the real NV2A where the
PFIFO DMA engine operates independently from the CPU.  The guest CPU's only
interaction is writing `CACHE1_DMA_PUT`; the thread wakes via a condition
variable, drains the push buffer until `DMA_GET == DMA_PUT`, and goes back to
sleep.  This decouples GPU command processing from the CPU tick callback,
allowing true parallel execution.

Command format:

- **INCREASING** (type 0): `count` data dwords dispatched to consecutive
  methods starting at `method` (method, method+4, method+8, …).
- **NON_INCREASING** (type 4): `count` data dwords all dispatched to the
  same `method` (used for FIFO-style registers like vertex data submission).
- **JUMP** (bit [31:30] = 01): redirects DMA_GET to the target address.
- **NOP** (all-zero dword): silently skipped.

Each dispatched method calls `method_handler(user, subchannel, method, data)`.
When no handler is installed (default), methods are counted but discarded.
Diagnostic counters exposed as emulator extension registers:

| Extension Register    | Offset     | Description                      |
|-----------------------|-----------|----------------------------------|
| `FIFO_METHODS`       | 0x003F00  | Cumulative methods dispatched    |
| `FIFO_DWORDS`        | 0x003F04  | Cumulative dwords consumed       |
| `FIFO_JUMPS`         | 0x003F08  | Cumulative JUMP commands         |

Standard PFIFO registers:| Register              | Offset     | Behaviour                        |
|-----------------------|-----------|----------------------------------|
| `PFIFO_INTR_0`       | 0x002100  | Interrupt status (W1C)           |
| `PFIFO_INTR_EN_0`    | 0x002140  | Interrupt enable                 |
| `PFIFO_CACHES`       | 0x002500  | Reassign enable (bit 0)          |
| `PFIFO_MODE`         | 0x002504  | Per-channel DMA/PIO mode         |
| `CACHE1_PUSH0`       | 0x003200  | Push access enable (bit 0)       |
| `CACHE1_PUSH1`       | 0x003210  | Channel ID (bits 4:0)            |
| `CACHE1_DMA_PUSH`    | 0x003220  | DMA pusher enable (bit 0)        |
| `CACHE1_DMA_STATE`   | 0x003228  | Bit 0 = busy (GET ≠ PUT)        |
| `CACHE1_DMA_PUT`     | 0x003240  | DMA put pointer                  |
| `CACHE1_DMA_GET`     | 0x003244  | DMA get pointer                  |
| `CACHE1_PULL0`       | 0x003250  | Pull access enable (bit 0)       |
| `CACHE1_STATUS`      | 0x003214  | Bit 4 = empty (init: 0x10)       |
| `PFIFO_RUNOUT_STATUS`| 0x002400  | Bit 4 = empty (init: 0x10)       |

**PGRAPH (3D engine)** ✅ DONE (register stubs + state shadow)

Register stubs for the 3D graphics pipeline, plus a full PGRAPH state shadow
(`src/xbox/pgraph.hpp`) that captures GPU method calls from the PFIFO DMA
pusher.  The state shadow tracks:

- **Surface**: format, pitch, colour/zeta offsets, clip dimensions
- **Blend**: enable, src/dst factors, equation, colour
- **Depth/stencil**: test enable, function, mask, stencil ops
- **Rasterizer**: cull face, front face, shade mode, colour mask
- **Textures**: offset, format, control, filter, image rect (4 stages)
- **Vertex shader**: program upload (136 slots × 4 dwords), constant upload
  (192 × vec4)
- **Register combiners**: colour/alpha ICW/OCW (8 stages), specular/fog
- **Primitive**: BEGIN_END mode, draw count, clear count

The state shadow is wired via `Nv2aState::method_handler` so that every
method dispatched by the PFIFO thread updates the shadow automatically.
14-assertion C++ unit test (`src/test_pgraph.cpp`) verifies end-to-end:
push buffer commands → PFIFO parse → PGRAPH state capture.

| Register              | Offset     | Behaviour                        |
|-----------------------|-----------|----------------------------------|
| `PGRAPH_INTR`        | 0x400100  | Interrupt status (W1C)           |
| `PGRAPH_INTR_EN`     | 0x400140  | Interrupt enable mask            |
| `PGRAPH_FIFO`        | 0x400720  | FIFO access enable (bit 0)       |
| `PGRAPH_CTX_CONTROL` | 0x400170  | Channel context status           |

**PRAMDAC (PLL / video clock)** ✅ DONE (stub)

PLL coefficient registers for clock generation.  Defaults match the Xbox
retail hardware (crystal reference = 16.667 MHz):

| Register              | Offset     | Default      | Description           |
|-----------------------|-----------|--------------|-----------------------|
| `NVPLL_COEFF`        | 0x680500  | 0x00011C01   | GPU core PLL (~233 MHz) |
| `MPLL_COEFF`         | 0x680504  | 0x00011801   | Memory PLL (~200 MHz)   |
| `VPLL_COEFF`         | 0x680508  | 0x00031801   | Video pixel PLL (~25 MHz)|
| `PLL_TEST_COUNTER`   | 0x680514  | 0            | PLL test readback       |
| `GENERAL_CONTROL`    | 0x680600  | 0x00000101   | DAC control (read-only) |

#### 5.16a NV2A Rendering via Vulkan
**Priority: CRITICAL for visual output** — the NV2A GPU is emulated by
translating its fixed-function and programmable pipeline into Vulkan shaders
on the host GPU.  The design targets a **GPU-driven architecture** where the
CPU is entirely absent from the rendering data path after submitting dirty
pages and a Vulkan command buffer.

**Architecture overview:**

The NV2A is an NV20-class GPU (GeForce 3 derivative) with a fixed-function
transform & lighting pipeline, 4 texture combiners, vertex shaders (vs.1.1),
and register combiners for pixel shading.  Rather than software-rasterising
guest GPU commands, we translate them into equivalent Vulkan draw calls:

```
Guest push buffer (PFIFO DMA)
    │
    ▼
┌────────────────────┐
│  PFIFO Thread       │  Dedicated host thread; wakes on DMA_PUT write.
│  (host CPU thread)  │  Parses push buffer, dispatches NV097 methods.
└────────┬───────────┘
         │  (future: migrate to GPU compute shader)
         ▼
┌────────────────────┐
│  PGRAPH State       │  Shadow NV2A register state (combiners, textures,
│  Tracker            │  transforms, vertex formats, render targets)
└────────┬───────────┘
         │
         ▼
┌────────────────────┐
│  Shader Interpreter │  NV2A vertex shader → interpreted in SPIR-V VS/CS
│  (SPIR-V)           │  NV2A register combiners → interpreted in SPIR-V FS
└────────┬───────────┘
         │
         ▼
┌────────────────────┐
│  Vulkan Renderer    │  Dynamic state, GPU-generated draw calls
│                     │  Framebuffer → swapchain present
└────────────────────┘
```

---

**Memory model — single VkDeviceMemory allocation:**

The guest's 64 MB unified RAM is represented as one `VkDeviceMemory` allocation.
Multiple `VkResource` handles (buffers for vertex fetch / push buffer parsing,
images for textures) are bound to overlapping ranges of this allocation at
identity-mapped offsets — something Vulkan explicitly supports.

- A `VkBuffer` with `STORAGE_BUFFER | UNIFORM_TEXEL_BUFFER | SHADER_DEVICE_ADDRESS`
  usage covers the full 64 MB range for shader access.
- `VK_KHR_buffer_device_address` (core 1.2) provides a `uint64_t` GPU VA for the
  buffer.  Compute shaders dereference this directly via GLSL
  `buffer_reference` — the NV2A's flat address model maps one-to-one onto
  Vulkan's device address space, so surface/vertex/push-buffer pointers need
  no translation.
- On resizable-BAR hardware (all current discrete GPUs), the allocation uses
  `HOST_VISIBLE | DEVICE_LOCAL` and is **persistently mapped**.  Guest CPU writes
  from the JIT go directly into GPU memory at cache-line granularity with no
  driver involvement.  Dirty page tracking uses the page-version system (§2.6).
- On non-BAR hardware, a staging buffer with `vkCmdCopyBuffer` for dirty ranges
  provides the fallback path.

---

**Texture handling — zero-copy Morton sampling:**

The NV2A stores textures in Morton (Z-order) swizzled layout.  Vulkan can bind
a `VkImage` and a `VkBuffer` to the same `VkDeviceMemory` region at the same
offset.  Combined with `VK_EXT_image_drm_format_modifier`, a `VkImage` is
created with a custom memory layout describing the Morton swizzle pattern.  The
hardware sampler then traverses NV2A-format memory directly — **no deswizzle
compute shader, no texture pool, no copy of any kind.**

- The `VkImage` shares physical memory with the guest RAM buffer.  JIT guest CPU
  writes go into the persistently-mapped buffer; the `VkImage` view at the same
  offset reads the same bytes.
- DXT1/DXT3/DXT5 compressed textures map directly to `VK_FORMAT_BC1/BC2/BC3`
  (native hardware decode).
- Each mip level and cube face has its own base address in PGRAPH.  Each level
  is a separate `VkImage` bound to `xboxMemory` at the level's guest base address.
- **Fallback:** on drivers without the specific Morton DRM format modifier, a
  small CS writes directly into `VkImage` memory (not a separate staging
  buffer).  This CS is triggered only on dirty pages — in the common case the
  `VkImage` view is already valid.
- **Invalidation:** tracked via the page-version system (§2.6).  When a page
  containing texture data is written by the guest CPU, the texture is marked
  dirty and re-uploaded (modifier path) or re-deswizzled (fallback) on next GPU
  use.

---

**Pipeline state — extended dynamic state:**

`VK_EXT_extended_dynamic_state3` (core in Vulkan 1.3, universal desktop support)
exposes a `vkCmd*` call for nearly every NV2A pipeline state register, eliminating
PSO creation and state-object caching entirely:

| NV2A PGRAPH Register            | Vulkan Dynamic Command               |
|----------------------------------|--------------------------------------|
| `NV_PGRAPH_BLEND` (equation)    | `vkCmdSetColorBlendEquationEXT`      |
| `NV_PGRAPH_CONTROL_0` (alpha)   | `vkCmdSetAlphaToOneEnableEXT`        |
| `NV_PGRAPH_SETUPRASTER` (cull)  | `vkCmdSetCullModeEXT`                |
| `NV_PGRAPH_ZCOMPRESSOCCLUDE`    | `vkCmdSetDepthCompareOpEXT`          |
| `NV_PGRAPH_CONTROL_2` (stencil) | `vkCmdSetStencilOpEXT`               |
| `NV_PGRAPH_CONTROL_0` (fill)    | `vkCmdSetPolygonModeEXT`             |
| `NV_PGRAPH_CONTROL_0` (zbias)   | `vkCmdSetDepthBiasEnableEXT`         |
| `NV_PGRAPH_BLEND` (write mask)  | `vkCmdSetColorWriteMaskEXT`          |
| `NV_PGRAPH_BLEND` (logic op)    | `vkCmdSetLogicOpEXT`                 |

A pipeline is created once per render-pass configuration (colour format, depth
format, sample count) — a handful of variants total — and reused forever.
Zero PSO creation per draw, zero state-object cache, zero stalls on first-seen
state combinations.

---

**Push buffer replay on GPU:**

`VK_EXT_device_generated_commands` (cross-vendor since 2024: Nvidia, AMD, Intel)
allows a compute shader to write Vulkan command tokens into a buffer that the
GPU then executes directly, including state changes, descriptor updates, push
constant writes, and draw calls.

A CS parses the Xbox push buffer from guest RAM and emits one token per NV097
method:

- `NV097_SET_BLEND_EQUATION` → emit `TOKEN_SET_COLOR_BLEND_EQUATION`
- `NV097_DRAW_ARRAYS` → emit `VkDrawIndirectCommand` with topology-adjusted
  vertex count
- `NV097_SET_TEXTURE_OFFSET` → write texture descriptor into descriptor buffer
  via `VK_EXT_descriptor_buffer` (GPU-side descriptor updates, no CPU involvement)
- All other NV097 methods → corresponding dynamic state tokens or PGRAPH
  shadow writes

The CPU's per-frame sequence becomes:
1. `memcpy` dirty pages into persistently-mapped `xboxMemory` (near-free on BAR)
2. `vkCmdDispatch` push buffer parser CS
3. `vkCmdExecuteGeneratedCommandsEXT`
4. `vkQueueSubmit`

Every state change, descriptor update, and draw call within the frame is
determined and executed by the GPU.  The CPU issues ~6 Vulkan calls per frame.

---

**GPU-driven draw calls:**

The push buffer parser CS writes `VkDrawIndirectCommand` structs into a buffer.
`vkCmdDrawIndirectCount` executes all draws from this buffer in a single call —
the CPU never computes vertex counts or issues individual draw calls.

---

**Register combiner interpreter (fragment shader):**

The NV2A's 8-stage register combiner is translated to a GLSL fragment shader
ubershader that interprets raw PGRAPH register state at runtime.  This avoids
per-draw shader compilation — one precompiled shader handles all combiner
configurations.  Key design from the reference HLSL implementation:

- **Register file:** `vec4 Regs[16]` — all 16 PS register indices (ZERO, C0, C1,
  FOG, V0, V1, T0–T3, R0–R1, V1R0_SUM, EF_PROD) mapped to a flat array.
  Direct indexing replaces all switch-based register dispatch.  Writes to
  slot 0 (ZERO/DISCARD) are silently dropped.
- **Per-stage combiner:** Each stage decodes 4 packed uint32 registers from
  PGRAPH (COMBINECOLORI, COMBINEALPHAO, COMBINECOLORO, COMBINEALPHAO) into
  8 input bytes (A/B/C/D for RGB and alpha).  Each input byte encodes a 4-bit
  register index, 1-bit channel select (RGB vs alpha replicate), and 3-bit
  input mapping (unsigned identity/invert, expand, halfbias, signed identity,
  negate).  AB and CD products are computed (with optional dot-product mode),
  then MUX or SUM'd with bias/scale output mapping.
- **Input mapping:** 8 modes encoded in bits [7:5] of each input byte.
  Implemented as branchless movc chains — no switch, uniform wavefront
  execution.  Scalar `ApplyInputMappingScalar` variant for alpha path avoids
  ~12 wasted float ops per stage.
- **Output mapping:** Bias (subtract 0.5) and scale (×1/×2/×4/÷2) decoded
  once per stage via `exp2((m+1)&3 - 1)` — single SFU instruction.
- **NV2A math:** `nv2a_mul(a, b)` enforces 0 × anything = 0 per component,
  even when the other operand is inf or NaN.  Standard GPU math produces NaN
  for 0 × inf, breaking vertex transforms and combiner blending.
- **Final combiner:** EFG phase computes `E*F → EF_PROD` and
  `V1 + R0 → V1R0_SUM` (with optional complement/clamp), then ABCD phase
  produces `saturate(lerp(C, B, A) + D)` for RGB and `G` for alpha.
- **Texture stages:** 19 texture modes (PROJECT2D/3D, CUBEMAP, PASSTHRU,
  CLIPPLANE, BUMPENVMAP, BUMPENVMAP_LUM, BRDF, DOT_ST, DOT_ZW,
  DOT_RFLCT_DIFF, DOT_RFLCT_SPEC, DOT_STR_3D, DOT_STR_CUBE, DPNDNT_AR/GB,
  DOTPRODUCT, DOT_RFLCT_SPEC_CONST) with conditional pre-loads for source
  registers — early modes skip register reads entirely.
- **Dot mapping:** `ApplyDotMapping()` handles 8 modes (ZERO_TO_ONE,
  MINUS1_TO_1 D3D/GL/generic, HILO_1, HILO_HEMISPHERE D3D/GL/generic)
  with NV2A-accurate two's complement conversion.
- **Post-processing:** Per-stage `ApplyTexFmtFixup` (5 fixup modes for
  BGRA/ABGR/luminance/alpha-luminance/opaque-alpha), `PerformColorSign`
  (Xbox X_D3DTSS_COLORSIGN extension: expand [0,1]→[-1,1] or contract),
  `PerformColorKeyOp` (alpha/RGBA kill at 8-bit precision), `PerformAlphaKill`.
- **Alpha test:** Emulated in shader (`PerformAlphaTest`) since Vulkan has no
  fixed-function alpha test.  Quantizes both alpha and reference to 8-bit
  before D3DCMPFUNC comparison (NEVER/LESS/EQUAL/.../ALWAYS).
- **Fog:** Register file FOG slot carries fog color in RGB (from
  NV_PGRAPH_FOGCOLOR) and vertex fog factor in alpha.  Table fog modes
  (EXP, EXP2, LINEAR) computed per-vertex in the VS; result interpolated and
  blended in the PS.
- **Specialisation constants:** `NUM_STAGES` (1–8) and `NUM_TEXTURE_STAGES`
  (1–4) control loop unrolling.  The pipeline cache deduplicates variants.
- **Unique C0/C1 per stage:** When `PS_COMBINERCOUNT_UNIQUE_C0/C1` flags are
  set, each stage loads its own constant from the PGRAPH factor register array
  (COMBINEFACTOR0/1, stride 4 per stage).  Otherwise all stages share stage 0.
- **PGRAPH register access:** A `StructuredBuffer<uint>` (GLSL: SSBO) at
  binding t12 provides direct access to the full PGRAPH register file
  (2048 uint32, 8 KB).  Both VS and PS bind the same buffer.  Accessors:
  `PG_UINT(byteOff)`, `PG_FLOAT(byteOff)`, `PG_COLOR(byteOff)` (ABGR
  unpack: bits [0:7]=R, [8:15]=G, [16:23]=B, [24:31]=A → float4 RGBA).

---

**Vertex shader interpreter:**

Rather than recompiling each Xbox vertex shader program into host SPIR-V, a
single precompiled interpreter ubershader executes the raw NV2A microcode at
runtime.  Reference design from the HLSL implementation:

- **NV2A vertex ISA:** 136 instruction slots (XFPR — Transform Program RAM),
  each a 128-bit container with 92 bits of instruction data.  Dual-issue:
  MAC unit (13 opcodes: NOP/MOV/MUL/ADD/MAD/DP3/DPH/DP4/DST/MIN/MAX/SLT/SGE/ARL)
  and ILU unit (8 opcodes: NOP/MOV/RCP/RCC/RSQ/EXP/LOG/LIT) can execute in
  parallel.  Each instruction encodes 3 source inputs (A/B/C) with independent
  mux (R/V/C), register index, 8-bit swizzle, and negate bit, plus output
  destination with 4-bit writemask, temp register address, and output register
  address.
- **Interpreter state:** Per-vertex: `r[14]` (12 temp registers + 2 zero
  guards), `v[16]` (input attribute registers), `oRegs[16]` (output registers:
  oPos, oD0, oD1, oFog, oPts, oB0, oB1, oT0–oT3), `c[194]` (192 constants
  + 2 guards), `a0` (address register).
- **Guard register optimisation:** `r[]` and `c[]` are padded with zero-valued
  guard elements at both ends.  Runtime indices are biased by +1 and clamped,
  so out-of-bounds accesses (e.g. `c[-1]` via a0.x, or r13–r15) silently read
  zero — matching NV2A hardware behaviour without a branch.
- **Program start:** Read from `NV_PGRAPH_CSV0_C` register (CHEOPS_PROGRAM_START
  field, bits [15:8]).  Interpreter loops from start slot until FLD_FINAL bit
  or max 136 slots.
- **Dual-issue correctness:** When paired (both MAC and ILU active), all 3
  input values (A/B/C) are snapshot **before** either unit writes back.  ILU
  always writes to r1 when paired; otherwise shares the output address with MAC.
- **Context writes:** Output with `out_orb=0` writes back to the constant
  register shadow array (writable `c[]`), not to output registers.  Subsequent
  reads from the same program see the updated value.
- **ARL (Address Register Load):** `floor(a.x + 0.001)` with bias to compensate
  for GPU float precision on byte-normalised vertex attributes (NV2A normalises
  e.g. 17 to 17/255; GPU float may represent 16.9999 after ×255, yielding
  floor=16 without bias).
- **ILU special functions:**
  - `RCC`: sign-preserving clamp `clamp(|1/s|, 5.42e-20, 1.84e+19)` with sign
    bit copy.
  - `EXP`: returns `(2^floor(s), s-floor(s), 2^s, 1)` — exact floor (no ARL bias).
  - `LOG(0)`: returns `(-inf, 1, -inf, 1)` matching xemu consensus.
  - `LIT`: specular power clamped to ±(128 - 1/256).
- **Fog output remapping:** NV2A remaps oFog writes so the most significant
  masked component ends up in `.x` (the only component the rasterizer reads).
- **Output footer:** `reverseScreenspaceTransform(oPos)` reverses the Xbox
  screen-space scale+offset convention, `TEXCOORDINDEX` remapping routes
  texture coordinates to stages, and `xboxTextureScaleRcp` applies per-stage
  reciprocal texture coordinate scaling.

---

**Fixed-function vertex shader:**

For draws that don't use a programmable VS, a fixed-function pipeline shader
replicates NV2A's T&L hardware:

- **Vertex blending:** 1–4 world-view matrices blended with per-vertex weights
  from the weight attribute.  `VertexBlend_CalcLastWeight` computes the final
  weight as `1 - sum(prior weights)`.
- **Lighting:** Up to 8 lights (point, spot, directional) with per-light
  attenuation, range cutoff, spotlight falloff (`cos(alpha) - cos(phi/2)` /
  `cos(theta/2) - cos(phi/2)`).  Blinn-Phong specular with `SpecularEnable`
  zeroing specular colours when disabled.  Two-sided lighting: back-face
  uses `-Normal` for NdotL/NdotH.
- **ColorVertex:** Material sources (ambient/diffuse/specular/emissive) can
  read from vertex colour (D3DMCS_COLOR1/COLOR2) instead of material state,
  independently for front and back faces.  Only applied when the corresponding
  vertex register is declared present.
- **Fog:** Depth mode (Z, W, range-based, or vertex fog passthrough) and
  table mode (EXP, EXP2, LINEAR) computed per-vertex.

---

**Fixed-function pixel shader:**

For draws not using the register combiner (D3D8-style texture stage state):

- Up to 4 texture stages with D3DTEXTUREOP operations (22 ops: SELECTARG,
  MODULATE/2X/4X, ADD/ADDSIGNED/SUBTRACT/ADDSMOOTH, BLEND variants,
  DOTPRODUCT3, MULTIPLYADD, LERP, BUMPENVMAP).
- D3DTA arguments: DIFFUSE, CURRENT, TEXTURE, TFACTOR, SPECULAR, TEMP — with
  ALPHAREPLICATE and COMPLEMENT modifiers.
- Per-stage: color sign conversion, color key, alpha kill, bump environment
  matrix (2×2), format fixup (BGRA/ABGR/luminance/alpha-luminance/opaque-alpha).
- Alpha test emulated in shader.

---

**Passthrough vertex shader:**

For pre-transformed vertices (2D/HUD draws): vertex attributes are fetched from
the buffer, output registers assigned directly from input slots, and the screen-
space transform is reversed to produce clip-space coordinates.

---

**Vertex fetch and topology conversion:**

NV2A vertex data lives in guest RAM and is fetched directly via buffer device
address in the vertex shader.  The VS receives only `gl_VertexIndex`
(`SV_VertexID`) — all 16 vertex attributes are fetched from a storage buffer
containing the raw Xbox vertex stream data.

Per-attribute descriptors (byte offset, stride, format, stream base) drive a
format decode switch covering 20 Xbox vertex formats:

| Format           | Decode                                              |
|------------------|-----------------------------------------------------|
| FLOAT1/2/3/4     | Direct `uintBitsToFloat` loads                      |
| D3DCOLOR         | BGRA→RGBA byte unpack + normalise                   |
| SHORT2/4         | Signed 16-bit unnormalised → float                  |
| SHORT2N/4N       | Signed 16-bit normalised (÷32767)                   |
| NORMPACKED3 (CMP)| 11+11+10 signed packed: `(raw<<21)>>21` etc.        |
| PBYTE1/2/3/4     | Unsigned byte(s) normalised (÷255)                  |
| FLOAT2H          | Xbox "half" with W: {x,y,w} stored as 3 floats     |
| SHORT1/3/1N/3N   | 1 or 3 signed 16-bit values with W=1.0              |
| NONE             | Use sticky default value (NV2A register persistence) |

Topology conversion (no index buffer rewrite — VS computes source index from
host triangle vertex ID):

| NV2A Topology    | Host Topology | Index Arithmetic                     |
|------------------|---------------|--------------------------------------|
| Quad list        | Triangle list | 6 verts/quad: LUT `{0,1,2,0,2,3}`   |
| Triangle fan     | Triangle list | Apex=0, `(0, tri+1, tri+2)`          |
| Quad strip       | Triangle list | `{+0,+1,+2,+2,+1,+3}` per quad      |
| Line loop        | Line list     | `(seg, (seg+1) % N)` per segment     |
| Indexed (16/32)  | Passthrough   | Index buffer indirection + base vertex|

Unaligned reads are handled with shift-merge (`ReadU32`/`ReadU16`) since Xbox
vertex data is not guaranteed to be 4-byte aligned within streams.

---

**Shader caching:** NV2A programs are short (max 136 vertex shader instructions,
max 8 register combiner stages).  Compiled SPIR-V is cached keyed by the full
NV2A program state hash.  Cache hit → reuse `VkPipeline`; miss → compile + link.

---

**Framebuffer readback:** When the guest CPU reads from the framebuffer address
(`PCRTC_START`), a `VkBuffer` readback copies the rendered image into guest RAM
for CPU-side compositing or screenshot capture.

---

**Render pass architecture:**

Xbox games frequently render to a surface and then read it as a texture in the
same frame.  Vulkan render passes with explicit subpass dependencies declare
these transitions upfront, and `VK_DEPENDENCY_BY_REGION_BIT` keeps data in tile
cache between subpasses.  `VK_KHR_dynamic_rendering` (core 1.3) allows
attachment changes without pre-compiled `VkRenderPass` objects.

---

**Upscaling:**

- **Spatial upscalers** (bilinear blit, integer scale, FSR 1, NIS): no motion
  vectors required; work immediately.
- **Temporal upscalers** (FSR 2, FSR 3, XeSS, DLSS 3): require motion vectors
  and jitter injection.  All have native Vulkan SDKs.
  - **DLSS 3 frame generation** (Nvidia): available in Vulkan — combines
    temporal upscaling with optical-flow-based frame generation via
    `VK_NV_optical_flow`, effectively doubling output frame rate.
  - **FSR 3 frame generation** (AMD): also available in Vulkan — cross-vendor
    frame generation.
- **Jitter injection:** In the GPU-driven path, VS constants flow directly from
  guest CPU writes into guest RAM.  A two-pass approach handles this:
  (1) parser CS writes constants unconditionally; (2) a small serial CS scans
  completed constant writes for projection matrix candidates (perspective
  divide row, rotation column magnitudes, near/far ratio) and applies Halton
  jitter.  For known titles, a per-title override table lets the CPU write
  jitter directly into the mapped buffer before the parser runs.
- **Motion vectors:** The VS reprojects world-space vertex positions through
  the previous frame's VP matrix.  `prevVP` is maintained by copying the
  identified projection matrix slot at frame boundaries (64-byte `memcpy`).
- **Reactive mask:** The combiner interpreter fragment shader outputs a second
  attachment flagging reflection/emissive pixels for reduced temporal weight.
- **2D / HUD pass detection:** The `NV097_SET_TRANSFORM_EXECUTION_MODE` method
  in the push buffer identifies 2D passes.  Rendering switches to a native-
  resolution attachment via `vkCmdBeginRendering` parameter change.

---

**Texture replacement:**

- **Hashing:** xxHash64 over raw guest RAM bytes at the texture's surface
  address.  Computed on CPU from the persistently-mapped pointer with no GPU
  synchronisation.  Recomputed only when the texture's dirty page bit is set.
- **Replacement loading:** When a replacement asset is ready (loaded on a
  background thread), a standalone `VkImage` at replacement resolution with
  `VK_IMAGE_TILING_OPTIMAL` replaces the modifier-path image for that address.
  Descriptor buffer entry is updated atomically.
- **Non-replaced textures** continue using the modifier path (zero-copy).
- **Render target surfaces** skip the hash lookup — flagged when the surface
  address appears in a push buffer `NV097_SET_SURFACE_*` method.

---

**Per-frame CPU work summary:**

| Task                | Approach                                           |
|---------------------|----------------------------------------------------|
| Dirty page upload   | `memcpy` into BAR-mapped memory (near-free)        |
| Texture deswizzle   | None — VkImage reads NV2A memory directly           |
| Pipeline state      | GPU emits dynamic state tokens from push buffer     |
| Draw calls          | GPU writes+executes `VkDrawIndirectCommand`         |
| Vertex fetch        | VS from guest RAM device address                   |
| Topology convert    | In-shader `gl_VertexIndex` arithmetic              |
| Combiner interpreter| GLSL, specialisation constants                     |
| CPU per-frame calls | ~6 total                                           |

---

**Required Vulkan extensions:**

| Extension                              | Status              | Purpose                              |
|----------------------------------------|---------------------|--------------------------------------|
| `VK_KHR_buffer_device_address`         | Core 1.2            | Device address for guest RAM pointer |
| `VK_EXT_extended_dynamic_state3`       | Core 1.3 / universal| Replace PSO state objects            |
| `VK_EXT_image_drm_format_modifier`     | Universal desktop   | VkImage over Morton memory           |
| `VK_EXT_device_generated_commands`     | Nvidia/AMD/Intel    | GPU push buffer replay               |
| `VK_EXT_descriptor_buffer`             | Universal desktop   | GPU texture descriptor updates       |
| `VK_KHR_dynamic_rendering`            | Core 1.3            | Subpass-free render passes           |

All extensions marked "universal desktop" are available on RTX 20+, RX 5000+,
and Intel Arc.

---

**Implementation phases:**
1. Push buffer parser — decode NV2A methods from DMA push buffer (GPU-side CS)
2. PGRAPH state shadow — track all register combiner / vertex shader state
3. Vertex shader → SPIR-V compiler
4. Register combiner → SPIR-V fragment shader compiler
5. Vulkan render pass assembly — vertex buffers, textures, draw calls
6. Swapchain presentation — present rendered frame to host window
7. Upscaler integration — FSR 2/3, DLSS 3, jitter + motion vectors
8. Texture replacement — hash-based lookup, async background loading

---

**Known remaining NV2A items** (independent of Vulkan backend):
- `PSCompareMode` — per-stage 4-bit RSTQ clip-plane comparison (LT vs GE per
  component).  Reference implementation: `ApplyCompareMode()` in the RC
  interpreter's CLIPPLANE texture mode.
- `PSDotMapping` — 8 dot-product remapping modes (ZERO_TO_ONE, MINUS1_TO_1
  D3D/GL/generic, HILO_1, HILO_HEMISPHERE D3D/GL/generic).  Reference:
  `ApplyDotMapping()` and `ApplyDotMappingForStage()`.
- `PSInputTexture` — dependent-texture source stage routing (stage 2: 1-bit
  selector, stage 3: 2-bit selector, decoded from NV_PGRAPH_SHADERCTL bits
  12–27).  Reference: `GetSourceStage()`.
- `DOT_RFLCT_SPEC` — eye vector from VS output q-components (iT1.w, iT2.w,
  iT3.w), saved before DOTPRODUCT stages overwrite T registers.  Reference:
  `eyeVec` in the RC interpreter main().
- `DOT_RFLCT_SPEC_CONST` — simplified variant: eye=(0,0,1), saves one
  normalize() and one dot vs the general case.  Reference: implemented.
- Full `BUMPENVMAP` — perturbation via 2×2 BEM matrix (BUMPMAT00/01/10/11)
  from PGRAPH, with optional luminance (BUMPSCALE/BUMPOFFSET).  Reference:
  implemented in FetchTexture.
- Full `BRDF` mode — requires eye and light sigma vector inputs (stubbed in
  reference as simple 2D sample).
- Framebuffer readback — GPU→CPU path for guest CPU surface reads.
- `PREMODULATE` texture op — multiplies next stage's CURRENT by its texture.
- Fixed-function PS texture coordinate transforms (VS-side, per-stage
  TEXTURETRANSFORMFLAGS count + projected flag).
- Point sprite size attenuation (PointScaleABC in the fixed-function VS).

**Reference shader file inventory** (`doc/reference/`):

| File                                     | Purpose                             |
|------------------------------------------|-------------------------------------|
| `CxbxRegisterCombinerInterpreter.hlsl`   | RC ubershader (8-stage combiner + final combiner + texture fetch) |
| `CxbxRegisterCombinerInterpreterState.hlsli` | RC auxiliary cbuffer layout (C++/HLSL shared) |
| `CxbxVertexShaderInterpreter.hlsl`       | VS interpreter ubershader (136-slot NV2A microcode executor) |
| `CxbxVertexShaderInterpreterState.hlsli` | VS instruction field constants (C++/HLSL shared) |
| `CxbxFixedFunctionVertexShader.hlsl`     | Fixed-function T&L (lights, blending, materials, fog) |
| `CxbxFixedFunctionVertexShaderState.hlsli` | FF VS state block (transforms, lights, materials, modes) |
| `CxbxFixedFunctionPixelShader.hlsl`      | Fixed-function PS (D3DTEXTUREOP stages) |
| `CxbxFixedFunctionPixelShader.hlsli`     | FF PS state block (stage ops, args, bump env, fog, alpha test) |
| `CxbxVertexShaderPassthrough.hlsl`       | Pre-transformed vertex passthrough (2D/HUD) |
| `CxbxVertexFetch.hlsli`                 | Vertex fetch from ByteAddressBuffer (20 formats, topology) |
| `CxbxVertexShaderCommon.hlsli`           | VS I/O structs, fog formula, TEXCOORDINDEX |
| `CxbxVertexOutputFooter.hlsli`           | VS output: screen→clip, fog, texcoord remap, scale |
| `CxbxScreenspaceTransform.hlsli`         | Reverse Xbox screen-space transform |
| `CxbxPixelShaderInput.hlsli`             | PS_INPUT struct (matches VS_OUTPUT) |
| `CxbxPixelShaderFunctions.hlsli`         | Pure-math PS helpers (color sign, color key, alpha test, dot mapping, tex fmt fixup) |
| `CxbxPGRAPHRegs.hlsli`                  | PGRAPH register offsets + StructuredBuffer accessors |
| `CxbxNV2APixelShaderConstants.hlsli`     | PS_REGISTER/PS_CHANNEL/PS_INPUTMAPPING/PS_TEXTUREMODES constants (C++/HLSL shared) |
| `CxbxNV2AMathHelpers.hlsli`             | NV2A-accurate mul (0×anything=0), dot3, dot4 |

#### 5.17 APU (Audio Processing Unit) ✅ DONE (stub)

MCPX APU register stub at `0xFE800000` (4 MB).  The Xbox audio processor
has three sub-units: Voice Processor (VP), Global Processor (GP), and
Encode Processor (EP), plus a front-end and setup engine.

**Register blocks implemented:**

| Register              | Offset     | Behaviour                        |
|-----------------------|-----------|----------------------------------|
| `NV_PAPU_ISTS`       | 0x1000    | Interrupt status (W1C)           |
| `NV_PAPU_IEN`        | 0x1004    | Interrupt enable mask            |
| `NV_PAPU_FECTL`      | 0x1100    | Front-end control                |
| `NV_PAPU_FECV`       | 0x1104    | Front-end current voice (read)   |
| `NV_PAPU_FESTATE`    | 0x1108    | Front-end state (0 = idle)       |
| `NV_PAPU_FESTATUS`   | 0x110C    | Front-end status (0 = not busy)  |
| `NV_PAPU_SECTL`      | 0x2000    | Setup engine control             |
| `NV_PAPU_SESTATUS`   | 0x200C    | Setup engine status (0 = idle)   |
| `NV_PAPU_GPRST`      | 0x3000    | GP reset control                 |
| `NV_PAPU_GPISTS`     | 0x3004    | GP interrupt status (read)       |
| `NV_PAPU_GPSADDR`    | 0x3008    | GP scratch base address          |
| `NV_PAPU_EPRST`      | 0x4000    | EP reset control                 |
| `NV_PAPU_EPISTS`     | 0x4004    | EP interrupt status (read)       |
| `NV_PAPU_EPSADDR`    | 0x4008    | EP scratch base address          |

All registers read sane defaults (0 = idle/no interrupts).  Writes are
accepted silently.  `ISTS` uses write-1-to-clear semantics.  This is
sufficient for games that probe audio hardware during init and don't hang
waiting for DSP status transitions.

**PCI identity:** Bus 0, Device 3, Function 0 — Vendor 0x10DE, Device 0x01B0.

**Test:** `tests/apu.asm` — 12 assertions covering init defaults, R/W,
and W1C behaviour.

**Audio backend — Cubeb (cross-platform):**

Host audio output uses [Cubeb](https://github.com/mozilla/cubeb) (Mozilla's
cross-platform audio library, used by Firefox).  Cubeb provides a single C
callback API that wraps native backends:

| Platform | Cubeb backend |
|---|---|
| Windows | WASAPI |
| Linux | PulseAudio / ALSA |
| macOS | CoreAudio |
| Android | AAudio / OpenSLES |

Integration via CMake `FetchContent` (same pattern as Zydis).

The Cubeb stream model maps to the MCPX APU pipeline:

| MCPX sub-unit | Cubeb equivalent | Role |
|---|---|---|
| VP (Voice Processor) | Software mixer input (up to 256 voices) | Per-voice ADPCM decode, pitch shift, volume envelope |
| GP (Global Processor) | Mix buffer + DSP callback | Global mix, reverb/chorus DSP effects |
| EP (Encode Processor) | Output stream → host device | Final stereo/5.1 output |

**Mixbin architecture:**

The Xbox APU does **not** mix voices directly to speaker channels.  Instead,
each voice routes its output to one or more of 32 **mixbins** — numbered
intermediate accumulation buffers.  The GP DSP program then reads mixbins
as inputs and writes processed audio to the EP output channels.

Standard mixbin assignments (set by `DirectSound::SetMixBinVolumes`):

| Mixbin | Channel / Purpose |
|---|---|
| 0 | Front Left |
| 1 | Front Right |
| 2 | Center (5.1 only) |
| 3 | LFE / Subwoofer (5.1 only) |
| 4 | Rear Left (surround) |
| 5 | Rear Right (surround) |
| 6–19 | Xbox DSP effect sends (reverb, chorus, etc.) |
| 20–31 | Custom / game-defined |

Each voice descriptor contains a `MixBinVolumes[8]` array specifying
which mixbins receive output and at what volume (0x0000–0xFFFF linear).
A typical stereo voice writes to mixbins 0+1; a 3D-positioned voice
writes to mixbins 0–5 with computed panning coefficients.

**Implementation with Cubeb:**

The Cubeb output stream must be **multi-channel** (not just stereo) to
preserve the mixbin routing:

1. Query host channel layout via `cubeb_get_preferred_channel_layout()`.
2. Create output stream with matching channel count (stereo, 5.1, or 7.1).
3. In the software mixer:
   - Accumulate each active voice into its target mixbin buffers.
   - Apply per-voice `MixBinVolumes[]` coefficients during accumulation.
   - Run a minimal GP stub: sum mixbins 0–5 into the 6-channel output.
     Mixbins 6–19 (DSP sends) are processed by a software reverb/chorus
     if enabled, otherwise silently dropped.
4. Downmix to the host channel count if needed (e.g. 5.1→stereo).

This preserves spatial audio information through the pipeline rather than
premixing everything to stereo at the VP stage.

Implementation plan:
1. `cubeb_init()` on xbox_setup(), create a multi-channel output stream
   (48 kHz float32, channel count matching host, ~10 ms latency target).
2. VP voice activation: when guest writes a voice descriptor with
   `NV_PAPU_VOICE_ON`, add the voice to an internal active-voice list.
   Each voice tracks format (PCM8/PCM16/ADPCM), pitch, volume,
   buffer pointer into guest RAM, and mixbin routing (8 destinations +
   volumes from the voice descriptor).
3. GP mixing: the Cubeb data callback (fired from audio thread) iterates
   active voices, decodes/resamples, and accumulates into 32 mixbin
   buffers weighted by `MixBinVolumes[]`.  Mixbins 0–5 map to output
   channels; mixbins 6+ feed a software reverb/chorus if present.
   Single lock-free ring buffer for voice-list thread safety.
4. EP output: Cubeb writes the final mixed/downmixed buffer to the host
   audio device.  Downmix matrix applied if host has fewer channels
   than the active mixbin set (e.g. 5.1→stereo fold-down).
5. Tick integration: VP voice list scanned each frame (~60 Hz) to
   start/stop voices matching guest state changes.  No per-sample
   emulation of the DSP — software mixing in the Cubeb callback.

**Input backend — SDL2/SDL3:**

Controller and keyboard input uses SDL (Simple DirectMedia Layer).  SDL
provides cross-platform gamepad support with hot-plug, rumble, and
dead-zone handling:

| Platform | SDL backend |
|---|---|
| Windows | XInput / DirectInput |
| Linux | evdev / udev |
| macOS | IOKit / GameController.framework |
| Android | NDK input |

The Xbox has 4 USB gamepad ports.  Each maps to an SDL `GameController`
instance.  Guest reads gamepad state via the OHCI USB controller or XInput
HLE stubs — the input backend translates host button/axis state into the
Xbox gamepad report format (digital buttons, analog triggers, thumbsticks).

**UI framework — Dear ImGui:**

Debug/configuration overlay uses [Dear ImGui](https://github.com/ocornut/imgui).
ImGui integrates directly with the Vulkan rendering backend (same swapchain):

- Trace cache inspector (hit counts, hot traces)
- Register/memory watch windows
- GPU state viewer (NV2A PGRAPH, PFIFO status)
- Performance counters (traces/sec, JIT compile rate)
- Configuration: controller mapping, video settings, upscaling options

Rendered as an overlay pass after the guest framebuffer presentation.
No separate window system dependency — ImGui uses the existing Vulkan
device/swapchain created for NV2A rendering.

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
- **Integration**: `PicPair` struct in `src/xbox/pic.hpp`, wired to executor via
  `irq_check`/`irq_ack` function pointers.  Devices call `pic.raise_irq(N)`
  to assert lines.  Test-only I/O port 0xEB triggers IRQs for testing.

Remaining devices:

**8254 PIT (Programmable Interval Timer)** ✅ DONE

3-channel 8254 PIT (ports 0x40-0x43), channel 0 wired to IRQ 0 on the PIC:

- **Mode/command register** (port 0x43): channel select, access mode
  (lobyte/hibyte/both), operating mode, BCD/binary.
- **Operating modes**: mode 0 (one-shot interrupt on terminal count),
  mode 2 (rate generator / periodic), mode 3 (square wave).
- **Counter latch**: OCW latch command for reading counter mid-count.
- **Tick callback**: `pit_tick_callback` wired to executor's `tick_fn`,
  called once per trace dispatch.  Decrements ch0 counter; fires IRQ 0
  on the PIC when counter reaches zero.
- **Backward-edge linking suppressed**: `try_link_trace` skips backward
  edges (`target_eip ≤ source_eip`) so loops always return to the run
  loop for device ticks and IRQ delivery.

Remaining devices:
- SMBus — implemented (see below)
- IDE controller — implemented (see below)
- USB OHCI — implemented (see below)
- PCI configuration space — implemented
- MCPX boot ROM — implemented (see below)

**IDE Controller (ATA)** ✅ DONE (stub)

Dual-channel ATA controller (PCI Bus 0, Dev 9) with standard I/O ports:

- **Primary channel** (HDD): 0x1F0–0x1F7 (task file), 0x3F6 (control/alt status)
- **Secondary channel** (DVD): 0x170–0x177 (task file), 0x376 (control/alt status)

Task-file registers (R/W): Error, Features, Sector Count, LBA Low/Mid/High,
Device/Head.  Status register is read-only; Command register is write-only
(same port 0x1F7/0x177).

**ATA commands handled:**

| Command | Opcode | Behaviour |
|---|---|---|
| IDENTIFY DEVICE | 0xEC | Sets DRQ, serves 512-byte identify buffer via data port |
| IDENTIFY PACKET DEVICE | 0xA1 | Same (for ATAPI/DVD) |
| READ SECTORS | 0x20 | LBA28 PIO read — loads sector from backing image into data buf |
| WRITE SECTORS | 0x30 | LBA28 PIO write — receives sector via data port, stores to image |
| SET FEATURES | 0xEF | Succeeds immediately (status = 0x50) |
| INITIALIZE DEVICE PARAMETERS | 0x91 | Succeeds immediately |
| FLUSH CACHE | 0xE7 | Succeeds immediately |
| Unknown | * | Sets ERR + ABRT (status = 0x51, error = 0x04) |

**PIO data transfer:** 16-bit reads/writes on the data port (0x1F0 / 0x170)
transfer one word at a time from a 512-byte sector buffer.  IDENTIFY fills
the buffer from the identify table; READ SECTORS fills it from the backing
image; WRITE SECTORS collects writes and flushes to the image.  Multi-sector
transfers advance the LBA automatically.

**Disk backing:** Set `IdeChannel::image_data` / `image_size` to a raw
sector image (512 bytes per sector).  No image = zero-fill on read, discard
on write.

**Device identity:** Primary master = "XBOX HDD" (~8 GB LBA28), secondary
master = "XBOX DVD" (ATAPI CD-ROM).  Both report ATA-6, LBA capable.

**Software reset:** Writing SRST (bit 2) to control register resets
task-file to post-reset defaults (status = 0x50, error = 0x01).

**Test:** `tests/ide.asm` — 16 assertions covering init status, task-file
R/W, IDENTIFY with data port transfer (word 0 + full 256-word drain),
SET FEATURES, unknown command error, alternate status, and software reset.

**SMBus Controller** ✅ DONE

I/O ports 0xC000–0xC00F (MCPX), devices on the I²C bus:

- **EEPROM (24C02)** at address 0x54: 256-byte Xbox EEPROM with factory defaults
  — game region (NTSC-NA), serial number, MAC address, video standard
  (NTSC-M), language (English), DVD region.  Byte read and byte write.
- **SMC (PIC16LC)** at address 0x10: system management controller stub.
  Reports version (0xD0 = v1.0 retail), tray state (closed), CPU/MB
  temperatures, AV pack type (composite).  Write commands silently accepted.
- **Video encoder** stubs: Conexant CX25871 (0x45), Focus FS454 (0x6A).
- **Protocol**: address + command + data registers, control write triggers
  transaction, status W1C with done bit.

**USB OHCI Controllers** ✅ DONE (stub)

Two OHCI USB host controllers (PCI Bus 0, Dev 4 and Dev 5), each with a
4 KB MMIO register space:

- **USB0** at `0xFED00000`: gamepad ports 1-2
- **USB1** at `0xFED08000`: gamepad ports 3-4

OHCI operational registers:

| Register            | Offset | Behaviour                                |
|---------------------|--------|------------------------------------------|
| `HcRevision`        | 0x00   | Read-only: 0x0110 (OHCI 1.1)            |
| `HcControl`         | 0x04   | Host controller functional state          |
| `HcCommandStatus`   | 0x08   | Bit 0 = reset (auto-clear, re-inits)     |
| `HcInterruptStatus` | 0x0C   | Interrupt event bits (W1C)                |
| `HcInterruptEnable` | 0x10   | Enable mask                               |
| `HcInterruptDisable`| 0x14   | Write-only: clears enable bits            |
| `HcFmInterval`      | 0x38   | Frame interval (default 0x27782EDF)       |
| `HcFmRemaining`     | 0x3C   | Remaining bit-times (stub: returns ~10K)  |
| `HcRhDescriptorA`   | 0x48   | NDP = 2 (2 downstream ports)              |
| `HcRhPortStatus[N]` | 0x54+  | Per-port status (no devices attached)     |

PCI config: vendor 0x10DE, device 0x01C2, class 0x0C/0x03/0x10.
BAR0 pre-set to the MMIO base address.

12-assertion test (`tests/usb.asm`): register defaults, reset, W1C,
enable/disable, PCI vendor/device/BAR readback.

**Flash ROM / MCPX Boot ROM** ✅ DONE

Implemented in `src/xbox/flash.hpp`:

- **Flash ROM** (1 MB at PA `0xF0000000`, aliased at `0xFF000000`):
  read-only MMIO region containing the BIOS image.  Initialized to `0xFF`
  (empty).  `FlashState::load_bios(path)` loads a 256 KB image (mirrored
  4× to fill 1 MB) or a full 1 MB image from a file.
- **MCPX hidden ROM** (512 bytes at PA `0xFFFFFE00`): secret boot ROM baked
  into the MCPX southbridge die.  Contains the x86 reset vector at
  `0xFFFFFFF0`, which decrypts and jumps to the 2BL in flash.
  `FlashState::load_mcpx(path)` loads a 512-byte dump.  When loaded, reads
  in the `0xFFFFFE00–0xFFFFFFFF` range return MCPX data; otherwise they
  return normal flash content from the top of the 1 MB region.
- **HLE mode**: neither image is needed; the XBE loader patches the kernel
  thunk table directly.
- **LLE mode** (planned): load a real BIOS dump → execution starts at the
  BIOS entry point, or load an MCPX dump → execution starts at the reset
  vector `0xFFFFFFF0`.

4-assertion test (`tests/flash.asm`): 32-bit and sub-word reads from both
the flash base and BIOS shadow regions verify the default `0xFF` fill.

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
  - Memory: `ExAllocatePool`, `ExAllocatePoolWithTag`, `ExFreePool`,
    `MmAllocateContiguousMemory[Ex]`, `MmFreeContiguousMemory`,
    `MmGetPhysicalAddress`, `MmMapIoSpace`, `MmUnmapIoSpace`,
    `NtAllocateVirtualMemory`, `NtFreeVirtualMemory` (bump allocator at 16 MB+)
  - Threading: `KeGetCurrentThread` (fake KTHREAD ptr), `PsCreateSystemThread`
    (stub), `PsTerminateSystemThread`
  - I/O: `NtClose` (stub)
  - Display: `AvSetDisplayMode`, `AvGetSavedDataAddress` (stubs)
  - System: `HalReturnToFirmware` (halts guest), `DbgPrint`, `XeLoadSection`
  - Synchronisation: `KeInitializeEvent`, `KeInitializeMutex`,
    `KeInitializeSemaphore`, `KeInitializeTimerEx`, `KeSetEvent`,
    `KeResetEvent`, `KeReleaseMutex`, `KeReleaseSemaphore`,
    `KeWaitForSingleObject`, `KeWaitForMultipleObjects`,
    `NtCreateEvent`, `NtCreateMutant`, `NtCreateSemaphore`,
    `NtSetEvent`, `NtClearEvent`, `NtWaitForSingleObject`,
    `NtWaitForMultipleObjects`,
    `RtlInitializeCriticalSection`, `RtlEnterCriticalSection`,
    `RtlLeaveCriticalSection` (single-threaded stubs)
  - Interlocked: `InterlockedIncrement`, `InterlockedDecrement`,
    `InterlockedExchange`, `InterlockedCompareExchange`
  - Timers: `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeInitializeDpc` (stubs)
  - Timing: `KeQueryPerformanceCounter` (host RDTSC), `KeQueryPerformanceFrequency`
    (733 MHz), `KeQuerySystemTime` (RDTSC-derived 100 ns units), `KeTickCount`
  - Object manager: `ObReferenceObjectByHandle`, `ObDereferenceObject` (stubs)
  - String/memory: `RtlInitAnsiString`, `RtlInitUnicodeString`,
    `RtlCompareMemory`, `RtlCopyMemory`, `RtlMoveMemory`,
    `RtlFillMemory`, `RtlZeroMemory`, `RtlEqualString`
  - Unhandled ordinals: log + return `STATUS_NOT_IMPLEMENTED`
- **stdcall cleanup**: args are removed from guest stack by relocating the
  return address before the stub's RET instruction.

Test runner `--xbox` mode writes HLE stubs to guest RAM and installs the handler.
`tests/hle.asm` exercises 21 kernel calls: memory, interlocked ops, events,
synchronisation, timers, memory/string utilities, and MMIO mapping stubs.

#### 5.20 Kernel: Dual-Mode (HLE + LLE)

The executor supports two kernel modes, selectable at launch:

**Mode 1 — HLE (High-Level Emulation)** *(current default)*

- XBE loader patches the kernel thunk table with `INT 0x20` stubs.
- Each kernel API call traps into `default_hle_handler`, which reimplements
  the ordinal in C++ (see §5.19 for the current stub list).
- **Pros**: fast to develop, no kernel binary needed, easy to debug.
- **Cons**: incomplete — every new ordinal must be hand-implemented,
  subtle behaviour differences from the real kernel.

**Mode 2 — LLE (Low-Level Emulation)** *(planned)*

- Load the real `xboxkrnl.exe` (PE format) into guest RAM.
- Kernel does its own init: GDT/IDT setup, PCI enumeration, memory
  manager, scheduler, device drivers — all running as guest code.
- The executor already emulates the hardware the kernel needs:
  PIC, PIT, PCI config space, SMBus (EEPROM/SMC), NV2A MMIO, APU, IDE.

**PE Loader** (`src/pe_loader.hpp`) ✅ DONE

Generic Win32 PE (x86-32) loader for loading `xboxkrnl.exe` and other Xbox
system binaries into guest RAM:

- **DOS/PE header parsing**: validates `MZ` magic, locates PE signature,
  parses COFF header (must be `IMAGE_FILE_MACHINE_I386`) and PE32 optional
  header.
- **Section mapping**: iterates the section table and copies each section's
  raw data to `image_base + section_rva` in guest RAM.  Sections larger in
  memory than on disk are zero-filled (BSS).
- **No relocations**: images are loaded at their preferred base — the Xbox
  kernel is always linked at a fixed address.
- **Export resolution**: `resolve_export_by_ordinal()` parses the PE export
  directory to look up kernel function addresses by ordinal number.  This
  is used to wire up the kernel thunk table when running in LLE mode.
- **12-assertion C++ unit test** (`src/test_pe_loader.cpp`): constructs a
  minimal PE in memory with 2 sections and an export directory, writes it
  to a temp file, loads it, and verifies image base, entry point, section
  content, ordinal resolution, and error handling.

**LLE Boot Entry Shim** ✅ DONE

`test_runner --bios <bios.bin> [--mcpx <mcpx.bin>]` boots from a BIOS dump:

- Loads the BIOS image into `FlashState` (256 KB mirrored or 1 MB direct).
- Optionally loads a 512-byte MCPX ROM into the hidden ROM region.
- Initialises Xbox hardware via `xbox_setup()` (PCI, PIC, PIT, SMBus,
  NV2A, APU, IDE, USB, Flash).
- Begins execution at the x86 reset vector (`0xFFFFFFF0`) — the flash
  MMIO handler serves the first instruction from the BIOS image.
- The JIT runs in 32-bit protected mode; the BIOS is expected to set up
  its own GDT/IDT and transition to a known state.

All LLE infrastructure gaps are now closed:

- **Remaining gaps for LLE boot:**

| Gap | Difficulty | Status |
|---|---|---|
| SYSENTER / SYSEXIT | Easy | ✅ DONE |
| SYSENTER MSRs (CS/EIP/ESP) | Easy | ✅ DONE |
| PE loader for xboxkrnl.exe | Medium | ✅ DONE |
| MCPX/2BL boot chain (or entry shim) | Medium | ✅ DONE |
| OHCI USB controller stub | Medium | ✅ DONE |
| More complete GDT/TSS handling | Easy | ✅ DONE |
| MTRR MSRs | Easy | ✅ DONE |

**Hybrid operation:** Both modes can coexist.  The LLE kernel runs natively,
but a hook can intercept specific kernel exports and redirect them to HLE
stubs for unimplemented hardware paths (e.g. USB, network).  This allows
incremental bring-up: start with HLE, test with LLE, gradually remove stubs.

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
built, `try_link_trace(t)` patches every unlinked **forward** exit whose target
trace is already in the cache.  The previously executed trace `prev_trace` is
also re-checked.

**Backward edges** (`target_eip ≤ source trace's guest_eip`) are intentionally
**not linked**.  This guarantees that loops return to the run loop on every
iteration, enabling device ticks (PIT timer) and IRQ delivery at trace
boundaries.  A future downcount mechanism could allow backward linking while
still ensuring periodic run-loop re-entry.

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

#### 5.22 Fastmem Aliased Window ✅ DONE

The original design rejected a 4 GB guard-page / VEH-based fastmem window
(§2.3 — "No Guard Pages").  The inline `CMP R14, R15; JAE slow_path` pattern
remains the correct approach.  However, the NV2A tiling mirror region at
PA `0x0C000000`–`0x13FFFFFF` aliases main RAM, and with `R15 = GUEST_RAM_SIZE`
(128 MB = `0x08000000`), every mirror access fell through to the MMIO slow path —
a significant penalty for framebuffer / texture operations.

**Solution: Aliased fastmem window (320 MB).**

The fastmem window is enlarged to `FASTMEM_WINDOW_SIZE = 0x14000000` (320 MB) and
backed by platform memory aliasing so the mirror region maps to the same physical
pages as main RAM with zero overhead:

```
Offset          Size     Contents
──────────────  ───────  ────────────────────────────────────
0x00000000      128 MB   Main RAM (shared-memory section)
0x08000000       64 MB   Gap (committed zero-fill, no devices)
0x0C000000       64 MB   Mirror alias #1 → backing[0, 64 MB)
0x10000000       64 MB   Mirror alias #2 → backing[0, 64 MB)
```

`R15 = FASTMEM_WINDOW_SIZE` (320 MB) so `CMP R14, R15` passes for both main RAM
and mirror accesses, resolving to `OP [R12 + R14]` — a direct host dereference
with ~3 cycle overhead.  Addresses above 320 MB (NV2A MMIO at `0xFD000000`,
APU at `0xFE800000`, etc.) correctly take the MMIO slow path.

**Platform implementation** (`platform.hpp`):

| Platform       | Backing                  | Aliasing mechanism                                     |
|----------------|--------------------------|--------------------------------------------------------|
| Windows 10+    | `CreateFileMappingW`     | `VirtualAlloc2` placeholder + `MapViewOfFile3` per-region |
| Linux          | `memfd_create`           | `mmap MAP_SHARED|MAP_FIXED` at multiple offsets         |
| macOS / BSD    | `shm_open` + `unlink`   | `mmap MAP_SHARED|MAP_FIXED` at multiple offsets         |

Windows APIs are resolved at runtime via `GetProcAddress` (from `kernelbase.dll`)
so the emulator links against plain `kernel32.lib` and gracefully falls back to
a plain `VirtualAlloc` allocation (with MMIO mirror dispatch) on older Windows.

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
| Raw memory write to host VA | Fastmem window containment; PA >= ram_size (320 MB) → MMIO dispatch |
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
