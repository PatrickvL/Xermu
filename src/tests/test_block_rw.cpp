// test_block_rw.cpp — Unit tests for read_guest_block / write_guest_block.
//
// Tests page-table-aware block memory copy with identity-mapped pages,
// kernel-mirror (0x80000000) pages, page-boundary crossings, and 4 MB
// large pages.

#include "cpu/executor.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// Stub MMIO (should never be hit).
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t pa, unsigned, void*) {
    fprintf(stderr, "[mmio] unexpected read PA=%08X\n", pa);
    return 0xDEAD'BEEFu;
}
static void stub_mmio_write(uint32_t pa, uint32_t v, unsigned, void*) {
    fprintf(stderr, "[mmio] unexpected write PA=%08X val=%08X\n", pa, v);
}

static MmioMap make_mmio() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);
    return mmio;
}

// ---------------------------------------------------------------------------
// Set up nboxkrnl-style page tables in guest RAM.
//
// Page directory at PA 0xF000:
//   PDE[0x000-0x00F] = identity-map first 64 MB as 4 MB large pages (PS=1)
//   PDE[0x200-0x20F] = mirror at 0x80000000 (same 64 MB as large pages)
//   PDE[0x300]       = self-map (0xF063)
// ---------------------------------------------------------------------------

static void setup_nboxkrnl_page_tables(uint8_t* ram) {
    const uint32_t PD_PA = 0xF000;
    memset(ram + PD_PA, 0, 0x1000);

    auto* pd = reinterpret_cast<uint32_t*>(ram + PD_PA);

    // Identity-map: PDE[0..15] → PA 0, 4MB, 8MB, ..., 60MB  (4 MB large pages)
    for (int i = 0; i < 16; i++) {
        pd[i] = (i * 0x00400000u) | 0xE3u;  // Present | RW | Accessed | Dirty | PS
    }

    // Mirror at 0x80000000: PDE[0x200..0x20F] → same physical addresses
    for (int i = 0; i < 16; i++) {
        pd[0x200 + i] = (i * 0x00400000u) | 0xE3u;
    }

    // Self-map: PDE[0x300] → PD_PA with small-page flags
    pd[0x300] = PD_PA | 0x63u;  // Present | RW | Accessed | Dirty
}

// ---------------------------------------------------------------------------
// Test 1: write and read back via identity-mapped VA (no paging).
// ---------------------------------------------------------------------------

static bool test_no_paging() {
    printf("=== Test: block R/W without paging ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }

    // Paging off (CR0.PG = 0).
    exec->ctx.cr0 = 0;

    // Write a pattern to PA/VA 0x20000.
    uint8_t pattern[16];
    for (int i = 0; i < 16; i++) pattern[i] = (uint8_t)(0xA0 + i);

    bool ok = exec->write_guest_block(0x20000, 16, pattern);
    if (!ok) { printf("  write_guest_block failed\n"); exec->destroy(); return false; }

    // Read it back.
    uint8_t readback[16] = {};
    ok = exec->read_guest_block(0x20000, 16, readback);
    if (!ok) { printf("  read_guest_block failed\n"); exec->destroy(); return false; }

    ok = (memcmp(pattern, readback, 16) == 0);
    printf("  identity R/W: %s\n", ok ? "PASS" : "FAIL");

    exec->destroy();
    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: write/read via kernel-mirror VA (0x80020000) with paging.
// ---------------------------------------------------------------------------

static bool test_kernel_mirror() {
    printf("=== Test: block R/W with kernel-mirror paging ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }

    setup_nboxkrnl_page_tables(exec->ram);

    // Enable paging.
    exec->ctx.cr0 = 0x80000001u;  // PG + PE
    exec->ctx.cr3 = 0xF000u;
    exec->ctx.cr4 = 0x00000610u;  // PSE + PGE + OSFXSR

    // Write pattern via kernel-mirror VA.
    uint8_t pattern[32];
    for (int i = 0; i < 32; i++) pattern[i] = (uint8_t)(0x50 + i);

    bool ok = exec->write_guest_block(0x80020000u, 32, pattern);
    if (!ok) { printf("  write_guest_block failed\n"); exec->destroy(); return false; }

    // Read directly from physical RAM to verify.
    bool match = (memcmp(exec->ram + 0x20000, pattern, 32) == 0);
    printf("  write via 0x80020000 → PA 0x20000: %s\n", match ? "PASS" : "FAIL");
    if (!match) { exec->destroy(); return false; }

    // Read back via kernel-mirror VA.
    uint8_t readback[32] = {};
    ok = exec->read_guest_block(0x80020000u, 32, readback);
    if (!ok) { printf("  read_guest_block failed\n"); exec->destroy(); return false; }

    match = (memcmp(pattern, readback, 32) == 0);
    printf("  read  via 0x80020000: %s\n", match ? "PASS" : "FAIL");

    exec->destroy();
    return match;
}

// ---------------------------------------------------------------------------
// Test 3: page-boundary crossing.
// Write 8 bytes starting at VA 0x80020FFE — spans two 4 MB large pages
// (well, within one large page, but at 0x80020FFE..0x80021005 — crosses
// a 4 KB page boundary which is what read_guest_block iterates over).
// ---------------------------------------------------------------------------

static bool test_page_boundary() {
    printf("=== Test: block R/W page-boundary crossing ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }

    setup_nboxkrnl_page_tables(exec->ram);

    exec->ctx.cr0 = 0x80000001u;
    exec->ctx.cr3 = 0xF000u;
    exec->ctx.cr4 = 0x00000610u;

    // 8 bytes crossing a page boundary at 0x80020FFE.
    uint8_t pattern[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };

    bool ok = exec->write_guest_block(0x80020FFEu, 8, pattern);
    if (!ok) { printf("  write_guest_block failed\n"); exec->destroy(); return false; }

    // Verify in physical RAM: PA 0x20FFE..0x21005
    bool match = (memcmp(exec->ram + 0x20FFE, pattern, 8) == 0);
    printf("  cross-page write at PA 0x20FFE: %s\n", match ? "PASS" : "FAIL");
    if (!match) { exec->destroy(); return false; }

    // Read back via VA.
    uint8_t readback[8] = {};
    ok = exec->read_guest_block(0x80020FFEu, 8, readback);
    if (!ok) { printf("  read_guest_block failed\n"); exec->destroy(); return false; }

    match = (memcmp(pattern, readback, 8) == 0);
    printf("  cross-page read: %s\n", match ? "PASS" : "FAIL");

    exec->destroy();
    return match;
}

// ---------------------------------------------------------------------------
// Test 4: large block (multi-page).
// Write 8192 bytes (2 pages) starting at VA 0x80030000.
// ---------------------------------------------------------------------------

static bool test_large_block() {
    printf("=== Test: large multi-page block R/W ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }

    setup_nboxkrnl_page_tables(exec->ram);

    exec->ctx.cr0 = 0x80000001u;
    exec->ctx.cr3 = 0xF000u;
    exec->ctx.cr4 = 0x00000610u;

    constexpr uint32_t SIZE = 8192;
    uint8_t pattern[SIZE];
    for (uint32_t i = 0; i < SIZE; i++) pattern[i] = (uint8_t)(i & 0xFF);

    bool ok = exec->write_guest_block(0x80030000u, SIZE, pattern);
    if (!ok) { printf("  write_guest_block failed\n"); exec->destroy(); return false; }

    // Verify in physical RAM.
    bool match = (memcmp(exec->ram + 0x30000, pattern, SIZE) == 0);
    printf("  8 KB write: %s\n", match ? "PASS" : "FAIL");
    if (!match) { exec->destroy(); return false; }

    // Read back.
    uint8_t readback[SIZE] = {};
    ok = exec->read_guest_block(0x80030000u, SIZE, readback);
    if (!ok) { printf("  read_guest_block failed\n"); exec->destroy(); return false; }

    match = (memcmp(pattern, readback, SIZE) == 0);
    printf("  8 KB readback: %s\n", match ? "PASS" : "FAIL");

    exec->destroy();
    return match;
}

// ---------------------------------------------------------------------------
// Test 5: fault on unmapped VA.
// ---------------------------------------------------------------------------

static bool test_unmapped_fault() {
    printf("=== Test: block R/W returns false on unmapped VA ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }

    setup_nboxkrnl_page_tables(exec->ram);

    exec->ctx.cr0 = 0x80000001u;
    exec->ctx.cr3 = 0xF000u;
    exec->ctx.cr4 = 0x00000610u;

    // VA 0xA0000000 has no PDE mapping → should fault.
    uint8_t buf[4] = {};
    bool ok_read = exec->read_guest_block(0xA0000000u, 4, buf);
    bool ok_write = exec->write_guest_block(0xA0000000u, 4, buf);

    bool pass = !ok_read && !ok_write;
    printf("  unmapped read  returns false: %s\n", !ok_read  ? "PASS" : "FAIL");
    printf("  unmapped write returns false: %s\n", !ok_write ? "PASS" : "FAIL");

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

    run(test_no_paging);
    run(test_kernel_mirror);
    run(test_page_boundary);
    run(test_large_block);
    run(test_unmapped_fault);

    printf("\n=== block R/W tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
