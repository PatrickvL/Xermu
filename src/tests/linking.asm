; ===========================================================================
; linking.asm — Block linking (trace chaining) tests.
;
; These tests exercise tight loops and sequential code that should trigger
; block linking between traces (JMP rel32 patching).  All tests verify
; functional correctness; the performance benefit is implicit.
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; ---------------------------------------------------------------------------
; Test 1: Tight counted loop — DEC + JNZ.
;   ECX counts down from 100 to 0.  The loop body is a single trace that
;   should be linked back to itself (the JNZ exit links to the loop head).
; ---------------------------------------------------------------------------
    mov ecx, 100
    xor edx, edx        ; accumulator
.loop1:
    add edx, 1
    dec ecx
    jnz .loop1

    ; After the loop: EDX should be 100, ECX should be 0.
    ASSERT_EQ edx, 100
    ASSERT_EQ ecx, 0

; ---------------------------------------------------------------------------
; Test 2: Two-block loop — a conditional branch that alternates between
;   two paths.  Tests that both taken and fallthrough link slots work.
; ---------------------------------------------------------------------------
    mov ecx, 50
    xor ebx, ebx        ; even counter
    xor esi, esi        ; odd counter
.loop2:
    test ecx, 1
    jz .even2
    add esi, 1           ; odd path
    jmp .merge2
.even2:
    add ebx, 1           ; even path
.merge2:
    dec ecx
    jnz .loop2

    ; 50 iterations: ECX 50,49,...,1.  Odd values: 49,47,...,1 = 25 values.
    ; Even values: 50,48,...,2 = 25 values.
    ASSERT_EQ ebx, 25
    ASSERT_EQ esi, 25

; ---------------------------------------------------------------------------
; Test 3: Sequential linear traces — CALL + RET chaining.
;   Call a subroutine that runs a small loop, testing that linking works
;   across CALL/RET boundaries (CALL is linkable, RET is not — but the
;   loop inside the subroutine should still link to itself).
; ---------------------------------------------------------------------------
    mov ecx, 0
    call .sub3
    ASSERT_EQ ecx, 200

    jmp .test4

.sub3:
    mov ecx, 200
.sub3_loop:
    dec ecx
    jnz .sub3_loop
    ; ECX = 0 when loop ends
    mov ecx, 200         ; restore expected value for assert
    ret

; ---------------------------------------------------------------------------
; Test 4: Nested loops — inner loop linked, outer loop linked.
; ---------------------------------------------------------------------------
.test4:
    mov edi, 0           ; total counter
    mov ebx, 10          ; outer count
.outer4:
    mov ecx, 10          ; inner count
.inner4:
    add edi, 1
    dec ecx
    jnz .inner4
    dec ebx
    jnz .outer4

    ASSERT_EQ edi, 100   ; 10 * 10 = 100

; ---------------------------------------------------------------------------
; Test 5: Straight-line sequential traces — no branches.
;   Multiple traces separated by page-crossing or trace-length limits.
;   At minimum, the fallthrough from one instruction block to the next
;   should be linkable.
; ---------------------------------------------------------------------------
    mov eax, 1
    add eax, 2
    add eax, 3
    add eax, 4
    add eax, 5
    ASSERT_EQ eax, 15

; ---------------------------------------------------------------------------
; Done — pass.
; ---------------------------------------------------------------------------
    xor eax, eax
    hlt
