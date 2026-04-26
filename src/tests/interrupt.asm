; ===========================================================================
; interrupt.asm — INT n / IDT / IRETD roundtrip tests.
;
; Tests:
;   1. INT 0x80 → ISR sets EDX=0xCAFEBABE, IRETD → verify EDX
;   2. INT 0x81 → ISR increments ECX, IRETD → verify ECX
;   3. Chained: INT 0x80 after INT 0x81 → verify both results
;   4. Nested: ISR for 0x82 does INT 0x83 internally → verify both
;   5. CLI/STI roundtrip: CLI, STI, verify execution continues
;   6. INT3 → vector 3 ISR → IRETD
;   7. Stack integrity: ESP correct after INT/IRETD
;   8. IRETD restores EFLAGS (CF set before INT, preserved after)
; ===========================================================================

%include "harness.inc"

; IDT lives at PA 0x3000 (256 entries × 8 bytes = 2048 bytes → 0x3000..0x37FF)
IDT_BASE equ 0x3000

; ---------------------------------------------------------------------------
; Macro: build a 32-bit interrupt gate descriptor at IDT_BASE + vector*8.
; handler_label must be a code address.
; CS selector = 0x0008 (flat code segment, matches typical protected mode).
; Type = 0x8E (32-bit interrupt gate, DPL=0, present).
; ---------------------------------------------------------------------------
%macro BUILD_IDT_ENTRY 2  ; %1=vector, %2=handler_label
    mov eax, %2
    mov word [IDT_BASE + %1*8 + 0], ax      ; offset 15:0
    mov word [IDT_BASE + %1*8 + 2], 0x0008  ; CS selector
    mov byte [IDT_BASE + %1*8 + 4], 0       ; reserved
    mov byte [IDT_BASE + %1*8 + 5], 0x8E    ; type: interrupt gate, present
    shr eax, 16
    mov word [IDT_BASE + %1*8 + 6], ax      ; offset 31:16
%endmacro

; ---------------------------------------------------------------------------
section .text
org 0x1000

start:
    ; ---- Set up IDT entries ----
    BUILD_IDT_ENTRY 0x03, isr_int3      ; vector 3 (INT3)
    BUILD_IDT_ENTRY 0x80, isr_80        ; vector 0x80
    BUILD_IDT_ENTRY 0x81, isr_81        ; vector 0x81
    BUILD_IDT_ENTRY 0x82, isr_82_outer  ; vector 0x82 (nests into 0x83)
    BUILD_IDT_ENTRY 0x83, isr_83_inner  ; vector 0x83

    ; ---- Load IDT ----
    lidt [idt_desc]

    ; ==== Test 1: INT 0x80 → ISR sets EDX ====
    xor edx, edx
    int 0x80
    ASSERT_EQ edx, 0xCAFEBABE

    ; ==== Test 2: INT 0x81 → ISR increments ECX ====
    xor ecx, ecx
    int 0x81
    ASSERT_EQ ecx, 1

    ; ==== Test 3: Chained — call both, verify independent results ====
    xor edx, edx
    xor ecx, ecx
    int 0x81          ; ECX = 1
    int 0x80          ; EDX = 0xCAFEBABE
    int 0x81          ; ECX = 2
    ASSERT_EQ edx, 0xCAFEBABE
    ASSERT_EQ ecx, 2

    ; ==== Test 4: Nested — ISR 0x82 calls INT 0x83 ====
    xor ebx, ebx
    int 0x82
    ; ISR 0x82 sets EBX bit 0; ISR 0x83 (called from 0x82) sets EBX bit 1
    ASSERT_EQ ebx, 3    ; bits 0 and 1 both set

    ; ==== Test 5: CLI / STI — verify execution continues ====
    mov esi, 0
    cli
    mov esi, 1          ; should still execute after CLI
    sti
    mov esi, 2          ; should still execute after STI
    ASSERT_EQ esi, 2

    ; ==== Test 6: INT3 → vector 3 ISR ====
    xor edi, edi
    int3
    ASSERT_EQ edi, 0x33333333

    ; ==== Test 7: Stack integrity after INT/IRETD ====
    mov eax, esp        ; save ESP before
    int 0x80
    ASSERT_EQ esp, eax  ; ESP must be restored to same value

    ; ==== Test 8: IRETD restores EFLAGS (CF) ====
    stc                 ; set CF
    int 0x80            ; ISR does CLC (clears CF), but IRETD restores old EFLAGS
    jc .cf_ok           ; CF should be set again after IRETD
    mov eax, __test_num + 1
    hlt
.cf_ok:
    %assign __test_num __test_num + 1

    PASS

; ---------------------------------------------------------------------------
; ISR handlers
; ---------------------------------------------------------------------------

isr_80:
    ; Set EDX = 0xCAFEBABE, clear CF to prove IRETD restores EFLAGS
    mov edx, 0xCAFEBABE
    clc
    iretd

isr_81:
    ; Increment ECX
    inc ecx
    iretd

isr_82_outer:
    ; Set EBX bit 0, then nest into vector 0x83
    or ebx, 1
    sti                 ; re-enable interrupts (INT gate clears IF)
    int 0x83
    iretd

isr_83_inner:
    ; Set EBX bit 1
    or ebx, 2
    iretd

isr_int3:
    ; Set EDI = 0x33333333
    mov edi, 0x33333333
    iretd

; ---------------------------------------------------------------------------
; IDT pseudo-descriptor (6 bytes: limit[16] + base[32])
; ---------------------------------------------------------------------------
idt_desc:
    dw 256*8 - 1        ; limit: 256 entries × 8 bytes - 1
    dd IDT_BASE          ; base address
