; ===========================================================================
; mmio_alu.asm — Non-patchable MMIO emulation tests.
;
; Exercises ALU/TEST/BIT instructions on the test MMIO device at 0x08000000.
; These instructions produce non-patchable mem_sites (patch_len=0) and are
; handled by software emulation in the VEH handler.
;
; Tests:
;   1.  MOV to MMIO, then OR [mmio], imm → read back result
;   2.  AND [mmio], imm → clears bits
;   3.  XOR [mmio], imm → toggles bits
;   4.  TEST [mmio], imm → sets ZF correctly (non-zero)
;   5.  TEST [mmio], imm → sets ZF correctly (zero result)
;   6.  ADD [mmio], imm → arithmetic add
;   7.  SUB [mmio], imm → arithmetic subtract
;   8.  INC [mmio] → increment
;   9.  DEC [mmio] → decrement
;  10.  NOT [mmio] → bitwise invert
;  11.  NEG [mmio] → two's complement negate
;  12.  BTS [mmio], imm → set bit, CF = old bit
;  13.  BTR [mmio], imm → reset bit, CF = old bit
;  14.  CMP [mmio], imm → sets flags without modifying memory
; ===========================================================================

%include "harness.inc"

MMIO equ 0x08000000     ; test MMIO device base
REG0 equ MMIO + 0       ; register at offset 0
REG4 equ MMIO + 4       ; register at offset 4

section .text
org 0x1000

start:
    ; Use ESI as MMIO base pointer.
    mov esi, MMIO

    ; === 1. OR [mmio], imm ===
    ; Write 0x000000F0 via MOV, then OR in 0x0F → expect 0xFF.
    mov dword [esi], 0xF0
    or  dword [esi], 0x0F
    mov eax, [esi]
    ASSERT_EQ eax, 0xFF

    ; === 2. AND [mmio], imm ===
    ; Start with 0xFF, AND with 0x0F → expect 0x0F.
    and dword [esi], 0x0F
    mov eax, [esi]
    ASSERT_EQ eax, 0x0F

    ; === 3. XOR [mmio], imm ===
    ; Start with 0x0F, XOR with 0xFF → expect 0xF0.
    xor dword [esi], 0xFF
    mov eax, [esi]
    ASSERT_EQ eax, 0xF0

    ; === 4. TEST [mmio], imm — non-zero result → ZF=0 ===
    mov dword [esi], 0xFF
    test dword [esi], 0x01
    jnz .t4_ok
    mov eax, 4
    hlt
.t4_ok:

    ; === 5. TEST [mmio], imm — zero result → ZF=1 ===
    mov dword [esi], 0xF0
    test dword [esi], 0x0F
    jz .t5_ok
    mov eax, 5
    hlt
.t5_ok:

    ; === 6. ADD [mmio], imm ===
    mov dword [esi], 100
    add dword [esi], 50
    mov eax, [esi]
    ASSERT_EQ eax, 150

    ; === 7. SUB [mmio], imm ===
    sub dword [esi], 30
    mov eax, [esi]
    ASSERT_EQ eax, 120

    ; === 8. INC [mmio] ===
    mov dword [esi], 41
    inc dword [esi]
    mov eax, [esi]
    ASSERT_EQ eax, 42

    ; === 9. DEC [mmio] ===
    dec dword [esi]
    mov eax, [esi]
    ASSERT_EQ eax, 41

    ; === 10. NOT [mmio] ===
    mov dword [esi], 0x000000FF
    not dword [esi]
    mov eax, [esi]
    ASSERT_EQ eax, 0xFFFFFF00

    ; === 11. NEG [mmio] ===
    mov dword [esi], 1
    neg dword [esi]
    mov eax, [esi]
    ASSERT_EQ eax, 0xFFFFFFFF   ; -1 in unsigned

    ; === 12. BTS [mmio], imm ===
    mov dword [esi], 0
    bts dword [esi], 3      ; set bit 3, old bit was 0 → CF=0
    jnc .t12_cf_ok
    mov eax, 12
    hlt
.t12_cf_ok:
    mov eax, [esi]
    ASSERT_EQ eax, 8           ; bit 3 set = 0x08

    ; === 13. BTR [mmio], imm ===
    btr dword [esi], 3      ; reset bit 3, old bit was 1 → CF=1
    jc .t13_cf_ok
    mov eax, 13
    hlt
.t13_cf_ok:
    mov eax, [esi]
    ASSERT_EQ eax, 0           ; bit 3 cleared

    ; === 14. CMP [mmio], imm ===
    mov dword [esi], 42
    cmp dword [esi], 42
    je .t14_ok
    mov eax, 14
    hlt
.t14_ok:

    PASS
