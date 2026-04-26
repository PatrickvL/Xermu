; ===========================================================================
; indirect.asm — Indirect control flow and memory stack operations.
;
; Tests:  JMP [mem]  — indirect jump through memory
;         CALL [mem] — indirect call through memory
;         PUSH [mem] — push memory operand onto stack
;         POP [mem]  — pop from stack into memory
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
;
; NOTE: Guest ESP is NOT mapped to host RSP.  Tests must NOT use
;       `mov reg, esp` to observe ESP.  PUSH/POP correctness is verified
;       by checking the pushed/popped values, not ESP arithmetic.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ========================= JMP [mem] =======================================
; Store target address in memory, then JMP through it.

    mov  dword [0x3000], .jmp_target1
    jmp  dword [0x3000]
    mov  eax, 1                              ; should not reach here
    hlt
.jmp_target1:
    ASSERT_EQ eax, 0                         ; 1 — EAX still 0 after JMP [mem]

; JMP [reg+disp] form
    mov  dword [0x3004], .jmp_target2
    mov  ebx, 0x3000
    jmp  dword [ebx+4]
    mov  eax, 2
    hlt
.jmp_target2:
    ASSERT_EQ eax, 0                         ; 2

; JMP [reg+reg*scale] form — jump table style
    mov  dword [0x3010], .jmp_t0
    mov  dword [0x3014], .jmp_t1
    mov  dword [0x3018], .jmp_t2
    mov  ebx, 0x3010
    mov  ecx, 2
    jmp  dword [ebx+ecx*4]
    mov  eax, 3
    hlt
.jmp_t0:
    mov  eax, 3
    hlt
.jmp_t1:
    mov  eax, 3
    hlt
.jmp_t2:
    ASSERT_EQ eax, 0                         ; 3 — jump table index 2

; ========================= CALL [mem] ======================================

; Simple CALL [mem] — call through a pointer in memory
    mov  dword [0x3020], .call_target1
    mov  ebx, 42
    call dword [0x3020]
    ASSERT_EQ ebx, 99                        ; 4 — callee set EBX=99
    jmp  .after_call1

.call_target1:
    mov  ebx, 99
    ret

.after_call1:

; CALL [reg+disp] — indirect call with base+displacement
    mov  dword [0x3024], .call_target2
    mov  edx, 0x3020
    xor  esi, esi
    call dword [edx+4]
    ASSERT_EQ esi, 77                        ; 5 — callee set ESI=77
    jmp  .after_call2

.call_target2:
    mov  esi, 77
    ret

.after_call2:

; CALL [mem] return address — verify callee can read return addr from stack
    mov  dword [0x3028], .call_target3
    call dword [0x3028]
    ASSERT_EQ ecx, 0xFE                     ; 6 — callee set ECX=0xFE
    jmp  .after_call3

.call_target3:
    mov  ecx, 0xFE
    ret

.after_call3:

; Nested CALL [mem] — two levels of indirect calls
    mov  dword [0x3030], .nest_outer
    mov  dword [0x3034], .nest_inner
    xor  ecx, ecx
    call dword [0x3030]
    ASSERT_EQ ecx, 3                         ; 7 — 1 + 2 from nested calls
    jmp  .after_nested

.nest_outer:
    add  ecx, 1
    call dword [0x3034]
    ret

.nest_inner:
    add  ecx, 2
    ret

.after_nested:

; ========================= PUSH [mem] =====================================

; PUSH dword [addr] — push a value from memory onto stack, verify by POP
    mov  dword [0x3040], 0xDEADBEEF
    push dword [0x3040]
    pop  eax
    ASSERT_EQ eax, 0xDEADBEEF               ; 8 — correct value pushed and popped
    mov  eax, 0

; PUSH [reg+disp]
    mov  dword [0x3044], 0x12345678
    mov  ebx, 0x3040
    push dword [ebx+4]
    pop  ecx
    ASSERT_EQ ecx, 0x12345678               ; 9

; PUSH [reg+reg*scale]
    mov  dword [0x3050], 0xAAAA0001
    mov  dword [0x3054], 0xBBBB0002
    mov  dword [0x3058], 0xCCCC0003
    mov  ebx, 0x3050
    mov  ecx, 1
    push dword [ebx+ecx*4]
    pop  edx
    ASSERT_EQ edx, 0xBBBB0002               ; 10

; Multiple PUSH [mem] — push 4 values, verify LIFO order via POP
    mov  dword [0x3060], 10
    mov  dword [0x3064], 20
    mov  dword [0x3068], 30
    mov  dword [0x306C], 40
    mov  ebx, 0x3060
    push dword [ebx+0]
    push dword [ebx+4]
    push dword [ebx+8]
    push dword [ebx+12]
    pop  eax
    ASSERT_EQ eax, 40                        ; 11
    pop  eax
    ASSERT_EQ eax, 30                        ; 12
    pop  eax
    ASSERT_EQ eax, 20                        ; 13
    pop  eax
    ASSERT_EQ eax, 10                        ; 14
    mov  eax, 0

; ========================= POP [mem] ======================================

; POP dword [addr] — pop from stack into memory location
    mov  dword [0x3070], 0
    push dword 0xCAFEBABE
    pop  dword [0x3070]
    ASSERT_EQ_MEM 0x3070, 0xCAFEBABE        ; 15

; POP [reg+disp]
    mov  dword [0x3074], 0
    mov  ebx, 0x3070
    push dword 0x55AA55AA
    pop  dword [ebx+4]
    ASSERT_EQ_MEM 0x3074, 0x55AA55AA        ; 16

; ========================= Combined patterns ==============================

; Function pointer table: CALL through array of function pointers
    mov  dword [0x3080], .fn_add10
    mov  dword [0x3084], .fn_mul3
    mov  dword [0x3088], .fn_sub5
    mov  eax, 7
    mov  ebx, 0x3080
    ; call fn_add10(7) → 17
    call dword [ebx+0]
    ASSERT_EQ eax, 17                        ; 17
    ; call fn_mul3(17) → 51
    call dword [ebx+4]
    ASSERT_EQ eax, 51                        ; 18
    ; call fn_sub5(51) → 46
    call dword [ebx+8]
    ASSERT_EQ eax, 46                        ; 19
    jmp  .after_fntable

.fn_add10:
    add  eax, 10
    ret
.fn_mul3:
    imul eax, 3
    ret
.fn_sub5:
    sub  eax, 5
    ret

.after_fntable:

; PUSH [mem] + POP [mem] round-trip
    mov  dword [0x3090], 0x11223344
    mov  dword [0x3094], 0
    push dword [0x3090]
    pop  dword [0x3094]
    ASSERT_EQ_MEM 0x3094, 0x11223344        ; 20

; ========================= ALL PASSED =====================================
    PASS
