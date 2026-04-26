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

#include "xbox/hle/bootstrap.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

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

static void io_debug_write(uint16_t, uint32_t val, unsigned, void*) {
    putchar(val & 0xFF);
    fflush(stdout);
}

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
                        "       test_runner --bios <bios.bin> [--mcpx <mcpx.bin>] [--rc4-key <key.bin>] [--dump-kernel <out.exe>]\n"
                        "       test_runner --xbox --kernel <xboxkrnl.exe> <game.xbe>\n");
        return 2;
    }

    // Parse flags.
    bool xbox_mode = false;
    bool bios_mode = false;
    const char* bios_path = nullptr;
    const char* mcpx_path = nullptr;
    const char* kernel_path = nullptr;
    const char* rc4_key_path = nullptr;
    const char* dump_kernel_path = nullptr;
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
        } else if (strcmp(argv[argi], "--rc4-key") == 0) {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "--rc4-key requires a 16-byte key file path\n");
                return 2;
            }
            rc4_key_path = argv[argi++];
        } else if (strcmp(argv[argi], "--dump-kernel") == 0) {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "--dump-kernel requires an output file path\n");
                return 2;
            }
            dump_kernel_path = argv[argi++];
        } else if (strcmp(argv[argi], "--kernel") == 0) {
            argi++;
            if (argi >= argc) {
                fprintf(stderr, "--kernel requires an xboxkrnl.exe path\n");
                return 2;
            }
            kernel_path = argv[argi++];
        } else {
            break;
        }
    }

    // ---- LLE boot mode (uses shared bootstrap) ----
    if (bios_mode) {
        xbox::BootConfig cfg;
        cfg.bios_path = bios_path;
        if (mcpx_path) cfg.mcpx_path = mcpx_path;
        if (rc4_key_path) cfg.rc4_key_path = rc4_key_path;
        if (dump_kernel_path) cfg.dump_kernel_path = dump_kernel_path;

        xbox::XboxSystem sys;
        if (!xbox::boot_lle(sys, cfg)) return 2;

        // Run 2BL to completion — it processes the init table, sets up
        // the memory controller, decrypts/decompresses the kernel into RAM,
        // then jumps to KiSystemStartup.
        // Run in a loop with run_step() to handle halts and retries.
        for (int attempt = 0; attempt < 20 && sys.running; ++attempt) {
            sys.exec->ctx.halted = false;
            sys.exec->ctx.stop_reason = STOP_NONE;
            sys.exec->run(sys.entry_eip, 500'000'000);

            uint32_t sr = sys.exec->ctx.stop_reason;
            uint32_t eip = sys.exec->ctx.eip;
            fprintf(stderr, "[bios] attempt %d: EIP=0x%08X stop=%u halted=%d\n",
                    attempt, eip, sr, sys.exec->ctx.halted);

            // Dump bytes at current EIP for diagnostics
            if (eip < GUEST_RAM_SIZE - 16) {
                fprintf(stderr, "  bytes @%08X:", eip);
                for (int i = 0; i < 16; ++i)
                    fprintf(stderr, " %02X", sys.exec->ram[eip + i]);
                fprintf(stderr, "\n");
            }
            // On first attempt, also dump the 2BL header at 0x400000
            if (attempt == 0 && 0x400040 < GUEST_RAM_SIZE) {
                fprintf(stderr, "  2BL @400000:");
                for (int i = 0; i < 64; ++i) {
                    if (i % 16 == 0 && i > 0) fprintf(stderr, "\n              ");
                    fprintf(stderr, " %02X", sys.exec->ram[0x400000 + i]);
                }
                fprintf(stderr, "\n");
            }

            if (sr == STOP_INVALID_OPCODE || sr == STOP_DIVIDE_ERROR) {
                // Hit an unhandled instruction — continue from next EIP
                sys.entry_eip = sys.exec->ctx.next_eip;
                continue;
            }
            if (sys.exec->ctx.halted) {
                // HLT — kernel might be waiting for interrupts
                sys.entry_eip = eip + 1; // skip HLT
                continue;
            }
            // Max steps exhausted — continue from current EIP
            sys.entry_eip = eip;
        }

        printf("[test_runner] Final: EIP=0x%08X EAX=0x%08X\n",
               sys.exec->ctx.eip, sys.exec->ctx.gp[GP_EAX]);

        // Scan RAM for a valid PE image (the decrypted/decompressed kernel).
        printf("[test_runner] Scanning RAM for PE images...\n");
        std::string dump_path = cfg.dump_kernel_path;
        if (dump_path.empty()) dump_path = "data/xboxkrnl_dumped.exe";
        uint32_t kernel_pa = xbox::scan_and_dump_kernel(
            sys.exec->ram, GUEST_RAM_SIZE, dump_path);
        return 0;
    }

    // ---- LLE-kernel mode (xboxkrnl.exe + XBE) ----
    if (kernel_path && argi < argc) {
        xbox::BootConfig cfg;
        cfg.kernel_path = kernel_path;
        cfg.xbe_path = argv[argi];

        xbox::XboxSystem sys;
        if (!xbox::boot_lle_kernel(sys, cfg)) return 2;

        // Register test-only IRQ trigger port
        sys.exec->register_io(0xEB, nullptr, io_irq_trigger_write, &sys.hw->pic);

        // Run to completion
        while (xbox::run_step(sys, 10'000'000)) {}

        uint32_t result = sys.exec->ctx.gp[GP_EAX];
        printf("[test_runner] %s (EAX=%u, EIP=0x%08X)\n",
               result == 0 ? "PASS" : "FAIL", result, sys.exec->ctx.eip);
        return result == 0 ? 0 : 1;
    }

    // ---- Test binary / XBE mode ----
    if (argi >= argc) {
        fprintf(stderr, "Usage: test_runner [--xbox] <test.bin> [load_pa_hex]\n");
        return 2;
    }

    const char* bin_path = argv[argi];
    uint32_t load_pa = 0x1000;
    if (argi + 1 < argc) load_pa = (uint32_t)strtoul(argv[argi + 1], nullptr, 16);

    // Read binary file.
    auto buf = xbox::read_file_to_vec(bin_path);
    if (buf.empty()) {
        fprintf(stderr, "Cannot open '%s'\n", bin_path);
        return 2;
    }
    long file_size = (long)buf.size();

    printf("[test_runner] Loading '%s' (%ld bytes) at PA 0x%08X\n",
           bin_path, file_size, load_pa);

    // XBE in xbox mode → use shared bootstrap
    bool is_xbe = (file_size >= 4 && memcmp(buf.data(), "XBEH", 4) == 0);
    if (is_xbe && xbox_mode) {
        xbox::BootConfig cfg;
        cfg.xbe_path = bin_path;

        xbox::XboxSystem sys;
        if (!xbox::boot_hle(sys, cfg)) return 2;

        // Test-only IRQ trigger port
        sys.exec->register_io(0xEB, nullptr, io_irq_trigger_write, &sys.hw->pic);

        // Run to completion (handles threads + DPCs via run_step)
        while (xbox::run_step(sys, 10'000'000)) {}

        uint32_t result = sys.exec->ctx.gp[GP_EAX];
        printf("[test_runner] %s (EAX=%u, EIP=0x%08X)\n",
               result == 0 ? "PASS" : "FAIL", result, sys.exec->ctx.eip);
        return result == 0 ? 0 : 1;
    }

    // ---- Flat binary test mode (non-XBE) ----
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

    exec->load_guest(load_pa, buf.data(), file_size);

    // Stack with sentinel return address.
    uint32_t stack_top = xbox_mode ? 0x000A'0000u : 0x0008'0000u;
    static constexpr uint32_t SENTINEL_EIP = 0xFFFF'FFFFu;
    exec->ctx.gp[GP_ESP] = stack_top;
    memcpy(exec->ram + stack_top, &SENTINEL_EIP, 4);
    exec->ctx.eflags = 0x0000'0202;

    // FS base for tests (non-XBE)
    exec->ctx.fs_base = 0x0007'0000u;
    exec->ctx.gs_base = 0;

    if (!xbox_mode)
        exec->register_io(0xE9, nullptr, io_debug_write);

    // In xbox mode (non-XBE), install HLE stubs for asm tests that need them.
    static xbe::XbeHeap hle_heap;
    if (xbox_mode) {
        xbe::write_hle_stubs(exec->ram);
        hle_heap.reset();
        exec->hle_handler = xbe::default_hle_handler;
        exec->hle_user    = &hle_heap;
        exec->register_io(0xEB, nullptr, io_irq_trigger_write, &hw->pic);
    }

    exec->run(load_pa, 10'000'000);

    uint32_t result = exec->ctx.gp[GP_EAX];
    printf("[test_runner] %s (EAX=%u, EIP=0x%08X)\n",
           result == 0 ? "PASS" : "FAIL", result, exec->ctx.eip);

    exec->destroy();
    delete hw;
    return result == 0 ? 0 : 1;
}
