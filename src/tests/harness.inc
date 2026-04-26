; ===========================================================================
; harness.inc — NASM test framework for flat 32-bit protected mode tests.
;
; Convention:
;   [BITS 32], loaded at ORG address (typically 0x1000).
;   EAX = 0 on pass, non-zero error code on fail.
;   Test ends with HLT.
;   Port 0xE9: debug console output  (bochs-style).
; ===========================================================================

[BITS 32]

%assign __test_num 0

; ---------------------------------------------------------------------------
; ASSERT_EQ  reg, expected
;   Compare a 32-bit register to an immediate value.  If not equal, halt
;   with the assertion number in EAX.
; ---------------------------------------------------------------------------
%macro ASSERT_EQ 2
    %assign __test_num __test_num + 1
    cmp %1, %2
    je %%ok
    mov eax, __test_num
    hlt
%%ok:
%endmacro

; ---------------------------------------------------------------------------
; ASSERT_EQ_MEM  addr, expected
;   Compare a DWORD at the given address to an immediate value.
; ---------------------------------------------------------------------------
%macro ASSERT_EQ_MEM 2
    %assign __test_num __test_num + 1
    cmp dword [%1], %2
    je %%ok
    mov eax, __test_num
    hlt
%%ok:
%endmacro

; ---------------------------------------------------------------------------
; ASSERT_NE  reg, unexpected
;   Fail if register equals the given value.
; ---------------------------------------------------------------------------
%macro ASSERT_NE 2
    %assign __test_num __test_num + 1
    cmp %1, %2
    jne %%ok
    mov eax, __test_num
    hlt
%%ok:
%endmacro

; ---------------------------------------------------------------------------
; ASSERT_FLAGS  flag_mask, expected_value
;   Push flags, AND with mask, compare to expected.  Clobbers EAX.
;   flag_mask and expected_value are immediates.
; ---------------------------------------------------------------------------
%macro ASSERT_FLAGS 2
    %assign __test_num __test_num + 1
    pushfd
    pop eax
    and eax, %1
    cmp eax, %2
    je %%ok
    mov eax, __test_num
    hlt
%%ok:
%endmacro

; ---------------------------------------------------------------------------
; PASS — signal success and halt.
; ---------------------------------------------------------------------------
%macro PASS 0
    xor eax, eax
    hlt
%endmacro

; ---------------------------------------------------------------------------
; PUTC  char — write a character to the debug console (port 0xE9).
; Clobbers nothing (pushes/pops EAX, EDX).
; ---------------------------------------------------------------------------
%macro PUTC 1
    push eax
    push edx
    mov  al, %1
    mov  dx, 0xE9
    out  dx, al
    pop  edx
    pop  eax
%endmacro

; ---------------------------------------------------------------------------
; PUTS  label — write a NUL-terminated string at `label` to debug console.
; Clobbers nothing.
; ---------------------------------------------------------------------------
%macro PUTS 1
    push esi
    push eax
    push edx
    mov  esi, %1
    mov  dx, 0xE9
%%loop:
    lodsb
    test al, al
    jz   %%done
    out  dx, al
    jmp  %%loop
%%done:
    pop  edx
    pop  eax
    pop  esi
%endmacro

; EFLAGS bit definitions
%define CF  0x001
%define PF  0x004
%define AF  0x010
%define ZF  0x040
%define SF  0x080
%define OF  0x800
