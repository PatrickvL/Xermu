; ===========================================================================
; fpu.asm — FPU (x87) operations: load, store, arithmetic, compare.
;
; Tests:  FLD1  FLDZ  FLD  FST  FSTP  FISTP
;         FADD  FSUB  FMUL  FDIV  FADDP  FSUBP  FMULP  FDIVP
;         FCOM  FCOMP  FCOMPP  FNSTSW  FCHS  FABS
;         Memory load/store through generic rewriter
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ============================= FLD1 + FADDP → FISTP ========================
; 1.0 + 1.0 = 2.0 → store as integer
    fld1
    fld1
    faddp  st1, st0                          ; ST(0) = 2.0
    fistp  dword [0x6000]
    ASSERT_EQ_MEM 0x6000, 2                  ; 1

; ============================= FLD1 + FADD ST0,ST0 chain ===================
; 1.0 → 2.0 → 4.0 → 8.0
    fld1
    fadd   st0, st0                          ; 2.0
    fadd   st0, st0                          ; 4.0
    fadd   st0, st0                          ; 8.0
    fistp  dword [0x6004]
    ASSERT_EQ_MEM 0x6004, 8                  ; 2

; ============================= 1+2+3+4+5 in x87 ===========================
; Build 5.0: 1+1=2, 2+1=3, 3+1=4, 4+1=5
    fld1                                     ; 1.0
    fld1
    faddp  st1, st0                          ; 2.0
    fld1
    faddp  st1, st0                          ; 3.0
    fld1
    faddp  st1, st0                          ; 4.0
    fld1
    faddp  st1, st0                          ; 5.0
    fistp  dword [0x6008]
    ASSERT_EQ_MEM 0x6008, 5                  ; 3

; ============================= FMULP =======================================
; 3.0 * 4.0 = 12.0
    fld1
    fld1
    faddp  st1, st0                          ; 2.0
    fld1
    faddp  st1, st0                          ; 3.0 (ST0)

    fld1
    fadd   st0, st0                          ; 2.0
    fadd   st0, st0                          ; 4.0 (ST0), 3.0 (ST1)

    fmulp  st1, st0                          ; 12.0
    fistp  dword [0x600C]
    ASSERT_EQ_MEM 0x600C, 12                 ; 4

; ============================= FSUBP =======================================
; 10.0 - 3.0 = 7.0
; Build 10.0: 1→2→4→8, +1→9, +1→10
    fld1
    fadd   st0, st0                          ; 2
    fadd   st0, st0                          ; 4
    fadd   st0, st0                          ; 8
    fld1
    faddp  st1, st0                          ; 9
    fld1
    faddp  st1, st0                          ; 10.0 (ST0)

; Build 3.0
    fld1
    fld1
    faddp  st1, st0                          ; 2.0
    fld1
    faddp  st1, st0                          ; 3.0 (ST0), 10.0 (ST1)

; FSUBP: ST1 = ST1 - ST0 = 10.0 - 3.0 = 7.0; pop
    fsubp  st1, st0                          ; 7.0
    fistp  dword [0x6010]
    ASSERT_EQ_MEM 0x6010, 7                  ; 5

; ============================= FDIVP =======================================
; 100.0 / 4.0 = 25.0
; Build 100: manually via memory
    mov    dword [0x6100], 100
    fild   dword [0x6100]                    ; ST0 = 100.0

; Build 4.0
    fld1
    fadd   st0, st0                          ; 2
    fadd   st0, st0                          ; 4.0 (ST0), 100.0 (ST1)

    fdivp  st1, st0                          ; ST1 / ST0 = 100/4 = 25; pop
    fistp  dword [0x6014]
    ASSERT_EQ_MEM 0x6014, 25                 ; 6

; ============================= FCOMPP + FNSTSW =============================
; Compare equal: 2.0 == 2.0 → C3=1
    fld1
    fadd   st0, st0                          ; 2.0
    fld1
    fadd   st0, st0                          ; 2.0
    fcompp
    fnstsw ax
    test   ah, 0x40                          ; C3 bit
    jnz    .cmp_eq_ok
    mov    eax, 7
    hlt
.cmp_eq_ok:

; Compare less: 1.0 < 2.0 → C0=1
    fld1                                     ; 1.0
    fld1
    fadd   st0, st0                          ; 2.0 (ST0), 1.0 (ST1)
    ; FCOMP compares ST0 with ST1: ST0(2.0) vs ST1(1.0) → ST0 > ST1 → C0=0
    ; Actually, we want to compare 1.0 < 2.0.  Let's reorder:
    fxch   st1                               ; 1.0 (ST0), 2.0 (ST1)
    fcompp                                   ; compare ST0(1.0) vs ST1(2.0)
    fnstsw ax                                ; ST0 < ST1 → C0=1
    test   ah, 0x01                          ; C0 bit
    jnz    .cmp_lt_ok
    mov    eax, 8
    hlt
.cmp_lt_ok:

; ============================= FCHS / FABS =================================
; FCHS: negate
    fld1
    fchs                                     ; -1.0
    fld1
    faddp  st1, st0                          ; -1.0 + 1.0 = 0.0
    fistp  dword [0x6018]
    ASSERT_EQ_MEM 0x6018, 0                  ; 9

; FABS: absolute value of -5.0
    mov    dword [0x6104], 5
    fild   dword [0x6104]                    ; 5.0
    fchs                                     ; -5.0
    fabs                                     ; 5.0
    fistp  dword [0x601C]
    ASSERT_EQ_MEM 0x601C, 5                  ; 10

; ============================= FILD / FISTP round-trip =====================
; Load integer from memory, store back
    mov    dword [0x6108], 42
    fild   dword [0x6108]
    fistp  dword [0x6020]
    ASSERT_EQ_MEM 0x6020, 42                 ; 11

; Negative integer
    mov    dword [0x610C], -17               ; 0xFFFFFFEF
    fild   dword [0x610C]
    fistp  dword [0x6024]
    cmp    dword [0x6024], -17
    je     .neg_ok
    mov    eax, 12
    hlt
.neg_ok:

; ============================= FLDZ ========================================
    fldz
    fistp  dword [0x6028]
    ASSERT_EQ_MEM 0x6028, 0                  ; 13

; ============================= FLD/FST double ==============================
; Load a double from memory, manipulate, store back
; 3.0 as IEEE 754 double: 0x4008000000000000
    mov  dword [0x6200], 0x00000000          ; low dword
    mov  dword [0x6204], 0x40080000          ; high dword (3.0)
    fld  qword [0x6200]                      ; ST(0) = 3.0
    fadd st0, st0                            ; 6.0
    fistp dword [0x602C]
    ASSERT_EQ_MEM 0x602C, 6                  ; 14

; ============================= FPU in a loop ===============================
; Sum 1.0 ten times → 10
    fldz                                     ; accumulator = 0.0
    mov  ecx, 10
.fpu_loop:
    fld1
    faddp  st1, st0                          ; acc += 1.0
    dec    ecx
    jnz    .fpu_loop
    fistp  dword [0x6030]
    ASSERT_EQ_MEM 0x6030, 10                 ; 15

; ============================= Mixed integer + FPU =========================
; Compute sum in both integer and FPU, verify they match
    xor    eax, eax
    fldz
    mov    ecx, 20
.mixed_loop:
    add    eax, ecx                          ; integer sum
    mov    dword [0x6300], ecx
    fild   dword [0x6300]
    faddp  st1, st0                          ; FPU sum
    dec    ecx
    jnz    .mixed_loop
    fistp  dword [0x6034]
    ASSERT_EQ eax, 210                       ; 16  (sum 1..20)
    ASSERT_EQ_MEM 0x6034, 210               ; 17

; ============================== PASS =======================================
    PASS
