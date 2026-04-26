; ===========================================================================
; div_error.asm — #DE (divide-by-zero, vector 0) exception delivery tests.
;
; Tests:
;   1. DIV reg32 by zero → #DE ISR fires, IRETD resumes at skip label
;   2. IDIV reg32 by zero → #DE ISR fires
;   3. DIV reg16 by zero → #DE ISR fires
;
; The ISR uses a monotonically-increasing de_count (never reset) as an index
; into skip_table to know where to resume after each #DE.
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
    ; ---- Set up IDT entry for vector 0 (#DE) ----
    BUILD_IDT_ENTRY 0, isr_de

    ; ---- Load IDT ----
    lidt [idt_desc]

    ; ---- Init: de_count = 0 (monotonic — never reset) ----
    mov dword [de_count], 0

    ; ==== Test 1: DIV ECX where ECX=0 → #DE ====
    xor edx, edx
    mov eax, 42
    xor ecx, ecx          ; divisor = 0
    div ecx                ; should fault → #DE
skip1:
    ; de_count should now be 1
    cmp dword [de_count], 1
    je .t1_ok
    mov eax, 1
    hlt
.t1_ok:

    ; ==== Test 2: IDIV ECX where ECX=0 → #DE ====
    xor edx, edx
    mov eax, 42
    xor ecx, ecx
    idiv ecx
skip2:
    cmp dword [de_count], 2
    je .t2_ok
    mov eax, 2
    hlt
.t2_ok:

    ; ==== Test 3: DIV 16-bit (DX:AX / CX) where CX=0 → #DE ====
    xor edx, edx         ; clear EDX fully (not just DX)
    mov eax, 100
    xor ecx, ecx
    div cx
skip3:
    cmp dword [de_count], 3
    je .t3_ok
    mov eax, 3
    hlt
.t3_ok:

    PASS

; ---------------------------------------------------------------------------
; #DE ISR — increments de_count, adjusts return EIP to the skip label
; that follows the faulting DIV/IDIV instruction.
;
; Stack on entry (no error code for #DE):
;   [ESP+0] = return EIP  (points to faulting instruction)
;   [ESP+4] = CS
;   [ESP+8] = EFLAGS
;
; de_count is a monotonic counter; its value before increment gives the
; index into skip_table for the correct resume address.
; ---------------------------------------------------------------------------
isr_de:
    push ebx
    push eax

    ; Increment counter
    mov eax, [de_count]
    mov ebx, eax              ; old count = index into skip table
    inc eax
    mov [de_count], eax

    ; Look up the resume address from the skip table
    mov eax, [skip_table + ebx*4]
    mov [esp+8], eax          ; overwrite saved EIP on interrupt frame

    pop eax
    pop ebx
    iretd

; ---------------------------------------------------------------------------
; Data (inline in .text — this is a flat binary)
; ---------------------------------------------------------------------------
align 4
de_count: dd 0

skip_table:
    dd skip1
    dd skip2
    dd skip3

idt_desc:
    dw 256*8 - 1      ; limit
    dd IDT_BASE        ; base
