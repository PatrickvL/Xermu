; ===========================================================================
; ud2.asm — #UD (invalid opcode, vector 6) exception delivery test.
;
; Tests:
;   1. UD2 instruction → #UD ISR fires, IRETD resumes after UD2
; ===========================================================================

%include "harness.inc"

IDT_BASE equ 0x3000

%macro BUILD_IDT_ENTRY 2
    mov eax, %2
    mov word [IDT_BASE + %1*8 + 0], ax
    mov word [IDT_BASE + %1*8 + 2], 0x0008
    mov byte [IDT_BASE + %1*8 + 4], 0
    mov byte [IDT_BASE + %1*8 + 5], 0x8E
    shr eax, 16
    mov word [IDT_BASE + %1*8 + 6], ax
%endmacro

section .text
org 0x1000

start:
    ; ---- Set up IDT entry for vector 6 (#UD) ----
    BUILD_IDT_ENTRY 6, isr_ud

    ; ---- Load IDT ----
    lidt [idt_desc]

    ; ---- Init ----
    mov dword [ud_count], 0

    ; ==== Test 1: UD2 → #UD ====
    ud2
skip1:
    cmp dword [ud_count], 1
    je .t1_ok
    mov eax, 1
    hlt
.t1_ok:

    PASS

; ---------------------------------------------------------------------------
; #UD ISR — increments ud_count, adjusts return EIP past the UD2.
; Stack on entry (no error code for #UD):
;   [ESP+0] = return EIP  (the UD2 instruction)
;   [ESP+4] = CS
;   [ESP+8] = EFLAGS
; ---------------------------------------------------------------------------
isr_ud:
    push eax

    inc dword [ud_count]

    ; Resume at skip1 (after UD2).
    mov eax, skip1
    mov [esp+4], eax          ; overwrite saved EIP

    pop eax
    iretd

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
align 4
ud_count: dd 0

idt_desc:
    dw 256*8 - 1      ; limit
    dd IDT_BASE        ; base
