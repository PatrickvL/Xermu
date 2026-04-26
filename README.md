# Guided Executor

A JIT-based x86-32 CPU emulator targeting original Xbox (Pentium III / NV2A)
hardware, running on x86-64 hosts. Guest instructions execute almost natively —
only privileged and memory-sensitive instructions are rewritten. Host registers
*are* guest registers, with surgical interception points.

The goal is full Xbox emulation: boot the BIOS, load XBE executables, emulate
the NV2A GPU, APU, and all system peripherals — enough to run the Xbox Dashboard
and retail games.

## How It Works

1. **Decode** — Zydis decodes guest 32-bit x86 instructions from guest RAM
2. **Classify** — Each instruction is categorized: clean, memory, privileged, or
   trace terminator
3. **Emit** — Clean instructions are copied verbatim; memory instructions get an
   inline fastmem/MMIO dispatch sequence; terminators produce trace exit stubs
4. **Execute** — An assembly trampoline loads guest registers into host registers,
   calls the JIT'd trace, and saves them back

A 4 GB fastmem window covers the full 32-bit guest physical address space.
RAM is mapped at `[0, 128 MB)`; MMIO ranges are left `PAGE_NOACCESS` and caught
by a VEH fault handler that decodes the faulting instruction and dispatches to
the appropriate device emulator. Clean memory accesses go through
`[R12 + R14]` (~3 cycle overhead).

## Project Structure

```
├── CMakeLists.txt                Build config (Zydis via FetchContent, NASM tests)
├── README.md
├── doc/
│   ├── DESIGN.md                 Architecture, roadmap, working document
│   ├── PROGRESS.md               Step-by-step development log
│   └── NV2A_Emulation_Vulkan.md  NV2A GPU emulation design notes
├── data/
│   ├── xbox5838.bin              Xbox BIOS ROM (256 KB)
│   └── xbox dash orig_5960/      Dashboard XBE + assets
└── src/
    ├── context.hpp               GuestContext struct, register indices, StopReason
    ├── platform.hpp              OS memory (4 GB fastmem window, RWX slabs)
    ├── mmio.hpp                  MMIO region dispatch table
    ├── code_cache.hpp            32 MB executable slab allocator
    ├── trace.hpp                 Trace struct, TraceCache hash table
    ├── emitter.hpp               Byte emitter, EA synthesis, fastmem helpers
    ├── trace_builder.hpp         TraceBuilder, TraceArena, FaultBitmaps, SoftTlb
    ├── trace_builder.cpp         Decode → classify → emit loop
    ├── executor.hpp              Executor struct, dispatch_trace decl
    ├── executor.cpp              Run loop, VEH fault handler, interrupt delivery
    ├── fault_handler.hpp         VEH-based MMIO fault decode & dispatch
    ├── insn_dispatch.hpp         Instruction-level MMIO read/write dispatch
    ├── dispatch_trace.asm        MASM trampoline (MSVC)
    ├── main.cpp                  BIOS boot / XBE loader entry point
    ├── pe_loader.hpp             Win32 PE loader (for testing with native PEs)
    ├── xbe_loader.hpp            XBE loader + HLE kernel (xboxkrnl.exe stubs)
    ├── xbox.hpp                  Xbox hardware constants and memory map
    ├── xbox/
    │   ├── address_map.hpp       Physical address map definitions
    │   ├── setup.hpp             Hardware init (PCI, MMIO ranges, RAM mirrors)
    │   ├── nv2a.hpp              NV2A GPU register emulation
    │   ├── nv2a_thread.hpp       NV2A PGRAPH command processor thread
    │   ├── pgraph.hpp            PGRAPH state shadow + register dispatch
    │   ├── apu.hpp               APU (audio) register emulation
    │   ├── flash.hpp             Flash ROM controller
    │   ├── ide.hpp               IDE/ATA disk controller
    │   ├── usb.hpp               USB OHCI host controller
    │   ├── smbus.hpp             SMBus controller (SMC, EEPROM, video encoder)
    │   ├── pic.hpp               Dual 8259 PIC emulation
    │   ├── pit.hpp               8254 PIT timer
    │   ├── ioapic.hpp            I/O APIC
    │   ├── pci.hpp               PCI config space
    │   ├── ram_mirror.hpp        RAM mirror/tiling logic
    │   └── misc_io.hpp           Miscellaneous I/O ports
    ├── test_pe_loader.cpp        PE loader unit test
    ├── test_pgraph.cpp           PGRAPH state unit test
    └── tests/
        ├── harness.inc           NASM test macros (ASSERT_EQ, PASS, etc.)
        ├── test_runner.cpp       Test runner: loads .bin, runs in JIT, checks exit
        └── *.asm                 50 NASM test files (ALU, FPU, SSE, MMIO, HLE, …)
```

## Building

Requires CMake ≥ 3.20 and a C++20 compiler (MSVC, GCC, or Clang). Zydis v4.1.0
is fetched automatically. NASM is required for assembling test binaries.

```bash
cmake -B build -A x64 -DNASM_EXECUTABLE="C:/tools/nasm/nasm-2.16.03/nasm.exe"
cmake --build build --config Release
```

On Linux / macOS (GCC or Clang):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running

### Self-test
```
./build/Release/guided_executor
```

### Test suite (52 tests)
```bash
ctest --test-dir build -C Release --output-on-failure
```

Tests cover: ALU, FPU, SSE, memory, flow control, string ops, segments, paging,
interrupts, privileged instructions, Xbox hardware (PIC, PIT, SMBus, NV2A GPU,
APU, IDE, USB, Flash, IOAPIC, PCI, PFIFO, PCRTC, PRAMDAC, PVIDEO, PBUS),
HLE kernel stubs, fastmem, block linking, and more.

### Xbox Dashboard (XBE)
```
./build/Release/guided_executor --xbe data/xbox\ dash\ orig_5960/xboxdash.xbe
```

Currently executes 305+ HLE kernel calls with zero unhandled ordinals. The
dashboard performs file I/O, thread creation, interrupt setup, and critical
section management through emulated kernel stubs.

## Current Status

**JIT engine:**
- Trace building with Zydis decode (IA-32 mode)
- Verbatim copy of clean instructions (with INC/DEC re-encoding for x64)
- Inline fastmem dispatch for all MOV sizes (8/16/32-bit)
- 4 GB fastmem window with VEH-based MMIO fault handling
- PUSH/POP, CALL, RET (including RET imm16), JMP, all Jcc
- High-byte register (AH/CH/DH/BH) memory access via C helpers
- Block linking with trace cache and SMC page-version validation
- Cross-platform: GCC/Clang (inline asm) and MSVC (MASM)

**Xbox hardware emulation:**
- NV2A GPU: PMC, PCRTC, PTIMER, PRAMDAC, PVIDEO, PBUS, PFIFO, PGRAPH shadow
- APU with voice processor registers
- IDE/ATA with DMA support
- USB OHCI host controller
- Flash ROM controller with command sequences
- Dual 8259 PIC, 8254 PIT, I/O APIC
- SMBus (SMC, EEPROM, video encoder)
- PCI configuration space
- Full Xbox physical address map with RAM mirrors

**HLE kernel (xboxkrnl.exe):**
- 60+ stubbed kernel exports covering memory, threading, sync, I/O, HAL, Rtl
- HLE stub architecture: stubs at 0x80000, INT 0x20 intercept for dispatch
- Kernel data exports (KeTickCount, XboxHardwareInfo, etc.) at 0x81000
- KPCR/KTHREAD structures for TLS and exception handling
- XBE section loading with kernel thunk patching

See [doc/DESIGN.md](doc/DESIGN.md) for the full architecture and roadmap,
and [doc/PROGRESS.md](doc/PROGRESS.md) for the development log.

## Dependencies

| Dependency | Version | License |
|------------|---------|---------|
| [Zydis](https://github.com/zyantific/zydis) | v4.1.0 | MIT |
| [NASM](https://www.nasm.us/) | ≥ 2.16 | BSD-2-Clause |

NASM is only needed for assembling test binaries. The code emitter is hand-rolled
byte sequences — no external assembler at runtime.

## License

TBD
