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
| FPU / SSE     | **Not yet implemented** — would be host FPU/SSE   |
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
CMP R14, R15           (PA < ram_size?)
JAE slow_path          (predicted not-taken)
OP reg, [R12 + R14]    ← fastmem: direct host dereference, ~3 cycle overhead
JMP done
slow_path:
  save GP regs → ctx
  call mmio_dispatch_{read,write}(ctx, PA, gp_idx, size)
  load GP regs ← ctx
done:
```

### 2.4 Clean Instruction Handling

Instructions without memory operands or privilege requirements are copied verbatim
into the code cache (`memcpy`). Exception: short-form INC/DEC r32 (opcodes
`0x40`–`0x4F`) which collide with REX prefixes on x86-64 — these are re-encoded
to the two-byte `FF /0` / `FF /1` forms.

### 2.5 Self-Modifying Code Detection

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
| `emitter.hpp`         | Byte emitter, EA synthesis, fastmem helpers    | ✅ Working    |
| `trace_builder.hpp`   | `TraceBuilder`, `TraceArena`, `PageVersions`   | ✅ Working    |
| `trace_builder.cpp`   | Decode → classify → emit loop                  | ✅ Working    |
| `executor.hpp`        | `XboxExecutor` struct, `dispatch_trace` decl   | ✅ Working    |
| `executor.cpp`        | ASM trampoline (GCC/Clang), run loop           | ✅ Working    |
| `dispatch_trace.asm`  | MASM trampoline for MSVC                       | ✅ Working    |
| `main.cpp`            | Self-test: sum 1..10, fastmem round-trip       | ✅ PASS       |

---

## 4. What Works Today

- [x] Zydis-based instruction decode (LEGACY_32 mode, full operand detail)
- [x] Trace building: linear scan to first branch/call/ret, page-boundary stop
- [x] Verbatim copy of clean instructions (with INC/DEC re-encoding)
- [x] Inline fastmem dispatch for MOV r32←mem, MOV mem←r32 (8/16/32-bit)
- [x] MMIO slow-path call-out (platform-aware ABI: SysV and Windows x64)
- [x] PUSH/POP r32 with inline ESP management
- [x] CALL near (direct + register-indirect) trace exit
- [x] RET trace exit (stack read → `next_eip`, preserves live EAX)
- [x] JMP direct/indirect/memory trace exit
- [x] Conditional branch (all Jcc) trace exit with taken/fallthrough paths
- [x] Trace cache with open-addressed hash lookup
- [x] SMC page-version validation on trace re-entry
- [x] ASM trampoline for both GCC/Clang (inline asm) and MSVC (MASM)
- [x] Shadow space / calling convention handling for Windows x64 JIT→C calls
- [x] Self-test passes: EAX=55, EBX=0xDEADBEEF, ECX=0, RAM round-trip

---

## 5. What's Missing — Prioritized

### Phase 1: Run Real x86-32 Code (No Xbox Hardware)

These are needed to execute non-trivial x86-32 programs beyond the self-test.

#### 5.1 EFLAGS Preservation Across Slow Paths
**Priority: HIGH** — Currently, MMIO slow paths (save GP → call C → load GP)
clobber EFLAGS. Any instruction sequence where a flag-setting instruction is
followed by a conditional branch, with a memory access in between, will
malfunction if the memory access takes the slow path. Fix: save/restore EFLAGS
around the slow-path call (PUSHFQ/POPFQ or LAHF/SAHF for the subset).

#### 5.2 FPU / x87 / MMX / SSE State
**Priority: HIGH** — The Xbox uses x87 for all floating-point math and SSE1 for
SIMD. Every 3D game uses these extensively. The current executor doesn't touch
FPU/SSE state at all.

- x87 stack: 8×80-bit registers, TOP pointer, status/control words, tag word
- MMX: aliases to x87 (ST0–ST7 as MM0–MM7)
- SSE1: XMM0–XMM7 (128-bit), MXCSR

Since host FPU/SSE state is directly usable for the guest (same ISA), the
approach is:
1. Save/restore host FPU/SSE state on executor entry/exit (FXSAVE/FXRSTOR or
   XSAVE/XRSTOR)
2. Let x87/MMX/SSE instructions run natively (verbatim copy in trace builder)
3. Memory-operand forms (e.g., `MOVAPS [mem], XMM0`) still need the inline
   fastmem dispatch pattern — extended to handle XMM register encodings

#### 5.3 String Instructions (REP MOVS/STOS/CMPS/SCAS/LODS)
**Priority: HIGH** — Used by memcpy/memset/strlen and compiler intrinsics.
Must be unrolled into a counted dispatch loop at rewrite time so each element
access goes through the fastmem/MMIO dispatch. See design §2.3 "REP MOVSD"
example in the preceding discussion.

#### 5.4 Segment Register Support (FS/GS Overrides)
**Priority: HIGH** — The Xbox kernel uses FS for KPCR (Kernel Processor Control
Region). Currently, `emit_ea_to_r14` rejects FS/GS segment overrides. Fix: add
a FS/GS base field to `GuestContext`, and emit an ADD of the segment base into
R14 during EA synthesis when a non-default segment is present.

#### 5.5 8-bit and 16-bit Register Operands in Memory Dispatch
**Priority: MEDIUM** — `emit_mem_dispatch` currently only handles 32-bit register
operands (`reg32_enc`). Instructions like `MOV AL, [mem]` or `MOV [mem], CX`
need 8-bit and 16-bit register encoding support. Also need MOVZX/MOVSX memory
forms.

#### 5.6 Immediate-to-Memory Instructions
**Priority: MEDIUM** — `MOV DWORD PTR [mem], imm32` has no register operand;
`emit_mem_dispatch` fails because `reg_op_idx` has no register to extract.
Need a separate path that synthesizes the EA, does the fastmem check, then
writes the immediate directly.

#### 5.7 Multi-Memory-Operand Instructions
**Priority: MEDIUM** — Instructions like `XCHG [mem], reg` or `CMPXCHG [mem], reg`
have both a read and a write to the same memory location. LEA r, [mem] has a
memory-syntax operand that doesn't actually access memory. The classifier needs
refinement.

#### 5.8 CALL/JMP Indirect Through Memory
**Priority: MEDIUM** — `JMP [mem]` is partially handled; `CALL [mem]` (vtable
dispatch) is not. C++ games hit this constantly.

#### 5.9 PUSH/POP Immediate and Memory Forms
**Priority: MEDIUM** — Only PUSH/POP r32 are implemented. Also need:
- `PUSH imm8/imm32`
- `PUSH [mem]` / `POP [mem]`
- `PUSHA` / `POPA` (used by some Xbox code)

#### 5.10 Privileged Instruction Handling
**Priority: MEDIUM** (for kernel boot) — `handle_privileged()` currently halts.
Needs real emulation for each opcode class:

| Instruction       | Emulation needed                                   |
|-------------------|----------------------------------------------------|
| RDMSR / WRMSR     | MSR table (APIC_BASE, SYSENTER_*, etc.)            |
| MOV CRn, r / r, CRn | Update ctx->cr0/cr2/cr3/cr4                     |
| IN / OUT          | I/O port dispatch table                            |
| LGDT / LIDT       | Update ctx->gdtr/idtr_base/limit                  |
| LLDT / LTR        | Update ctx->ldtr/tr                                |
| CLI / STI         | Toggle ctx->virtual_if                             |
| PUSHF / POPF      | Merge virtual_if into/from EFLAGS image            |
| IRET              | Pop EIP/CS/EFLAGS from guest stack, restore IF     |
| INVLPG            | Invalidate shadow TLB entry (once paging exists)   |
| CPUID             | Return Xbox-appropriate CPUID leaves               |
| RDTSC             | Return scaled/offset TSC value                     |
| HLT               | Idle until next interrupt                          |

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
Running guest from PA 00001000 ...
ctx.gp[EAX]  = 55        (expected 55)
ctx.gp[EBX]  = 0xDEADBEEF (expected 0xDEADBEEF)
ctx.gp[ECX]  = 0          (expected 0)
RAM[0x4000]  = 55         (expected 55)
RAM[0x4004]  = 55         (expected 55)

PASS
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
