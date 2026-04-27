# nboxkrnl Integration Plan

Integration of [nboxkrnl](https://github.com/ergo720/nboxkrnl) as a drop-in
replacement for the HLE kernel stub machinery.  Each milestone is a separate
commit with its own test and documentation update.

> **Host-side work first.** Every milestone below can be built, tested, and
> committed without nboxkrnl present.  The guest PE is loaded only in the final
> milestones.

---

## Background

### What is nboxkrnl?

nboxkrnl is a GPL-2.0 re-implementation of the original Xbox kernel
(`xboxkrnl.exe`).  It compiles as a **32-bit Windows Native PE** with:

```
/BASE:0x80010000  /SUBSYSTEM:NATIVE  /ENTRY:KernelEntry
```

It runs **inside the guest address space** at exactly the same address as the
real kernel.  It sets up its own GDT, IDT, TSS, page tables, and accesses all
Xbox hardware directly through standard I/O ports and MMIO — PCI config
(0xCF8/0xCFC), PIC (0x20/0xA0), PIT (0x40), SMBus (0xC000), NV2A (0xFD000000),
etc.

### How it talks to the host

nboxkrnl communicates with the host emulator through **custom I/O ports
0x200–0x210** that don't conflict with any real Xbox hardware:

| Port    | Name                | Direction | Purpose |
|---------|---------------------|-----------|---------|
| 0x200   | `DBG_STR`           | Write     | Guest VA of a debug string (512 bytes max) |
| 0x201   | `MACHINE_TYPE`      | Read      | Console type: 0=Xbox, 1=Chihiro, 2=Devkit |
| 0x202   | `ABORT`             | Write     | Terminate emulation |
| 0x203   | `CLOCK_INCREMENT_LOW`  | Read   | Low 32 bits of clock increment (100 ns units) |
| 0x204   | `CLOCK_INCREMENT_HIGH` | Read   | High 32 bits of clock increment |
| 0x205   | `BOOT_TIME_MS`      | Read      | Elapsed time since boot in milliseconds |
| 0x206   | `IO_START`          | Write     | Submit an IoRequest packet (guest VA) |
| 0x207   | `IO_RETRY`          | Write     | Flush pending I/O packets |
| 0x208   | `IO_QUERY`          | Write     | Query an IoInfoBlock for completion (guest VA) |
| 0x20A   | `IO_CHECK_ENQUEUE`  | Read      | Returns 1 if pending I/O packets exist |
| 0x20D   | `XE_XBE_PATH_LENGTH`| Read      | Length of XBE launch path |
| 0x20E   | `XE_XBE_PATH_ADDR`  | Write     | Guest VA to receive XBE path string |
| 0x20F   | `ACPI_TIME_LOW`     | Read      | Low 32 bits of ACPI timer (3.579545 MHz) |
| 0x210   | `ACPI_TIME_HIGH`    | Read      | High 32 bits of ACPI timer |

### Why the current HLE stubs are insufficient

The HLE path intercepts XBE kernel thunk calls via `INT 0x20` stubs and fakes
return values.  This requires implementing every kernel API individually —
currently ~100 stubs with many returning wrong data.  The dashboard calls
`HalReturnToFirmware(ReturnFirmwareFatal)` because D3D initialisation never
actually touches NV2A.  nboxkrnl solves this by running the *real* kernel code
that programs hardware correctly.

### What Xermu already has

The guided executor already implements nearly everything nboxkrnl needs:

- ✅ Protected mode with paging (CR0, CR3, CR4)
- ✅ GDT/IDT/TSS loading (`LGDT`, `LIDT`, `LTR`)
- ✅ Exception delivery through IDT (`INT n`, `#PF`, `#GP`, `#UD`)
- ✅ `IRET` — pops EIP/CS/EFLAGS from guest stack
- ✅ Segment register loading with GDT base lookup (FS for KPCR)
- ✅ Full page table walk (32-bit non-PAE, 4 KB + 4 MB pages, A/D bits)
- ✅ `CLI`/`STI` via `virtual_if` flag
- ✅ `IN`/`OUT` via registered callbacks
- ✅ PIC, PIT, PCI, SMBus, IDE, ACPI I/O ports
- ✅ NV2A MMIO (16 MB @ 0xFD000000)
- ✅ IRQ delivery (`irq_check` / `irq_ack` / `deliver_interrupt`)
- ✅ 4 GB fastmem window with VEH MMIO dispatch

### What we need to add (this plan)

| # | Feature | Effort | Milestone |
|---|---------|--------|-----------|
| 1 | Guest memory block read/write helpers | Small | M1 |
| 2 | Host-kernel I/O port handlers (ports 0x200–0x210) | Small | M2 |
| 3 | Host-side asynchronous file I/O system | Medium | M3 |
| 4 | Host-side XBE path + disk partition setup | Small | M4 |
| 5 | `--nboxkrnl` boot mode: PE loader + page tables | Medium | M5 |
| 6 | EEPROM/certificate key pass-through | Small | M6 |
| 7 | End-to-end integration test | Small | M7 |

---

## Milestone 1 — Guest Memory Block Read/Write Helpers

**Goal:** Provide `read_guest_block(ctx, guest_va, size, host_buf)` and
`write_guest_block(ctx, guest_va, size, host_buf)` functions that copy
arbitrary-length byte ranges between host buffers and guest virtual memory,
correctly handling page table translation and page boundary crossings.

### Why

nboxkrnl communicates file I/O through structured packets in guest RAM.  The
host must read `IoRequest` structs (up to 44 bytes) from guest virtual
addresses, read file path strings (up to 256 bytes), and write `IoInfoBlock`
results (up to 36 bytes) and file data (arbitrary length) back to guest memory.

The existing `read_guest_mem32` / `write_guest_mem32` in `jit_helpers.cpp`
only handle aligned 4-byte accesses.  `translate_va` in `executor.cpp` handles
single addresses.  We need a general block-copy function that:

1. Translates the starting VA through the page tables (via `translate_va`)
2. Copies up to the end of the current 4 KB page
3. Translates the next page's VA and continues
4. Repeats until the full block is transferred

### Implementation

**File:** `src/cpu/executor.hpp` + `src/cpu/executor.cpp`

```cpp
// Read 'size' bytes from guest virtual address 'va' into 'buf'.
// Handles page boundary crossings via translate_va().
// Returns false if a page fault would occur.
bool read_guest_block(uint32_t va, uint32_t size, void* buf);

// Write 'size' bytes from 'buf' to guest virtual address 'va'.
// Handles page boundary crossings via translate_va().
// Returns false if a page fault would occur.
bool write_guest_block(uint32_t va, uint32_t size, const void* buf);
```

**Algorithm:**

```
while remaining > 0:
    pa = translate_va(va, is_write)
    if pa >= ram_size: return false   // fault
    chunk = min(remaining, 0x1000 - (pa & 0xFFF))
    memcpy(dst, ram + pa, chunk)      // or reverse for write
    va += chunk
    remaining -= chunk
```

When paging is disabled (`CR0.PG` = 0), the VA is used as-is (identity map).
For addresses in the contiguous memory region (0x80000000–0x83FFFFFF), the
translation subtracts 0x80000000 to get the physical address — the page table
walk already handles this via the kernel-installed PDEs.

### Test

Add a unit test in `src/tests/test_block_rw.cpp`:

1. Set up the executor with paging enabled (identity-map 64 MB + 0x80000000
   mirror, matching nboxkrnl's initial page table layout)
2. Write a known pattern to guest VA 0x80020000 via `write_guest_block`
3. Read it back via `read_guest_block` and verify
4. Test page-boundary crossing: write 8 bytes starting at 0x80020FFE
5. Test with a 4 MB large-page PDE

Add a CMake target `test_block_rw` and a brief entry in `doc/PROGRESS.md`.

### Commit

```
nboxkrnl M1: guest memory block read/write helpers

Add read_guest_block() and write_guest_block() to Executor for copying
arbitrary-length byte ranges between host buffers and guest virtual memory.
Handles page table translation and page boundary crossings.  These are
needed by the nboxkrnl host I/O port handlers to read IoRequest packets
and write completion results.

Includes test_block_rw with identity-mapped and large-page scenarios.
```

---

## Milestone 2 — Host-Kernel Communication I/O Ports

**Goal:** Register I/O port handlers for ports 0x200–0x210 that implement the
nboxkrnl ↔ host protocol for debug output, time queries, machine type, and
abort.  File I/O ports are wired but stubbed (completed in M3).

### Why

When nboxkrnl boots, the very first host interaction is reading
`MACHINE_TYPE` (port 0x201) to determine the console type.  Shortly after,
it reads the clock increment to initialise the time subsystem, and writes
debug strings.  These must work before file I/O.

### Implementation

**New file:** `src/xbox/nboxkrnl_host.hpp`

This file contains the I/O port read/write callbacks and state:

```cpp
struct NboxkrnlHostState {
    // Timing
    std::chrono::steady_clock::time_point boot_time;
    uint64_t last_clock_us = 0;
    uint64_t lost_clock_increment = 0;

    // File I/O (M3)
    // ... forward-declared, implemented in M3

    // XBE path
    std::string xbe_path;       // Xbox-format path, e.g. "\Device\CdRom0\default.xbe"

    // Pointer back to executor (for read/write_guest_block)
    Executor* exec = nullptr;
};
```

**Port handlers:**

| Port | Read handler | Write handler |
|------|-------------|---------------|
| 0x200 `DBG_STR` | — | Read 512 bytes from guest VA in `val`, print to stderr |
| 0x201 `MACHINE_TYPE` | Return 0 (Xbox) | — |
| 0x202 `ABORT` | — | Set `ctx.halted = true` |
| 0x203 `CLOCK_INCREMENT_LOW` | Compute elapsed time, return low 32 bits | — |
| 0x204 `CLOCK_INCREMENT_HIGH` | Return high 32 bits of cached increment | — |
| 0x205 `BOOT_TIME_MS` | Return elapsed ms since boot | — |
| 0x206 `IO_START` | — | Stub: log warning (completed in M3) |
| 0x207 `IO_RETRY` | — | Stub: NOP (completed in M3) |
| 0x208 `IO_QUERY` | — | Stub: NOP (completed in M3) |
| 0x20A `IO_CHECK_ENQUEUE` | Return 0 (no pending) | — |
| 0x20D `XE_XBE_PATH_LENGTH` | Return `xbe_path.size()` | — |
| 0x20E `XE_XBE_PATH_ADDR` | — | Write XBE path bytes to guest VA in `val` |
| 0x20F `ACPI_TIME_LOW` | Read ACPI timer, return low 32 bits | — |
| 0x210 `ACPI_TIME_HIGH` | Return high 32 bits of cached ACPI time | — |

**Clock increment calculation** (from nxbx `kernel.cpp`):

```cpp
uint64_t curr_us = duration_cast<microseconds>(now - boot_time).count();
uint64_t elapsed_us = curr_us - last_clock_us;
last_clock_us = curr_us;
uint64_t elapsed_increment = elapsed_us * 10;  // 100ns units
lost_clock_increment += elapsed_increment;
uint64_t actual = (lost_clock_increment / 10000) * 10000;  // floor to 1ms
lost_clock_increment -= actual;
return actual;
```

**ACPI timer**: nboxkrnl reads a 64-bit ACPI timer via ports 0x20F/0x210.
This is separate from the ACPI PM Timer at I/O port 0x8008 (which is 24-bit
and already implemented).  The host port returns microseconds since boot
converted to ACPI timer ticks (3.579545 MHz):

```cpp
uint64_t acpi_time = (uint64_t)(elapsed_us * 3.579545);
```

**Registration** in `setup.hpp` (or a new `nboxkrnl_setup` function):

```cpp
for (uint16_t p = 0x200; p <= 0x210; p++)
    exec.register_io(p, nboxkrnl_host_read, nboxkrnl_host_write, &nboxkrnl_state);
```

### Test

Add to `test_runner.cpp` a `--nboxkrnl-ports-test` mode that:

1. Initialises the executor with Xbox hardware + nboxkrnl host ports
2. Executes a small handcrafted 32-bit code sequence:
   ```nasm
   ; Read MACHINE_TYPE
   mov dx, 0x201
   in  eax, dx          ; expect EAX = 0 (Xbox)
   cmp eax, 0
   jne .fail

   ; Read BOOT_TIME_MS
   mov dx, 0x205
   in  eax, dx          ; expect EAX > 0

   ; Write debug string
   ; (set up a string "HELLO" at guest VA 0x80020000)
   mov dx, 0x200
   mov eax, 0x80020000
   out dx, eax

   ; Read XBE path length
   mov dx, 0x20D
   in  eax, dx          ; expect EAX = length of configured path

   hlt                   ; success
   .fail:
   mov eax, 0xDEAD
   hlt
   ```
3. Verify final EAX = 0 (no .fail taken) and halted=true

### Commit

```
nboxkrnl M2: host-kernel communication I/O ports

Add NboxkrnlHostState and register I/O port handlers for ports 0x200-0x210
implementing the nboxkrnl host protocol: debug string output, machine type
query, clock increment, boot time, ACPI timer, XBE path, and abort.
File I/O ports (0x206-0x208, 0x20A) are wired but stubbed pending M3.

Includes test verifying MACHINE_TYPE, BOOT_TIME_MS, DBG_STR, and
XBE_PATH_LENGTH via hand-assembled guest code.
```

---

## Milestone 3 — Host-Side Asynchronous File I/O System

**Goal:** Implement the file I/O packet processing that nboxkrnl expects:
submit, queue, process (on a worker thread), query completion, and transfer
data.  This is the largest single milestone.

### Why

nboxkrnl's I/O manager (`IoCreateFile`, `NtReadFile`, `NtWriteFile`,
`NtClose`, etc.) translates kernel file operations into `IoRequest` packets
written to guest memory, then notifies the host via `OUT` to port 0x206.
The host reads the packet, performs the actual file operation on the host
filesystem, writes the result into an `IoInfoBlock` in guest memory, and
marks it ready.  The guest polls via port 0x208.

This is exactly what nxbx's `io.cpp` does.  We port the same logic, adapted
to Xermu's `read_guest_block` / `write_guest_block` API.

### Data Structures

From nboxkrnl `kernel.hpp`, the packet layouts (all packed, 32-bit):

```cpp
// Type layout:  IoRequestType[31:28] | DevType[27:23] | IoFlags[22:3] | Disposition[2:0]

struct IoRequestHeader {        // 8 bytes
    uint32_t id;                // unique request ID
    uint32_t type;              // packed type/dev/flags/disposition
};

struct IoRequestOc {            // 36 bytes (after header)
    int64_t  initial_size;
    uint32_t size;              // path string length
    uint32_t handle;
    uint32_t path;              // guest VA of path string
    uint32_t attributes;
    uint32_t timestamp;
    uint32_t desired_access;
    uint32_t create_options;
};

struct IoRequestRw {            // 20 bytes (after header)
    int64_t  offset;
    uint32_t size;              // bytes to transfer
    uint32_t address;           // guest VA of data buffer
    uint32_t handle;
    uint32_t timestamp;
};

struct IoRequestXx {            // 4 bytes (after header)
    uint32_t handle;
};

struct IoInfoBlock {            // 16 bytes
    uint32_t id;
    int32_t  status;            // NTSTATUS
    uint32_t info;              // IoInfo enum
    uint32_t ready;             // 0=pending, 1=done
};

struct IoInfoBlockOc {          // 36 bytes
    IoInfoBlock header;
    uint32_t file_size;
    union {
        struct { uint32_t free_clusters, creation_time, last_access_time, last_write_time; } fatx;
        int64_t xdvdfs_timestamp;
    };
};
```

The `sizeof(IoRequestHeader) + sizeof(IoRequestOc)` = 8 + 36 = 44 bytes total
(the `packed_request_t` is always 44 bytes, with the union).

### Architecture

```
Guest (nboxkrnl)                          Host (Xermu)
─────────────────                         ────────────────
IoCreateFile()                            
  ├─ builds IoRequest packet              
  ├─ writes to guest RAM                  
  └─ OUT 0x206, &packet        ───────►   nboxkrnl_host_write(0x206, va)
                                            ├─ read_guest_block(va, 44, &pkt)
                                            ├─ if open: read path string from guest
                                            ├─ create host request_t
                                            └─ enqueue to io_worker thread
                                                     │
                                          io_worker thread:
                                            ├─ dequeue request
                                            ├─ map Xbox path → host path
                                            ├─ perform fopen/fread/fwrite/fclose
                                            ├─ build IoInfoBlock result
                                            └─ store in completed_map
                                                     │
KeQuerySystemTime() ...                              │
SubmitIoRequestToHost() polls:                       │
  OUT 0x208, &info_block        ───────►   nboxkrnl_host_write(0x208, va)
                                            ├─ read id from guest info_block
                                            ├─ lookup in completed_map
                                            ├─ if read: write_guest_block(data)
                                            ├─ write_guest_block(va, info_block)
                                            │  (sets ready=1)
                                            └─ remove from completed_map
Guest sees ready=1, continues.
```

### Implementation

**New file:** `src/xbox/nboxkrnl_io.hpp`

Contains:

1. **Packet structure definitions** — matching nboxkrnl's packed layouts
2. **Path translation** — maps `\Device\CdRom0\...` → host DVD directory,
   `\Device\Harddisk0\Partition1\...` → host C: partition directory, etc.
3. **Worker thread** — `std::jthread` with `std::deque` work queue,
   `std::mutex`, `std::atomic_flag` for signalling
4. **Request processing** — open (with all 6 dispositions), close, read, write,
   remove
5. **Completed request map** — `std::unordered_map<uint32_t, request_t>`
   keyed by request ID
6. **submit_io_packet(exec, guest_va)** — called from port 0x206 write handler
7. **flush_pending_packets()** — called from port 0x207 write handler
8. **query_io_packet(exec, guest_va)** — called from port 0x208 write handler

**Path mapping:**

nboxkrnl sends paths like `\Device\CdRom0\default.xbe` or
`\Device\Harddisk0\Partition2\TDATA\...`.  The host maps these:

| Xbox device path | Host directory |
|-----------------|----------------|
| `\Device\CdRom0\...` | XBE directory (or mounted XISO) |
| `\Device\Harddisk0\Partition0\...` | `data/hdd/Partition0/` (config area) |
| `\Device\Harddisk0\Partition1\...` | `data/hdd/Partition1/` (game saves) |
| `\Device\Harddisk0\Partition2\...` | `data/hdd/Partition2/` (C: drive, dashboard) |
| `\Device\Harddisk0\Partition3\...` | `data/hdd/Partition3/` (cache X:) |
| `\Device\Harddisk0\Partition4\...` | `data/hdd/Partition4/` (cache Y:) |
| `\Device\Harddisk0\Partition5\...` | `data/hdd/Partition5/` (cache Z:) |

**Simplification vs nxbx:**  nxbx uses a full FATX filesystem driver with
metadata.bin files to track cluster allocation and directory entries.  For the
initial integration, we use **pass-through host filesystem I/O**: Xbox file
paths map directly to host directories, and `fopen`/`fread`/`fwrite` operate
on real host files.  Timestamps are fabricated.  Free cluster count returns a
large fixed value.  This avoids the FATX complexity while still allowing
nboxkrnl to open, read, write, and close files.

Later, the full FATX driver from nxbx can be ported if games need it.

### Test

Add `test_nboxkrnl_io.cpp`:

1. Set up executor with nboxkrnl host ports + I/O system
2. Create a test file in `data/hdd/Partition2/test.txt` with known content
3. Manually construct an `IoRequest` open packet in guest RAM for
   `\Device\Harddisk0\Partition2\test.txt` with disposition FILE_OPEN
4. Write the packet address to port 0x206
5. Poll port 0x208 until info_block.ready == 1
6. Verify status == STATUS_SUCCESS and file_size matches
7. Construct a read packet, submit, poll, verify data matches

This test exercises the full submit → process → query pipeline without
running nboxkrnl.

### Commit

```
nboxkrnl M3: host-side asynchronous file I/O system

Implement the file I/O packet protocol that nboxkrnl uses to communicate
with the host: submit (port 0x206), retry (0x207), query (0x208), and
check-enqueue (0x20A).  Uses a worker thread with a deque-based queue.

Supports open (all 6 dispositions), close, read, write, and remove
operations.  Xbox device paths are mapped to host directories under
data/hdd/.  Uses pass-through host filesystem I/O (no FATX metadata).

Includes test_nboxkrnl_io exercising the full submit/process/query
pipeline with a hand-constructed IoRequest.
```

---

## Milestone 4 — XBE Path + Disk Partition Setup

**Goal:** Set up the host-side directory structure that nboxkrnl expects, and
configure the XBE launch path that the kernel reads at boot.

### Why

After nboxkrnl initialises the memory manager, object manager, HAL, and I/O
subsystems, it calls `XeLoadXbe()` which reads the XBE path from host port
0x20D/0x20E.  The kernel then opens this path via the file I/O system to load
and launch the title.

The host must:
1. Create the `data/hdd/Partition0..5/` directories if they don't exist
2. Copy/symlink the XBE (and its directory) into the appropriate partition
   or DVD directory
3. Set `NboxkrnlHostState::xbe_path` to the correct `\Device\...` format

### Implementation

**New function:** `nboxkrnl_setup_paths(const char* xbe_or_xiso_path)`

```cpp
void nboxkrnl_setup_paths(NboxkrnlHostState& state, const char* input_path) {
    // Create HDD partition directories
    for (int i = 0; i < 6; i++) {
        std::string dir = "data/hdd/Partition" + std::to_string(i);
        create_directory_recursive(dir);
    }

    // Determine input type: XBE file or XISO image
    if (ends_with(input_path, ".xbe")) {
        // XBE mode: DVD directory = parent of XBE file
        state.dvd_dir = parent_directory(input_path);
        std::string xbe_name = filename(input_path);
        state.xbe_path = "\\Device\\CdRom0\\" + xbe_name;
    } else if (ends_with(input_path, ".iso") || ends_with(input_path, ".xiso")) {
        // XISO mode: mount the ISO image
        state.xiso_path = input_path;
        state.xbe_path = "\\Device\\CdRom0\\default.xbe";
    }

    // Copy dashboard files to Partition2 if booting dashboard
    // (The real Xbox has the dashboard on the C: partition)
    if (is_dashboard(input_path)) {
        copy_dashboard_to_partition2(input_path, "data/hdd/Partition2/");
    }
}
```

### Test

In `test_runner`, add `--nboxkrnl` mode detection that calls
`nboxkrnl_setup_paths()` and prints the configured XBE path and directory
structure to stderr for verification:

```
$ test_runner --nboxkrnl --xbox "data/xbox dash orig_5960/xboxdash.xbe"
[nboxkrnl] DVD dir: data/xbox dash orig_5960/
[nboxkrnl] XBE path: \Device\CdRom0\xboxdash.xbe
[nboxkrnl] HDD partitions: data/hdd/Partition{0..5}/
```

### Commit

```
nboxkrnl M4: XBE path and disk partition setup

Add nboxkrnl_setup_paths() to create the host-side HDD partition
directory structure and configure the XBE launch path in the format
nboxkrnl expects (\Device\CdRom0\<name>.xbe or similar).

Supports both XBE files (DVD directory = XBE parent) and XISO images.
Creates data/hdd/Partition{0..5}/ directories on first run.
```

---

## Milestone 5 — `--nboxkrnl` Boot Mode: PE Loader + Page Tables

**Goal:** Add a new boot mode that loads the nboxkrnl PE into guest RAM at
0x80010000, sets up initial page tables, configures CPU registers, and jumps
to `KernelEntry`.

### Why

This is the core integration step.  It replaces the HLE bootstrap
(`boot_hle()`) with a real kernel loaded from a PE file.

### Implementation

**New file:** `src/xbox/nboxkrnl_boot.hpp`

```cpp
namespace nboxkrnl {

struct BootConfig {
    const char* kernel_pe_path;   // Path to nboxkrnl.exe
    const char* input_path;       // XBE or XISO to launch
    const char* keys_path;        // Optional: path to keys.bin (32 bytes: 16B EEPROM + 16B cert)
};

// Load nboxkrnl PE, set up page tables, configure entry state.
// Returns the entry EIP (KernelEntry address).
uint32_t boot_nboxkrnl(Executor& exec, XboxHardware& hw,
                        NboxkrnlHostState& host, const BootConfig& cfg);

}
```

**PE loading** (following nxbx `cpu.cpp`):

1. Read the PE file from disk
2. Validate DOS + NT headers (`IMAGE_DOS_SIGNATURE`, `IMAGE_NT_SIGNATURE`,
   `Machine == IMAGE_FILE_MACHINE_I386`, `ImageBase == 0x80010000`)
3. Copy PE headers to guest PA 0x10000 (`0x80010000 - 0x80000000`)
4. Copy each section to `ram[0x10000 + section.VirtualAddress]`
5. Zero-fill any section padding (VirtualSize > SizeOfRawData)

**Page table setup** (following nxbx `cpu.cpp`):

Page directory at PA 0xF000 (1024 × 4-byte PDEs):

```
PDE[0x000..0x00F]: Identity-map PA 0–64 MB with 4 MB large pages
                   Flags: 0xE3 (large, dirty, accessed, r/w, present)
                   Each PDE.base increments by 0x400000

PDE[0x200..0x20F]: Map VA 0x80000000–0x83FFFFFF → PA 0–64 MB (contiguous)
                   Same flags, base starts at 0x80000000 | PA

PDE[0x300]:        Map VA 0xC0000000 → PA 0xF000 (page tables self-map)
                   Flags: 0xF063 (dirty, accessed, r/w, present)

All other PDEs:    0 (not present)
```

**CPU state:**

```
CR0 = 0x80000021    (PE=1, NE=1, PG=1)
CR3 = 0x0000F000    (page directory PA)
CR4 = 0x00000610    (PSE=1, OSFXSR=1, OSXMMEXCPT=1)

ESP = 0x80400000    (stack at top of 4 MB)
EBP = 0x80400000

Segment bases: CS/DS/SS/ES/GS = 0, FS = 0
Segment selectors: CS=0x08, DS=SS=ES=GS=0x10, FS=0x18 (will be reloaded by kernel)

EIP = ImageBase + AddressOfEntryPoint
```

**Keys on stack:**

nboxkrnl's `KernelEntry` expects 32 bytes of keys at `[ESP]`:
- Bytes 0–15: EEPROM key
- Bytes 16–31: Certificate key

If `keys_path` is provided, read 32 bytes from the file and write them
to guest VA 0x80400000 via `write_guest_block`.  Otherwise write zeros.

**Integration into test_runner:**

```
test_runner --nboxkrnl <nboxkrnl.exe> [--keys <keys.bin>] <game.xbe>
```

This calls `xbox_setup()` (all hardware), `nboxkrnl_setup_paths()`,
registers host I/O ports, then `boot_nboxkrnl()`, then `exec.run()`.

### Test

1. Build nboxkrnl from the fork (separate step, documented in README)
2. Run: `test_runner --nboxkrnl nboxkrnl.exe "data/xbox dash orig_5960/xboxdash.xbe"`
3. Expected output: kernel starts, prints debug strings via port 0x200,
   queries machine type, initialises memory manager, loads GDT/IDT/TSS,
   and begins subsystem initialisation
4. Even without the full file I/O pipeline being correct, the kernel should
   get through `MmInitSystem()`, `ObInitSystem()`, `HalInitSystem()` —
   these don't use file I/O

**Fallback test** (without nboxkrnl binary):

Add a small hand-assembled "mock kernel" that mimics nboxkrnl's entry
sequence: read MACHINE_TYPE, write a debug string, halt.  This tests the
PE loading and page table setup without requiring the real kernel.

### Commit

```
nboxkrnl M5: PE loader, page tables, and --nboxkrnl boot mode

Add boot_nboxkrnl() that loads a nboxkrnl PE binary into guest RAM at
0x80010000, sets up 32-bit non-PAE page tables (identity-map + contiguous
mirror + self-map), configures CPU state (CR0/CR3/CR4, segments, stack),
passes EEPROM/certificate keys on the stack, and jumps to KernelEntry.

New CLI: test_runner --nboxkrnl <kernel.exe> [--keys <keys.bin>] <game.xbe>
Includes a mock-kernel test that validates the page table setup and
host port communication without requiring the full nboxkrnl binary.
```

---

## Milestone 6 — EEPROM/Certificate Key Setup

**Goal:** Generate or load the 32 bytes of keys that nboxkrnl expects on the
stack at entry, and ensure the SMBus EEPROM device serves consistent data.

### Why

nboxkrnl reads the EEPROM key and certificate key from the stack, then uses
them to decrypt the EEPROM data read via SMBus (slave address 0xA8).  The
EEPROM contains settings like video mode, language, timezone, network config.
If the keys don't match, the kernel may crash or behave incorrectly.

Xermu already has an `eeprom` array in the SMBus handler.  This milestone
ensures the keys passed to nboxkrnl match the EEPROM data.

### Implementation

**Option A — Zero keys + unencrypted EEPROM:**

nboxkrnl checks if the EEPROM key is all-zeros, and if so, skips decryption
(or uses a known default).  This is the simplest path: pass 32 zero bytes,
and configure the EEPROM with unencrypted data containing sensible defaults
(NTSC video, English language, etc.).

**Option B — Load from keys.bin:**

Allow the user to provide a 32-byte `keys.bin` file (16B EEPROM key + 16B
certificate key) extracted from a real Xbox.  The EEPROM data in the SMBus
handler must then be encrypted with the matching key.

Start with Option A; support Option B via `--keys`.

### Test

Boot nboxkrnl with zero keys and verify `HalInitSystem()` completes
(evidenced by debug output showing PIC/PIT/SMBus initialisation).

### Commit

```
nboxkrnl M6: EEPROM and certificate key setup

Configure key pass-through for nboxkrnl boot: 32 bytes of EEPROM +
certificate keys are passed on the initial stack.  Default is zero keys
with unencrypted EEPROM defaults (NTSC, English).  Optional --keys flag
loads real keys from a 32-byte file.

Ensures SMBus EEPROM data is consistent with the key configuration.
```

---

## Milestone 7 — End-to-End Integration Test

**Goal:** Run the Xbox Dashboard through nboxkrnl and verify it reaches
further than the HLE path (which halts at `HalReturnToFirmware(Fatal)`).

### Why

This validates the complete integration: PE loading → kernel boot → subsystem
init → HAL (PIC/PIT/SMBus) → I/O (file system) → XBE loading → D3D init →
NV2A MMIO access.

### Test Procedure

```bash
# Build nboxkrnl (from the fork, see README)
cd ../nboxkrnl && cmake --build build --config Release

# Run dashboard through nboxkrnl
cd ../files
.\build\Release\test_runner.exe --nboxkrnl ..\nboxkrnl\build\bin\Release\nboxkrnl.exe \
    "data\xbox dash orig_5960\xboxdash.xbe" 2>&1 | tee nboxkrnl_dash.txt
```

**Success criteria:**
1. Kernel prints debug strings showing subsystem initialisation
2. File I/O requests appear in the log (opening XBE sections)
3. NV2A MMIO events occur (D3D touches GPU registers)
4. No `HalReturnToFirmware(Fatal)` — kernel doesn't abort
5. The run progresses further than the 293-call HLE trace

**Expected issues and mitigations:**

| Issue | Likelihood | Mitigation |
|-------|-----------|------------|
| Missing MSR (nboxkrnl reads unknown MSRs) | Medium | Add RDMSR handlers, return 0 |
| NV2A register values unexpected | High | Compare with xemu register traces |
| SMBus EEPROM data wrong | Medium | Match nxbx's default EEPROM layout |
| IDE disk reads needed by nboxkrnl | Low | Initial boot only needs DVD/file I/O |
| Page fault in kernel code | Low | Debug via translate_va diagnostics |

### Commit

```
nboxkrnl M7: end-to-end dashboard integration test

Add documentation for building the nboxkrnl fork and running the
dashboard through the nboxkrnl boot path.  Update PROGRESS.md with
integration status.
```

---

## Appendix A — nboxkrnl Fork Requirements

The nboxkrnl fork is at https://github.com/PatrickvL/nboxkrnl and needs
minimal changes for Xermu compatibility:

1. **Build configuration** — Ensure it builds with the same MSVC 2022 toolchain
2. **Export table** — Verify all kernel ordinal exports are present
3. **Possible patches:**
   - Disable `NboxkrnlVersion` check (or match the expected version string)
   - Adjust any hardcoded assumptions about the host emulator (unlikely needed)

The fork should be kept closely synced with upstream to benefit from ongoing
development by ergo720, RadWolfie, and num0005.

## Appendix B — License Considerations

- nboxkrnl: GPL-2.0
- Xermu: TBD (currently no license specified)
- nxbx: GPL-3.0 (we reference its architecture but don't copy code)

nboxkrnl runs as **guest code** inside the emulator, communicating only through
I/O ports.  The host-side I/O port handlers in Xermu are original implementations
following the protocol documented in `kernel.hpp`.  This separation may constitute
an "arm's-length" interface under GPL, similar to how emulators handle BIOS files.

**Recommendation:** If Xermu is to be non-GPL, treat the nboxkrnl binary the
same way emulators treat BIOS ROMs — users provide their own copy.  The fork
at https://github.com/PatrickvL/nboxkrnl remains GPL-2.0 in its own
repository.  The host-side protocol handlers are original code that can be
any license.

## Appendix C — File Structure After Integration

```
src/
    xbox/
        nboxkrnl_host.hpp       ← M2: I/O port handlers + state
        nboxkrnl_io.hpp         ← M3: file I/O packet processing
        nboxkrnl_boot.hpp       ← M5: PE loader + page tables
        setup.hpp               ← modified to register nboxkrnl ports
    cpu/
        executor.hpp            ← M1: read/write_guest_block declarations
        executor.cpp            ← M1: read/write_guest_block implementations
    tests/
        test_runner.cpp         ← M5: --nboxkrnl boot mode
        test_block_rw.cpp       ← M1: block R/W test
        test_nboxkrnl_io.cpp    ← M3: file I/O pipeline test
```

## Appendix D — Milestone Summary

| # | Milestone | New files | Modified files | Depends on |
|---|-----------|-----------|---------------|------------|
| M1 | Guest memory block R/W | test_block_rw.cpp | executor.hpp, executor.cpp, CMakeLists.txt | — |
| M2 | Host I/O port handlers | nboxkrnl_host.hpp | setup.hpp, test_runner.cpp | M1 |
| M3 | Async file I/O | nboxkrnl_io.hpp | nboxkrnl_host.hpp | M1, M2 |
| M4 | XBE path + partitions | (in nboxkrnl_host.hpp) | test_runner.cpp | M2 |
| M5 | PE loader + boot mode | nboxkrnl_boot.hpp | test_runner.cpp, CMakeLists.txt | M1–M4 |
| M6 | EEPROM/key setup | (in nboxkrnl_boot.hpp) | smbus/eeprom | M5 |
| M7 | E2E integration test | — | PROGRESS.md, README.md | M1–M6 |

All milestones are host-side work.  The nboxkrnl binary is only required
starting from M5's integration test (the mock-kernel fallback test works
without it).
