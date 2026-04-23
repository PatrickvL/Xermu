#include "executor.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <memory>

// ---------------------------------------------------------------------------
// Stub MMIO — catches any access above guest RAM (should not be hit here).
// ---------------------------------------------------------------------------

static uint32_t stub_mmio_read(uint32_t pa, unsigned /*sz*/, void* /*u*/) {
    fprintf(stderr, "[mmio] unexpected read  PA=%08X\n", pa);
    return 0xDEAD'BEEFu;
}
static void stub_mmio_write(uint32_t pa, uint32_t v, unsigned /*sz*/, void* /*u*/) {
    fprintf(stderr, "[mmio] unexpected write PA=%08X val=%08X\n", pa, v);
}

// Shared executor setup / teardown helpers.
static MmioMap make_mmio() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);
    return mmio;
}

static constexpr uint32_t STACK_TOP    = 0x0008'0000;
static constexpr uint32_t SENTINEL_EIP = 0xFFFF'FFFFu;

static bool setup_exec(XboxExecutor& exec, MmioMap& mmio,
                        uint32_t load_pa, const uint8_t* code, size_t size) {
    if (!exec.init(&mmio)) { fprintf(stderr, "init failed\n"); return false; }
    exec.load_guest(load_pa, code, size);
    exec.ctx.gp[GP_ESP] = STACK_TOP;
    memcpy(exec.ram + STACK_TOP, &SENTINEL_EIP, 4);
    exec.ctx.eflags = 0x0000'0202;
    return true;
}

// ===========================================================================
// Test 1: Sum loop + fastmem round-trip
// ===========================================================================

// Guest program (32-bit protected mode, load PA = 0x1000):
//   Sum integers 1..10 into EAX, store to RAM, verify round-trip.
//   mov  eax, 0           ; sum = 0
//   mov  ecx, 10          ; i   = 10
// .loop:
//   add  eax, ecx         ; sum += i
//   dec  ecx              ; i--   (DEC ECX short-form 0x49 — re-encoded by JIT)
//   jnz  .loop            ; loop while i != 0
//   mov  [0x4000], eax    ; store sum (fastmem write)
//   mov  edx, [0x4000]    ; reload    (fastmem read — round-trip check)
//   mov  [0x4004], edx    ; store reload result
//   mov  ebx, 0xDEADBEEF
//   ret

static const uint8_t test1_code[] = {
    0xB8, 0x00, 0x00, 0x00, 0x00,       // mov eax, 0
    0xB9, 0x0A, 0x00, 0x00, 0x00,       // mov ecx, 10
    0x01, 0xC8,                          // add eax, ecx
    0x49,                                // dec ecx
    0x75, 0xFB,                          // jnz -5
    0x89, 0x05, 0x00, 0x40, 0x00, 0x00, // mov [0x4000], eax
    0x8B, 0x15, 0x00, 0x40, 0x00, 0x00, // mov edx, [0x4000]
    0x89, 0x15, 0x04, 0x40, 0x00, 0x00, // mov [0x4004], edx
    0xBB, 0xEF, 0xBE, 0xAD, 0xDE,       // mov ebx, 0xDEADBEEF
    0xC3,                                // ret
};
static_assert(sizeof(test1_code) == 39);

static bool test_sum_loop() {
    printf("=== Test 1: Sum loop + fastmem round-trip ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<XboxExecutor>();
    if (!setup_exec(*exec, mmio, 0x1000, test1_code, sizeof(test1_code)))
        return false;

    exec->run(0x1000, 100'000);

    uint32_t ram4000 = 0, ram4004 = 0;
    memcpy(&ram4000, exec->ram + 0x4000, 4);
    memcpy(&ram4004, exec->ram + 0x4004, 4);

    printf("  EAX  = %u  \t(expected 55)\n",           exec->ctx.gp[GP_EAX]);
    printf("  EBX  = 0x%08X\t(expected 0xDEADBEEF)\n", exec->ctx.gp[GP_EBX]);
    printf("  ECX  = %u  \t(expected 0)\n",            exec->ctx.gp[GP_ECX]);
    printf("  [0x4000] = %u  \t(expected 55)\n",       ram4000);
    printf("  [0x4004] = %u  \t(expected 55)\n",       ram4004);

    bool ok = exec->ctx.gp[GP_EAX] == 55u
           && exec->ctx.gp[GP_EBX] == 0xDEADBEEFu
           && exec->ctx.gp[GP_ECX] == 0u
           && ram4000              == 55u
           && ram4004              == 55u;

    printf("  %s\n", ok ? "PASS" : "FAIL");
    exec->destroy();
    return ok;
}

// ===========================================================================
// Test 2: EFLAGS preservation across memory dispatch
// ===========================================================================

// Guest program (load PA = 0x2000):
//   Verify that EFLAGS set by CMP survive across a memory load whose inline
//   fastmem dispatch (CMP R14, R15) would clobber ZF without the fix.
//
//   mov  eax, 42          ; value A
//   mov  ecx, 42          ; value B (equal to A)
//   mov  [0x4000], eax    ; store A to RAM
//   cmp  eax, ecx         ; sets ZF=1 (A == B)
//   mov  edx, [0x4000]    ; memory load — must NOT clobber ZF
//   je   .equal           ; should be taken (ZF=1)
//   mov  ebx, 0           ; not-taken path
//   ret
// .equal:
//   mov  ebx, 1           ; taken path
//   ret
//
// Expected: EBX = 1 (JE taken because ZF was preserved)
// Without fix: EBX = 0 (CMP R14, R15 clears ZF → JE not taken)

static const uint8_t test2_code[] = {
    0xB8, 0x2A, 0x00, 0x00, 0x00,       //  0: mov eax, 42
    0xB9, 0x2A, 0x00, 0x00, 0x00,       //  5: mov ecx, 42
    0x89, 0x05, 0x00, 0x40, 0x00, 0x00, // 10: mov [0x4000], eax
    0x39, 0xC8,                          // 16: cmp eax, ecx     → ZF=1
    0x8B, 0x15, 0x00, 0x40, 0x00, 0x00, // 18: mov edx, [0x4000]
    0x74, 0x06,                          // 24: je +6 → offset 32
    0xBB, 0x00, 0x00, 0x00, 0x00,       // 26: mov ebx, 0  (not taken)
    0xC3,                                // 31: ret
    0xBB, 0x01, 0x00, 0x00, 0x00,       // 32: mov ebx, 1  (taken)
    0xC3,                                // 37: ret
};
static_assert(sizeof(test2_code) == 38);

static bool test_eflags_preservation() {
    printf("=== Test 2: EFLAGS preservation across memory dispatch ===\n");

    MmioMap mmio = make_mmio();
    auto exec = std::make_unique<XboxExecutor>();
    if (!setup_exec(*exec, mmio, 0x2000, test2_code, sizeof(test2_code)))
        return false;

    exec->run(0x2000, 100'000);

    printf("  EBX  = %u  \t(expected 1 = JE taken)\n", exec->ctx.gp[GP_EBX]);
    printf("  EDX  = %u  \t(expected 42 = loaded from RAM)\n", exec->ctx.gp[GP_EDX]);

    bool ok = exec->ctx.gp[GP_EBX] == 1u
           && exec->ctx.gp[GP_EDX] == 42u;

    printf("  %s\n", ok ? "PASS" : "FAIL");
    exec->destroy();
    return ok;
}

// ===========================================================================

int main() {
    bool all_pass = true;

    all_pass &= test_sum_loop();
    printf("\n");
    all_pass &= test_eflags_preservation();

    printf("\n%s\n", all_pass ? "ALL PASS" : "SOME FAILED");
    return all_pass ? 0 : 1;
}
