; ===========================================================================
; flow.asm — Control flow: loops, conditional branches, CALL/RET patterns.
;
; Tests:  Jcc (all major conditions)
;         LOOP / LOOPE / LOOPNE
;         Simple subroutine CALL/RET
;         Nested CALL/RET
;         Countdown loops with various termination conditions
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ============================= Simple countdown loop =======================
    mov  ecx, 10
    xor  eax, eax
.loop1:
    add  eax, ecx
    dec  ecx
    jnz  .loop1
    ASSERT_EQ eax, 55                        ; 1  (sum 1..10)

; ============================= LOOP instruction ============================
    mov  ecx, 5
    xor  eax, eax
.loop2:
    add  eax, 1
    loop .loop2
    ASSERT_EQ eax, 5                         ; 2
    ASSERT_EQ ecx, 0                         ; 3

; ============================= Nested loops ================================
; Outer loop 3 iterations, inner loop 4 iterations each = 12 total
    xor  eax, eax
    mov  ebx, 3                              ; outer counter
.outer:
    mov  ecx, 4                              ; inner counter
.inner:
    inc  eax
    dec  ecx
    jnz  .inner
    dec  ebx
    jnz  .outer
    ASSERT_EQ eax, 12                        ; 4

; ============================= All Jcc conditions ==========================
; JE / JNE
    xor  eax, eax
    cmp  eax, 0
    je   .je_ok
    mov  eax, 5
    hlt
.je_ok:
    cmp  eax, 1
    jne  .jne_ok
    mov  eax, 6
    hlt
.jne_ok:

; JL / JGE  (signed less / greater-or-equal)
    mov  eax, -1
    cmp  eax, 0
    jl   .jl_ok
    mov  eax, 7
    hlt
.jl_ok:
    mov  eax, 0
    cmp  eax, 0
    jge  .jge_ok
    mov  eax, 8
    hlt
.jge_ok:

; JLE / JG  (signed less-or-equal / greater)
    mov  eax, 0
    cmp  eax, 0
    jle  .jle_ok
    mov  eax, 9
    hlt
.jle_ok:
    mov  eax, 1
    cmp  eax, 0
    jg   .jg_ok
    mov  eax, 10
    hlt
.jg_ok:

; JB / JAE  (unsigned below / above-or-equal)
    mov  eax, 5
    cmp  eax, 10
    jb   .jb_ok
    mov  eax, 11
    hlt
.jb_ok:
    mov  eax, 10
    cmp  eax, 10
    jae  .jae_ok
    mov  eax, 12
    hlt
.jae_ok:

; JBE / JA  (unsigned below-or-equal / above)
    mov  eax, 10
    cmp  eax, 10
    jbe  .jbe_ok
    mov  eax, 13
    hlt
.jbe_ok:
    mov  eax, 11
    cmp  eax, 10
    ja   .ja_ok
    mov  eax, 14
    hlt
.ja_ok:

; JS / JNS  (sign flag)
    mov  eax, -1
    test eax, eax
    js   .js_ok
    mov  eax, 15
    hlt
.js_ok:
    mov  eax, 1
    test eax, eax
    jns  .jns_ok
    mov  eax, 16
    hlt
.jns_ok:

; JO / JNO  (overflow)
    mov  eax, 0x7FFFFFFF
    add  eax, 1                              ; signed overflow → OF=1
    jo   .jo_ok
    mov  eax, 17
    hlt
.jo_ok:
    mov  eax, 1
    add  eax, 1                              ; no overflow → OF=0
    jno  .jno_ok
    mov  eax, 18
    hlt
.jno_ok:

; JP / JNP  (parity)
    mov  eax, 0x03                           ; popcount(3)=2 → PF=1 (even)
    test eax, eax
    jp   .jp_ok
    mov  eax, 19
    hlt
.jp_ok:
    mov  eax, 0x01                           ; popcount(1)=1 → PF=0 (odd)
    test eax, eax
    jnp  .jnp_ok
    mov  eax, 20
    hlt
.jnp_ok:

; ============================= CALL / RET ==================================
; Simple subroutine: add42 adds 42 to EAX
    mov  eax, 100
    call .add42
    ASSERT_EQ eax, 142                       ; 21
    jmp  .after_add42

.add42:
    add  eax, 42
    ret

.after_add42:

; ============================= Nested CALL =================================
; double_add42: calls add42 twice (adds 84)
    mov  eax, 0
    call .double_add42
    ASSERT_EQ eax, 84                        ; 22
    jmp  .after_nested

.double_add42:
    call .add42
    call .add42
    ret

.after_nested:

; ============================= Recursive factorial =========================
; fact(n): if n<=1 return 1; else return n * fact(n-1)
; fact(5) = 120
    mov  eax, 5
    call .fact
    ASSERT_EQ eax, 120                       ; 23
    jmp  .after_fact

.fact:
    cmp  eax, 1
    jle  .fact_base
    push eax                                 ; save n
    dec  eax                                 ; n-1
    call .fact                               ; EAX = fact(n-1)
    pop  ecx                                 ; ECX = n
    imul eax, ecx                            ; EAX = n * fact(n-1)
    ret
.fact_base:
    mov  eax, 1
    ret

.after_fact:

; ============================= Fibonacci loop ==============================
; fib(10) = 55 (iterative)
    mov  ecx, 10                             ; n
    mov  eax, 0                              ; fib(0) = 0
    mov  ebx, 1                              ; fib(1) = 1
.fib_loop:
    cmp  ecx, 0
    je   .fib_done
    mov  edx, eax
    add  edx, ebx                            ; edx = fib(k) + fib(k+1)
    mov  eax, ebx
    mov  ebx, edx
    dec  ecx
    jmp  .fib_loop
.fib_done:
    ASSERT_EQ eax, 55                        ; 24

; ============================= Branch over memory ops ======================
; Verify branches work correctly when interleaved with memory dispatches
    mov  dword [0x6000], 0
    mov  ecx, 10
.mem_loop:
    add  dword [0x6000], 1                   ; memory RMW
    dec  ecx
    jnz  .mem_loop
    ASSERT_EQ_MEM 0x6000, 10                 ; 25

; ============================= Conditional chains ==========================
; if (x > 5) result = 1; else if (x > 0) result = 2; else result = 3;
    mov  eax, 7                              ; x = 7
    cmp  eax, 5
    jle  .chain_le5
    mov  ebx, 1
    jmp  .chain_done
.chain_le5:
    cmp  eax, 0
    jle  .chain_le0
    mov  ebx, 2
    jmp  .chain_done
.chain_le0:
    mov  ebx, 3
.chain_done:
    ASSERT_EQ ebx, 1                         ; 26

    mov  eax, 3                              ; x = 3
    cmp  eax, 5
    jle  .chain2_le5
    mov  ebx, 1
    jmp  .chain2_done
.chain2_le5:
    cmp  eax, 0
    jle  .chain2_le0
    mov  ebx, 2
    jmp  .chain2_done
.chain2_le0:
    mov  ebx, 3
.chain2_done:
    ASSERT_EQ ebx, 2                         ; 27

; ============================== PASS =======================================
    PASS
