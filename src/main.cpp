#include "executor.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <memory>

// ---------------------------------------------------------------------------
// Guest program (hand-assembled, 32-bit protected mode, load PA = 0x1000):
//
//   ; Sum integers 1..10 into EAX, store to RAM, verify round-trip.
//   mov  eax, 0           ; sum = 0
//   mov  ecx, 10          ; i   = 10
// .loop:
//   add  eax, ecx         ; sum += i
//   dec  ecx              ; i--   (DEC ECX short-form 0x49 — re-encoded by JIT)
//   jnz  .loop            ; loop while i != 0
//   mov  [0x4000], eax    ; store sum (fastmem write)
//   mov  edx, [0x4000]    ; reload    (fastmem read — round-trip check)
//   mov  [0x4004], edx    ; store reload result
//   ; Place a known value in EBX so we can see it survives a trace boundary.
//   mov  ebx, 0xDEADBEEF
//   ret                   ; pop [ESP] → next_eip; run loop halts on OOB address
//
// Expected post-conditions:
//   ctx.gp[GP_EAX] = 55          (sum, still live when saved by ret epilog)
//   ctx.gp[GP_EBX] = 0xDEADBEEF (survives ret epilog save)
//   RAM[0x4000]    = 55          (fastmem write)
//   RAM[0x4004]    = 55          (fastmem read round-trip)
//
// Assembled bytes (AT&T / NASM compatible, verified by hand):
// ---------------------------------------------------------------------------

static const uint8_t guest_code[] = {
    // mov eax, 0
    0xB8, 0x00, 0x00, 0x00, 0x00,
    // mov ecx, 10
    0xB9, 0x0A, 0x00, 0x00, 0x00,
    // .loop:
    // add eax, ecx          (2 bytes, offset 10 = 0x100A)
    0x01, 0xC8,
    // dec ecx               (1 byte short form — JIT re-encodes to FF C9)
    0x49,
    // jnz .loop  rel8       (offset 10 = 0x100A; after jnz: 0x100F; delta=-5)
    0x75, 0xFB,
    // mov [0x4000], eax
    0x89, 0x05, 0x00, 0x40, 0x00, 0x00,
    // mov edx, [0x4000]
    0x8B, 0x15, 0x00, 0x40, 0x00, 0x00,
    // mov [0x4004], edx
    0x89, 0x15, 0x04, 0x40, 0x00, 0x00,
    // mov ebx, 0xDEADBEEF
    0xBB, 0xEF, 0xBE, 0xAD, 0xDE,
    // ret
    0xC3,
};

// Verify our byte layout manually:
// 0x1000 +  0 : B8 00 00 00 00        MOV EAX, 0
// 0x1000 +  5 : B9 0A 00 00 00        MOV ECX, 10
// 0x1000 + 10 : 01 C8                 ADD EAX, ECX  ← loop target
// 0x1000 + 12 : 49                    DEC ECX
// 0x1000 + 13 : 75 FB                 JNZ -5  →  10 = 0x100A ✓  (0x100F - 5)
// 0x1000 + 15 : 89 05 00 40 00 00     MOV [0x4000], EAX
// 0x1000 + 21 : 8B 15 00 40 00 00     MOV EDX, [0x4000]
// 0x1000 + 27 : 89 15 04 40 00 00     MOV [0x4004], EDX
// 0x1000 + 33 : BB EF BE AD DE        MOV EBX, 0xDEADBEEF
// 0x1000 + 38 : C3                    RET
static_assert(sizeof(guest_code) == 39, "guest_code length mismatch");

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

// ---------------------------------------------------------------------------

int main() {
    MmioMap mmio;
    mmio.add(GUEST_RAM_SIZE, ~0u - GUEST_RAM_SIZE,
             stub_mmio_read, stub_mmio_write);

    auto exec = std::make_unique<XboxExecutor>();
    if (!exec->init(&mmio)) { fprintf(stderr, "init failed\n"); return 1; }

    static constexpr uint32_t LOAD_PA = 0x1000;
    exec->load_guest(LOAD_PA, guest_code, sizeof(guest_code));

    // Stack well inside RAM; initial frame is just the sentinel return address.
    static constexpr uint32_t STACK_TOP = 0x0008'0000;
    exec->ctx.gp[GP_ESP] = STACK_TOP;
    // Place a real sentinel return address on the stack so RET can succeed
    // with a predictable EIP that the run loop will cleanly halt on.
    static constexpr uint32_t SENTINEL_EIP = 0xFFFF'FFFFu;
    memcpy(exec->ram + STACK_TOP, &SENTINEL_EIP, 4);

    exec->ctx.eflags = 0x0000'0202;

    printf("Running guest from PA %08X ...\n", LOAD_PA);
    exec->run(LOAD_PA, /*max_steps=*/100'000);

    // Post-conditions
    uint32_t ram4000 = 0, ram4004 = 0;
    memcpy(&ram4000, exec->ram + 0x4000, 4);
    memcpy(&ram4004, exec->ram + 0x4004, 4);

    printf("ctx.gp[EAX]  = %u  \t(expected 55)\n",   exec->ctx.gp[GP_EAX]);
    printf("ctx.gp[EBX]  = 0x%08X\t(expected 0xDEADBEEF)\n", exec->ctx.gp[GP_EBX]);
    printf("ctx.gp[ECX]  = %u  \t(expected 0)\n",    exec->ctx.gp[GP_ECX]);
    printf("RAM[0x4000]  = %u  \t(expected 55)\n",   ram4000);
    printf("RAM[0x4004]  = %u  \t(expected 55)\n",   ram4004);

    bool ok = exec->ctx.gp[GP_EAX] == 55u
           && exec->ctx.gp[GP_EBX] == 0xDEADBEEFu
           && exec->ctx.gp[GP_ECX] == 0u
           && ram4000              == 55u
           && ram4004              == 55u;

    printf("\n%s\n", ok ? "PASS" : "FAIL");
    exec->destroy();
    return ok ? 0 : 1;
}
