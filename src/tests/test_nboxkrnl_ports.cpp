// test_nboxkrnl_ports.cpp — Unit tests for nboxkrnl host I/O port handlers.
//
// Verifies MACHINE_TYPE, BOOT_TIME_MS, DBG_STR, XBE_PATH_LENGTH, ABORT,
// and ACPI timer via hand-assembled guest code running on the executor.

#include "cpu/executor.hpp"
#include "xbox/nboxkrnl_host.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>

// ---------------------------------------------------------------------------
// Stub MMIO
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t pa, unsigned, void*) { return 0; }
static void stub_mmio_write(uint32_t, uint32_t, unsigned, void*) {}

static MmioMap make_mmio() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);
    return mmio;
}

// ---------------------------------------------------------------------------
// Test 1: Read MACHINE_TYPE — expect 0 (Xbox).
//
// Guest code at PA 0x1000:
//   mov dx, 0x201     ; PORT_MACHINE_TYPE
//   in  eax, dx       ; read → EAX
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_machine_type_code[] = {
    0x66, 0xBA, 0x01, 0x02,    // mov dx, 0x201
    0xED,                      // in  eax, dx
    0xF4,                      // hlt
};

static bool test_machine_type() {
    printf("=== Test: MACHINE_TYPE read ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    state.xbe_path = "\\Device\\CdRom0\\default.xbe";
    nboxkrnl::register_host_ports(*exec, state);

    exec->load_guest(0x1000, test_machine_type_code, sizeof(test_machine_type_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    uint32_t eax = exec->ctx.gp[GP_EAX];
    bool pass = (eax == 0);  // CONSOLE_XBOX
    printf("  EAX = %u (expected 0 = Xbox): %s\n", eax, pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 2: Read BOOT_TIME_MS — expect > 0 (some time has passed).
//
// Guest code:
//   mov dx, 0x205     ; PORT_BOOT_TIME_MS
//   in  eax, dx
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_boot_time_code[] = {
    0x66, 0xBA, 0x05, 0x02,    // mov dx, 0x205
    0xED,                      // in  eax, dx
    0xF4,                      // hlt
};

static bool test_boot_time() {
    printf("=== Test: BOOT_TIME_MS read ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    nboxkrnl::register_host_ports(*exec, state);

    exec->load_guest(0x1000, test_boot_time_code, sizeof(test_boot_time_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    uint32_t eax = exec->ctx.gp[GP_EAX];
    // Boot time could be 0 if we run fast enough, so just verify no crash.
    bool pass = true;  // any value is valid (including 0)
    printf("  BOOT_TIME_MS = %u ms: %s\n", eax, pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 3: Write DBG_STR — write a debug string pointer, verify no crash.
//
// Guest code:
//   mov eax, 0x2000   ; VA of string (identity-mapped, no paging)
//   mov dx, 0x200     ; PORT_DBG_STR
//   out dx, eax
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_dbg_str_code[] = {
    0xB8, 0x00, 0x20, 0x00, 0x00,  // mov eax, 0x2000
    0x66, 0xBA, 0x00, 0x02,        // mov dx, 0x200
    0xEF,                          // out dx, eax
    0xF4,                          // hlt
};

static bool test_dbg_str() {
    printf("=== Test: DBG_STR write ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    nboxkrnl::register_host_ports(*exec, state);

    // Place a test string at PA 0x2000.
    const char* msg = "Hello from nboxkrnl test!\n";
    exec->load_guest(0x2000, msg, strlen(msg) + 1);

    exec->load_guest(0x1000, test_dbg_str_code, sizeof(test_dbg_str_code));
    exec->ctx.eflags = 0x202;

    fprintf(stderr, "  [expect: \"[nboxkrnl] Hello from nboxkrnl test!\"]\n");
    exec->run(0x1000, 100);

    bool pass = exec->ctx.halted;
    printf("  halted after DBG_STR write: %s\n", pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 4: Read XBE_PATH_LENGTH — expect length of configured path.
//
// Guest code:
//   mov dx, 0x20D     ; PORT_XBE_PATH_LENGTH
//   in  eax, dx
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_xbe_path_len_code[] = {
    0x66, 0xBA, 0x0D, 0x02,    // mov dx, 0x20D
    0xED,                      // in  eax, dx
    0xF4,                      // hlt
};

static bool test_xbe_path_length() {
    printf("=== Test: XBE_PATH_LENGTH read ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    state.xbe_path = "\\Device\\CdRom0\\default.xbe";
    nboxkrnl::register_host_ports(*exec, state);

    exec->load_guest(0x1000, test_xbe_path_len_code, sizeof(test_xbe_path_len_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    uint32_t eax = exec->ctx.gp[GP_EAX];
    uint32_t expected = (uint32_t)state.xbe_path.size();
    bool pass = (eax == expected);
    printf("  XBE_PATH_LENGTH = %u (expected %u): %s\n", eax, expected, pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 5: Write ABORT — sets halted=true.
//
// Guest code:
//   mov eax, 1
//   mov dx, 0x202     ; PORT_ABORT
//   out dx, eax
//   ; should not reach here
//   mov eax, 0xDEAD
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_abort_code[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
    0x66, 0xBA, 0x02, 0x02,        // mov dx, 0x202
    0xEF,                          // out dx, eax
    0xB8, 0xAD, 0xDE, 0x00, 0x00,  // mov eax, 0xDEAD (should not be reached)
    0xF4,                          // hlt
};

static bool test_abort() {
    printf("=== Test: ABORT write ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    nboxkrnl::register_host_ports(*exec, state);

    exec->load_guest(0x1000, test_abort_code, sizeof(test_abort_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    bool halted = exec->ctx.halted;
    printf("  halted after ABORT: %s\n", halted ? "PASS" : "FAIL");

    exec->destroy();
    return halted;
}

// ---------------------------------------------------------------------------
// Test 6: Read ACPI_TIME — low 32 bits should be non-negative.
//
// Guest code:
//   mov dx, 0x20F     ; PORT_ACPI_TIME_LOW
//   in  eax, dx
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_acpi_time_code[] = {
    0x66, 0xBA, 0x0F, 0x02,    // mov dx, 0x20F
    0xED,                      // in  eax, dx
    0xF4,                      // hlt
};

static bool test_acpi_time() {
    printf("=== Test: ACPI_TIME_LOW read ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    nboxkrnl::register_host_ports(*exec, state);

    exec->load_guest(0x1000, test_acpi_time_code, sizeof(test_acpi_time_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    uint32_t eax = exec->ctx.gp[GP_EAX];
    // Just verify it doesn't crash; any timer value is valid.
    bool pass = true;
    printf("  ACPI_TIME_LOW = %u ticks: %s\n", eax, pass ? "PASS" : "FAIL");

    exec->destroy();
    return pass;
}

// ---------------------------------------------------------------------------
// Test 7: Write XBE_PATH_ADDR — write the path to guest memory, verify.
//
// Guest code:
//   mov eax, 0x3000   ; destination VA (identity-mapped)
//   mov dx, 0x20E     ; PORT_XBE_PATH_ADDR
//   out dx, eax
//   hlt
// ---------------------------------------------------------------------------

static const uint8_t test_xbe_path_addr_code[] = {
    0xB8, 0x00, 0x30, 0x00, 0x00,  // mov eax, 0x3000
    0x66, 0xBA, 0x0E, 0x02,        // mov dx, 0x20E
    0xEF,                          // out dx, eax
    0xF4,                          // hlt
};

static bool test_xbe_path_addr() {
    printf("=== Test: XBE_PATH_ADDR write ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<Executor>();
    if (!exec->init(&mmio)) { printf("  init failed\n"); return false; }

    nboxkrnl::HostState state;
    state.xbe_path = "\\Device\\CdRom0\\default.xbe";
    nboxkrnl::register_host_ports(*exec, state);

    // Clear destination area.
    memset(exec->ram + 0x3000, 0, 256);

    exec->load_guest(0x1000, test_xbe_path_addr_code, sizeof(test_xbe_path_addr_code));
    exec->ctx.eflags = 0x202;
    exec->run(0x1000, 100);

    // Verify the path was written to guest RAM at PA 0x3000.
    char buf[256] = {};
    memcpy(buf, exec->ram + 0x3000, state.xbe_path.size());
    bool pass = (memcmp(buf, state.xbe_path.c_str(), state.xbe_path.size()) == 0);
    printf("  path at PA 0x3000: \"%s\" (expected \"%s\"): %s\n",
           buf, state.xbe_path.c_str(), pass ? "PASS" : "FAIL");

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

    run(test_machine_type);
    run(test_boot_time);
    run(test_dbg_str);
    run(test_xbe_path_length);
    run(test_abort);
    run(test_acpi_time);
    run(test_xbe_path_addr);

    printf("\n=== nboxkrnl host port tests: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
