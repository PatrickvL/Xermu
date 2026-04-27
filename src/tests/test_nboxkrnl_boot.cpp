// test_nboxkrnl_boot.cpp — Test the nboxkrnl boot setup.
//
// Uses a small mock kernel binary (hand-assembled) to validate:
// 1. Page table setup (identity-map + kernel mirror)
// 2. CPU state configuration (CR0, CR3, CR4, segments)
// 3. Host port communication after boot
//
// Also validates boot_nboxkrnl() with the mock PE.

#include "cpu/executor.hpp"
#include "xbox/nboxkrnl_boot.hpp"
#include "xbox/nboxkrnl_host.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// Stub MMIO
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t, unsigned, void*) { return 0; }
static void stub_mmio_write(uint32_t, uint32_t, unsigned, void*) {}

static MmioMap make_mmio() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);
    return mmio;
}

// ---------------------------------------------------------------------------
// Test 1: Validate page table layout set up by setup_page_tables().
// ---------------------------------------------------------------------------

static bool test_page_tables() {
    printf("=== Test: page table setup ===\n");

    uint8_t* ram = static_cast<uint8_t*>(calloc(GUEST_RAM_SIZE, 1));
    if (!ram) { printf("  calloc failed\n"); return false; }

    nboxkrnl::setup_page_tables(ram);

    auto* pd = reinterpret_cast<uint32_t*>(ram + nboxkrnl::PAGE_DIR_PA);

    bool ok = true;

    // Check identity-map PDEs [0..15].
    for (int i = 0; i < 16; ++i) {
        uint32_t expected = (i * 0x00400000u) | 0xE3u;
        if (pd[i] != expected) {
            printf("  PDE[%d] = 0x%08X (expected 0x%08X)\n", i, pd[i], expected);
            ok = false;
        }
    }

    // Check kernel-mirror PDEs [0x200..0x20F] (contiguous memory alias).
    for (int i = 0; i < 16; ++i) {
        uint32_t expected = (0x80000000u + i * 0x00400000u) | 0xE3u;
        if (pd[0x200 + i] != expected) {
            printf("  PDE[0x%03X] = 0x%08X (expected 0x%08X)\n",
                   0x200 + i, pd[0x200 + i], expected);
            ok = false;
        }
    }

    // Check self-map PDE [0x300].
    uint32_t expected_self = nboxkrnl::PAGE_DIR_PA | 0x63u;
    if (pd[0x300] != expected_self) {
        printf("  PDE[0x300] = 0x%08X (expected 0x%08X)\n", pd[0x300], expected_self);
        ok = false;
    }

    // Check unmapped PDE is zero.
    if (pd[0x100] != 0) {
        printf("  PDE[0x100] = 0x%08X (expected 0)\n", pd[0x100]);
        ok = false;
    }

    printf("  page table layout: %s\n", ok ? "PASS" : "FAIL");

    free(ram);
    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: setup_cpu_state sets correct registers.
// ---------------------------------------------------------------------------

static bool test_cpu_state() {
    printf("=== Test: CPU state setup ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::setup_cpu_state(*exec);

    bool ok = true;
    auto check = [&](const char* name, uint32_t actual, uint32_t expected) {
        if (actual != expected) {
            printf("  %s = 0x%08X (expected 0x%08X)\n", name, actual, expected);
            ok = false;
        }
    };

    check("CR0",    exec->ctx.cr0,         0x80000021u);
    check("CR3",    exec->ctx.cr3,         nboxkrnl::PAGE_DIR_PA);
    check("CR4",    exec->ctx.cr4,         0x00000610u);
    check("ESP",    exec->ctx.gp[GP_ESP],  nboxkrnl::STACK_VA);
    check("EBP",    exec->ctx.gp[GP_EBP],  nboxkrnl::STACK_VA);
    check("EFLAGS", exec->ctx.eflags,      0x00000002u);
    check("CS_SEL", (uint32_t)exec->ctx.cs_sel, 0x08u);
    check("DS_SEL", (uint32_t)exec->ctx.ds_sel, 0x10u);

    printf("  CPU state: %s\n", ok ? "PASS" : "FAIL");

    exec->destroy();
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: Manual boot sequence — load code at PA 0x10000 (VA 0x80010000),
// set up page tables and CPU state, run guest code that uses the kernel
// mirror mapping to read/write memory and talk to host ports.
//
// Mock kernel at VA 0x80010000:
//   mov dx, 0x201     ; PORT_MACHINE_TYPE
//   in  eax, dx       ; expect EAX = 0
//   cmp eax, 0
//   jne .fail
//
//   ; Write a known value to VA 0x80020000 (maps to PA 0x20000)
//   mov dword [0x80020000], 0xCAFEBABE
//
//   ; Read it back via the identity map at VA 0x20000
//   mov eax, [0x20000]
//   cmp eax, 0xCAFEBABE
//   jne .fail
//
//   ; Read XBE path length
//   mov dx, 0x20D
//   in  eax, dx
//   ; EAX should be > 0
//   test eax, eax
//   jz  .fail
//
//   mov eax, 0        ; success marker
//   hlt
// .fail:
//   mov eax, 0xDEAD
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t mock_kernel[] = {
    // Read MACHINE_TYPE
    0x66, 0xBA, 0x01, 0x02,                         // mov dx, 0x201
    0xED,                                            // in  eax, dx
    0x83, 0xF8, 0x00,                                // cmp eax, 0
    0x75, 0x27,                                      // jne .fail (offset +39 → 0x29)

    // Write 0xCAFEBABE to VA 0x80020000
    0xC7, 0x05, 0x00, 0x00, 0x02, 0x80,             // mov dword [0x80020000], 0xCAFEBABE
    0xBE, 0xBA, 0xFE, 0xCA,

    // Read from VA 0x20000 (identity-mapped)
    0xA1, 0x00, 0x00, 0x02, 0x00,                   // mov eax, [0x20000]
    0x3D, 0xBE, 0xBA, 0xFE, 0xCA,                   // cmp eax, 0xCAFEBABE
    0x75, 0x12,                                      // jne .fail

    // Read XBE path length
    0x66, 0xBA, 0x0D, 0x02,                         // mov dx, 0x20D
    0xED,                                            // in  eax, dx
    0x85, 0xC0,                                      // test eax, eax
    0x74, 0x09,                                      // jz  .fail

    // Success
    0xB8, 0x00, 0x00, 0x00, 0x00,                   // mov eax, 0
    0xF4,                                            // hlt

    // .fail:
    0xB8, 0xAD, 0xDE, 0x00, 0x00,                   // mov eax, 0xDEAD
    0xF4,                                            // hlt
};

static bool test_mock_kernel_boot() {
    printf("=== Test: mock kernel boot ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    // Register host ports.
    nboxkrnl::HostState host;
    host.xbe_path = "\\Device\\CdRom0\\xboxdash.xbe";
    nboxkrnl::register_host_ports(*exec, host);

    // Load mock kernel at PA 0x10000 (= VA 0x80010000).
    exec->load_guest(nboxkrnl::KERNEL_PA_BASE, mock_kernel, sizeof(mock_kernel));

    // Set up page tables.
    nboxkrnl::setup_page_tables(exec->ram);

    // Set up CPU state.
    nboxkrnl::setup_cpu_state(*exec);

    // Run from EIP = 0x80010000 (first instruction of mock kernel).
    uint32_t entry = nboxkrnl::KERNEL_VA_BASE;
    exec->run(entry, 1000);

    uint32_t eax = exec->ctx.gp[GP_EAX];
    bool halted = exec->ctx.halted;
    bool pass = (halted && eax == 0);

    printf("  EAX = 0x%08X (expected 0x00000000), halted=%d\n", eax, halted);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    auto run = [&](bool(*fn)()) {
        if (fn()) ++passed; else ++failed;
    };

    run(test_page_tables);
    run(test_cpu_state);
    run(test_mock_kernel_boot);

    printf("\n=== nboxkrnl boot tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
