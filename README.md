# Guided Executor

A JIT-based x86-32 CPU emulator targeting Pentium III class CPUs, running on
x86-64 hosts. Guest instructions execute almost natively — only privileged and
memory-sensitive instructions are rewritten. Host registers *are* guest registers,
with surgical interception points.

## How It Works

1. **Decode** — Zydis decodes guest 32-bit x86 instructions from guest RAM
2. **Classify** — Each instruction is categorized: clean, memory, privileged, or
   trace terminator
3. **Emit** — Clean instructions are copied verbatim; memory instructions get an
   inline fastmem/MMIO dispatch sequence; terminators produce trace exit stubs
4. **Execute** — An assembly trampoline loads guest registers into host registers,
   calls the JIT'd trace, and saves them back

Memory accesses below the RAM threshold go directly through a host pointer
(`[R12 + R14]`, ~3 cycle overhead). Accesses above take a slow path into MMIO
handlers. No guard pages, no VEH, no exception handling in the hot path.

## Project Structure

```
├── CMakeLists.txt              Build configuration (Zydis fetched via FetchContent)
├── README.md
├── doc/
│   └── DESIGN.md               Architecture, missing features, roadmap
└── src/
    ├── context.hpp              GuestContext struct, register indices
    ├── platform.hpp             OS memory allocation (RWX / RW)
    ├── mmio.hpp                 MMIO region dispatch table
    ├── code_cache.hpp           32 MB executable slab allocator
    ├── trace.hpp                Trace struct, TraceCache hash table
    ├── emitter.hpp              Byte emitter, EA synthesis, fastmem helpers
    ├── trace_builder.hpp        TraceBuilder, TraceArena, FaultBitmaps, SoftTlb
    ├── trace_builder.cpp        Decode → classify → emit loop
    ├── executor.hpp             Executor struct, dispatch_trace decl
    ├── executor.cpp             ASM trampoline (GCC/Clang), init, run loop
    ├── dispatch_trace.asm       MASM trampoline (MSVC)
    └── main.cpp                 Self-test: sum 1..10, fastmem round-trip
```

## Building

Requires CMake ≥ 3.20 and a C++20 compiler (MSVC, GCC, or Clang). Zydis v4.1.0
is fetched automatically.

```bash
cmake -B build -A x64
cmake --build build --config Release
```

On Linux / macOS (GCC or Clang):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running

```
./build/Release/guided_executor
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

The self-test runs a hand-assembled 32-bit guest program that sums integers 1..10,
stores/reloads via fastmem, and verifies all results.

## Current Status

Working:
- Trace building with Zydis decode (IA-32 mode)
- Verbatim copy of clean instructions (with INC/DEC re-encoding for x64)
- Inline fastmem dispatch for all common MOV sizes (8/16/32-bit)
- MMIO slow-path with platform-aware calling convention (SysV + Windows x64)
- PUSH/POP r32, CALL near, RET, JMP, all Jcc trace exits
- Trace cache with SMC page-version validation
- Cross-platform: GCC/Clang (inline asm) and MSVC (MASM)

See [doc/DESIGN.md](doc/DESIGN.md) for the full architecture, missing features
roadmap, and sandbox properties.

## Dependencies

| Dependency | Version | License |
|------------|---------|---------|
| [Zydis](https://github.com/zyantific/zydis) | v4.1.0 | MIT |

No other external dependencies. The code emitter is hand-rolled byte sequences.

## License

TBD
