// ---------------------------------------------------------------------------
// test_runner — loads flat 32-bit binaries and executes them through the JIT.
//
// Usage:  test_runner [--xbox] <test.bin> [load_pa_hex]
//         test_runner --bios <bios.bin> [--mcpx <mcpx.bin>]
//
// Convention:
//   Code is loaded at the specified PA (default 0x1000).
//   Stack is set up at 0x80000 with a sentinel return address.
//   I/O port 0xE9: debug console output  (OUT DX, AL  where DX=0xE9)
//   HLT stops execution.  EAX = 0 means PASS, non-zero = fail code.
//
//   --xbox: Use the Xbox physical address map with real device stubs
//           instead of the default catch-all stub MMIO.
//   --bios: LLE boot mode — load a BIOS image into flash, initialise Xbox
//           hardware, and start execution at the x86 reset vector (0xFFFFFFF0).
//   --mcpx: (optional with --bios) Load a 512-byte MCPX ROM dump.
// ---------------------------------------------------------------------------

#include "executor.hpp"
#include "xbox.hpp"
#include "xbe_loader.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// Stub MMIO
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t pa, unsigned, void*) {
    fprintf(stderr, "[mmio] unexpected read  PA=%08X\n", pa);
    return 0xDEAD'BEEFu;
}
static void stub_mmio_write(uint32_t pa, uint32_t v, unsigned, void*) {
    fprintf(stderr, "[mmio] unexpected write PA=%08X val=%08X\n", pa, v);
}

// ---------------------------------------------------------------------------
// Test MMIO device: 4 KB register file at GUEST_RAM_SIZE (0x08000000).
// Simple read/write backing store for non-patchable MMIO tests.
// ---------------------------------------------------------------------------
static constexpr uint32_t TEST_MMIO_BASE = GUEST_RAM_SIZE;
static constexpr uint32_t TEST_MMIO_SIZE = 4096;
static uint8_t test_mmio_regs[TEST_MMIO_SIZE];

static uint32_t test_mmio_read(uint32_t pa, unsigned sz, void*) {
    uint32_t off = pa - TEST_MMIO_BASE;
    if (off + sz > TEST_MMIO_SIZE) return 0xFFFFFFFFu;
    uint32_t v = 0;
    memcpy(&v, &test_mmio_regs[off], sz);
    return v;
}
static void test_mmio_write(uint32_t pa, uint32_t v, unsigned sz, void*) {
    uint32_t off = pa - TEST_MMIO_BASE;
    if (off + sz > TEST_MMIO_SIZE) return;
    memcpy(&test_mmio_regs[off], &v, sz);
}

// ---------------------------------------------------------------------------
// I/O port handlers
// ---------------------------------------------------------------------------

// Port 0xE9: bochs-style debug console — prints the byte to stdout.
static void io_debug_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

// Port 0xEB: test IRQ trigger — writing N raises IRQ N on the PIC.
static void io_irq_trigger_write(uint16_t, uint32_t val, unsigned, void* user) {
    auto* pic = static_cast<xbox::PicPair*>(user);
    if (val < 16) pic->raise_irq((int)val);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_runner [--xbox] <test.bin> [load_pa_hex]\n"
                        "       test_runner --bios <bios.bin> [--mcpx <mcpx.bin>]\n");
        return 2;
    }

    // Parse flags.
    bool xbox_mode = false;
    bool bios_mode = false;
    const char* bios_path = nullptr;
    const char* mcpx_path = nullptr;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--xbox") == 0) {
            xbox_mode = true;
            argi++;
        } else if (strcmp(argv[argi], "--bios") == 0) {
            bios_mode = true;
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "--bios requires a BIOS image path\n");
                return 2;
            }
            bios_path = argv[argi++];
        } else if (strcmp(argv[argi], "--mcpx") == 0) {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "--mcpx requires an MCPX ROM path\n");
                return 2;
            }
            mcpx_path = argv[argi++];
        } else {
            break;
        }
    }

    if (bios_mode) {
        // ---- LLE boot mode ----
        printf("[test_runner] LLE boot: loading BIOS '%s'\n", bios_path);

        auto exec = std::make_unique<Executor>();
        auto* hw = xbox::xbox_setup(*exec);

        // Load BIOS into flash.
        if (!hw->flash.load_bios(bios_path)) {
            fprintf(stderr, "Failed to load BIOS image '%s'\n", bios_path);
            delete hw;
            return 2;
        }
        printf("[test_runner] BIOS loaded into flash\n");

        // Optionally load MCPX ROM.
        if (mcpx_path) {
            if (!hw->flash.load_mcpx(mcpx_path)) {
                fprintf(stderr, "Failed to load MCPX ROM '%s'\n", mcpx_path);
                delete hw;
                return 2;
            }
            printf("[test_runner] MCPX ROM loaded\n");
        }

        // Boot: interpret the 16-bit real-mode prologue (LGDT/LIDT/PE/far JMP)
        // to reach the 32-bit protected mode entry point.
        if (!exec->interpret_real_mode_boot()) {
            fprintf(stderr, "Real-mode boot stub interpretation failed\n");
            exec->destroy();
            delete hw;
            return 2;
        }

        printf("[test_runner] Entering protected mode at EIP=0x%08X\n",
               exec->ctx.eip);
        printf("[test_runner] GDT: base=0x%08X limit=%u  IDT: base=0x%08X limit=%u\n",
               exec->ctx.gdtr_base, exec->ctx.gdtr_limit,
               exec->ctx.idtr_base, exec->ctx.idtr_limit);
        printf("[test_runner] CR0=0x%08X\n", exec->ctx.cr0);

        exec->run(exec->ctx.eip, /*max_steps=*/100'000'000);

        printf("[test_runner] Halted: EIP=0x%08X EAX=0x%08X\n",
               exec->ctx.eip, exec->ctx.gp[GP_EAX]);

        exec->destroy();
        delete hw;
        return 0;
    }

    // ---- Test binary mode ----
    if (argi >= argc) {
        fprintf(stderr, "Usage: test_runner [--xbox] <test.bin> [load_pa_hex]\n");
        return 2;
    }

    const char* bin_path = argv[argi];
    uint32_t load_pa = 0x1000;
    if (argi + 1 < argc) load_pa = (uint32_t)strtoul(argv[argi + 1], nullptr, 16);

    // Read binary file.
    FILE* f = fopen(bin_path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open '%s'\n", bin_path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0 || (uint32_t)file_size > GUEST_RAM_SIZE - load_pa) {
        fprintf(stderr, "Invalid file size (%ld) or won't fit at PA 0x%08X\n",
                file_size, load_pa);
        fclose(f);
        return 2;
    }
    auto buf = std::make_unique<uint8_t[]>(file_size);
    if (fread(buf.get(), 1, file_size, f) != (size_t)file_size) {
        fprintf(stderr, "Read error\n");
        fclose(f);
        return 2;
    }
    fclose(f);

    printf("[test_runner] Loading '%s' (%ld bytes) at PA 0x%08X\n",
           bin_path, file_size, load_pa);

    // Set up executor.
    auto exec = std::make_unique<Executor>();
    xbox::XboxHardware* hw = nullptr;
    MmioMap stub_mmio;

    if (xbox_mode) {
        hw = xbox::xbox_setup(*exec);
    } else {
        memset(test_mmio_regs, 0, sizeof(test_mmio_regs));
        stub_mmio.add(TEST_MMIO_BASE, TEST_MMIO_SIZE,
                      test_mmio_read, test_mmio_write);
        stub_mmio.add(TEST_MMIO_BASE + TEST_MMIO_SIZE,
                      ~0u - (TEST_MMIO_BASE + TEST_MMIO_SIZE),
                      stub_mmio_read, stub_mmio_write);
        if (!exec->init(&stub_mmio)) {
            fprintf(stderr, "Executor init failed\n");
            return 2;
        }
    }

    exec->load_guest(load_pa, buf.get(), file_size);

    // Stack with sentinel return address.
    // In xbox mode, 0x80000 is used for HLE stubs, so stack goes higher.
    uint32_t stack_top = xbox_mode ? 0x000A'0000u : 0x0008'0000u;
    static constexpr uint32_t SENTINEL_EIP = 0xFFFF'FFFFu;
    exec->ctx.gp[GP_ESP] = stack_top;
    memcpy(exec->ram + stack_top, &SENTINEL_EIP, 4);
    exec->ctx.eflags = 0x0000'0202;

    // Segment bases: FS points to a scratch area for tests.
    // The Xbox kernel uses FS for KPCR; tests can store/load via FS:[offset].
    static constexpr uint32_t FS_BASE = 0x0007'0000;
    exec->ctx.fs_base = FS_BASE;
    exec->ctx.gs_base = 0;

    // Register I/O ports (in non-xbox mode; xbox_setup already registers them).
    if (!xbox_mode)
        exec->register_io(0xE9, nullptr, io_debug_write);

    // In xbox mode, install HLE kernel stubs and handler.
    static xbe::XbeHeap hle_heap;
    if (xbox_mode) {
        xbe::write_hle_stubs(exec->ram);
        hle_heap = xbe::XbeHeap();  // reset allocator
        exec->hle_handler = xbe::default_hle_handler;
        exec->hle_user    = &hle_heap;

        // Test-only IRQ trigger port: write N to 0xEB → raise IRQ N on PIC.
        exec->register_io(0xEB, nullptr, io_irq_trigger_write, &hw->pic);
    }

    // Run.
    exec->run(load_pa, /*max_steps=*/10'000'000);

    // Result: EAX = 0 is PASS, non-zero is fail code.
    uint32_t result = exec->ctx.gp[GP_EAX];

    printf("[test_runner] %s (EAX=%u, EIP=0x%08X)\n",
           result == 0 ? "PASS" : "FAIL", result, exec->ctx.eip);

    exec->destroy();
    delete hw;
    return result == 0 ? 0 : 1;
}
