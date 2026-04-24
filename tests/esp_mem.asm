; ===========================================================================
; esp_mem.asm — ESP edge cases with memory operands.
;
; Tests scenarios where ESP appears in ALU/MOV/MOVZX/MOVSX instructions
; together with a memory operand.  Guest ESP is NOT in a host register,
; so the JIT must route through R8D (for ESP-in-reg-field memory ops)
; to avoid corrupting host RSP.
;
; Tests:
;   1.  ADD [mem], ESP — store (mem + guest_ESP) to memory
;   2.  ADD ESP, [mem] — add memory value to guest ESP
;   3.  SUB [mem], ESP — store (mem - guest_ESP) to memory
;   4.  CMP [mem], ESP — compare (no write-back)
;   5.  AND ESP, [mem] — AND memory value into guest ESP
;   6.  MOVZX ESP, BYTE [mem]  — zero-extend byte into guest ESP
;   7.  MOVSX ESP, BYTE [mem]  — sign-extend byte into guest ESP
;   8.  XOR [mem], ESP — XOR guest ESP into memory
;   9.  OR ESP, [mem]  — OR memory value into guest ESP
;  10.  MOVZX ESP, WORD [mem]  — zero-extend word into guest ESP
; ===========================================================================

%include "harness.inc"

ORG 0x1000

    ; Set up a known ESP value.  Use LEA to avoid triggering MOV ESP,imm
    ; (which is already tested).  We set ESP = 0x80000 (our stack base).
    ; The harness already sets ESP = 0x80000, so leave it.

    ; Use memory at 0x70000 as a scratch area.
    mov edi, 0x70000

; === 1. ADD [mem], ESP ===
; [0x70000] = 100, ESP = 0x80000 → [0x70000] = 0x80064
    mov dword [edi], 100
    add [edi], esp
    mov eax, [edi]
    ASSERT_EQ eax, (0x80000 + 100)

; === 2. ADD ESP, [mem] ===
; ESP = 0x80000, [0x70004] = 0x100 → ESP = 0x80100
    mov dword [edi+4], 0x100
    add esp, [edi+4]
    ; Verify ESP changed:
    mov eax, esp
    ASSERT_EQ eax, 0x80100

    ; Restore ESP for remaining tests.
    sub esp, 0x100        ; ESP = 0x80000

; === 3. SUB [mem], ESP ===
; [0x70008] = 0x90000, ESP = 0x80000 → [0x70008] = 0x10000
    mov dword [edi+8], 0x90000
    sub [edi+8], esp
    mov eax, [edi+8]
    ASSERT_EQ eax, 0x10000

; === 4. CMP [mem], ESP (no write-back, just set flags) ===
; [0x70008] = 0x10000, ESP = 0x80000 → 0x10000 < 0x80000, so CF=1
    cmp [edi+8], esp
    jb .cmp_ok
    mov eax, __test_num + 1
    hlt
.cmp_ok:
    %assign __test_num __test_num + 1

; === 5. AND ESP, [mem] ===
; ESP = 0x80000 = 0b 1000 0000 0000 0000 0000
; [0x70010] = 0xFFF00000 → ESP = 0x80000 & 0xFFF00000 = 0x00000000
; Hmm, that zeros ESP which makes the stack unusable.
; Use a value that preserves useful bits.
; ESP = 0x80000, [0x70010] = 0xFFFF0000 → ESP = 0x80000 & 0xFFFF0000 = 0x80000
    mov dword [edi+16], 0xFFFF0000
    and esp, [edi+16]
    mov eax, esp
    ASSERT_EQ eax, 0x80000

; === 6. MOVZX ESP, BYTE [mem] ===
; [0x70020] = 0xAB → ESP = 0x000000AB
    mov byte [edi+32], 0xAB
    movzx esp, byte [edi+32]
    mov eax, esp
    ASSERT_EQ eax, 0xAB
    ; Restore ESP
    mov esp, 0x80000

; === 7. MOVSX ESP, BYTE [mem] ===
; [0x70021] = 0x80 (negative) → ESP = 0xFFFFFF80
    mov byte [edi+33], 0x80
    movsx esp, byte [edi+33]
    mov eax, esp
    ASSERT_EQ eax, 0xFFFFFF80
    ; Restore ESP
    mov esp, 0x80000

; === 8. XOR [mem], ESP ===
; [0x70024] = 0xDEADBEEF, ESP = 0x80000
; → [0x70024] = 0xDEADBEEF ^ 0x80000 = 0xDEA3BEEF
    mov dword [edi+36], 0xDEADBEEF
    xor [edi+36], esp
    mov eax, [edi+36]
    ASSERT_EQ eax, (0xDEADBEEF ^ 0x80000)

; === 9. OR ESP, [mem] ===
; ESP = 0x80000, [0x70028] = 0x0F → ESP = 0x8000F
    mov dword [edi+40], 0x0F
    or esp, [edi+40]
    mov eax, esp
    ASSERT_EQ eax, 0x8000F
    ; Restore ESP
    mov esp, 0x80000

; === 10. MOVZX ESP, WORD [mem] ===
; [0x7002C] = 0x1234 → ESP = 0x00001234
    mov word [edi+44], 0x1234
    movzx esp, word [edi+44]
    mov eax, esp
    ASSERT_EQ eax, 0x1234
    ; Restore ESP
    mov esp, 0x80000

; === 11. MOV ESP, imm32 ===
    mov esp, 0x12345678
    mov eax, esp
    ASSERT_EQ eax, 0x12345678
    ; Restore ESP
    mov esp, 0x80000

    PASS
