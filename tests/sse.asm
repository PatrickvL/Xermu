; ===========================================================================
; sse.asm — SSE1 instruction tests with memory operands.
;
; Xbox CPU = Pentium III (Coppermine): SSE1 only, no SSE2.
;
; Tests: MOVAPS/MOVUPS load/store, ADDPS, SUBPS, MULPS, DIVPS,
;        scalar ADDSS/SUBSS/MULSS/DIVSS, XORPS, ANDPS, ORPS,
;        SHUFPS, CVTSI2SS/CVTTSS2SI, COMISS, MOVSS load/store,
;        SQRTSS, MINPS/MAXPS, UNPCKLPS/UNPCKHPS, LDMXCSR/STMXCSR.
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
;
; We store XMM results to memory and verify individual dwords via
; ASSERT_EQ_MEM.  All data is aligned to 16 bytes.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; Jump over data section
    jmp  code_start

; ============================= DATA (16-byte aligned) ======================
    align 16
vec_1234:   dd 1.0, 2.0, 3.0, 4.0
vec_5678:   dd 5.0, 6.0, 7.0, 8.0
vec_ones:   dd 1.0, 1.0, 1.0, 1.0
vec_twos:   dd 2.0, 2.0, 2.0, 2.0
scalar_3:   dd 3.0, 0.0, 0.0, 0.0
scalar_10:  dd 10.0, 0.0, 0.0, 0.0
scalar_4:   dd 4.0, 0.0, 0.0, 0.0

    align 16
result:     dd 0, 0, 0, 0          ; scratch space for storing XMM results

; ============================= CODE ========================================
    align 16
code_start:

; ---------------------------------------------------------------------------
; 1. MOVAPS load + ADDPS + MOVAPS store
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]            ; xmm0 = {1,2,3,4}
    movaps xmm1, [vec_5678]            ; xmm1 = {5,6,7,8}
    addps  xmm0, xmm1                  ; xmm0 = {6,8,10,12}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40C00000 ; 1  6.0f
    ASSERT_EQ_MEM result+4,  0x41000000 ; 2  8.0f
    ASSERT_EQ_MEM result+8,  0x41200000 ; 3  10.0f
    ASSERT_EQ_MEM result+12, 0x41400000 ; 4  12.0f

; ---------------------------------------------------------------------------
; 2. SUBPS
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_5678]            ; {5,6,7,8}
    subps  xmm0, [vec_1234]            ; {4,4,4,4}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40800000 ; 5  4.0f
    ASSERT_EQ_MEM result+4,  0x40800000 ; 6  4.0f
    ASSERT_EQ_MEM result+8,  0x40800000 ; 7  4.0f
    ASSERT_EQ_MEM result+12, 0x40800000 ; 8  4.0f

; ---------------------------------------------------------------------------
; 3. MULPS
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]
    mulps  xmm0, [vec_5678]            ; {5,12,21,32}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40A00000 ; 9  5.0f
    ASSERT_EQ_MEM result+4,  0x41400000 ; 10 12.0f
    ASSERT_EQ_MEM result+8,  0x41A80000 ; 11 21.0f
    ASSERT_EQ_MEM result+12, 0x42000000 ; 12 32.0f

; ---------------------------------------------------------------------------
; 4. DIVPS: {8,8,8,8} / {2,2,2,2} = {4,4,4,4}
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_5678]            ; {5,6,7,8}
    subps  xmm0, [vec_1234]            ; {4,4,4,4}
    addps  xmm0, xmm0                  ; {8,8,8,8}
    divps  xmm0, [vec_twos]            ; {4,4,4,4}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40800000 ; 13 4.0f
    ASSERT_EQ_MEM result+4,  0x40800000 ; 14 4.0f
    ASSERT_EQ_MEM result+8,  0x40800000 ; 15 4.0f
    ASSERT_EQ_MEM result+12, 0x40800000 ; 16 4.0f

; ---------------------------------------------------------------------------
; 5. XORPS (zero a register) + ADDSS (scalar)
; ---------------------------------------------------------------------------
    xorps  xmm0, xmm0                  ; zero
    addss  xmm0, [scalar_3]            ; xmm0[0] = 3.0
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40400000 ; 17 3.0f
    ASSERT_EQ_MEM result+4,  0x00000000 ; 18 upper lanes zero

; ---------------------------------------------------------------------------
; 6. MULSS: 3.0 * 10.0 = 30.0 = 0x41F00000
; ---------------------------------------------------------------------------
    mulss  xmm0, [scalar_10]           ; xmm0[0] = 30.0
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x41F00000 ; 19 30.0f

; ---------------------------------------------------------------------------
; 7. MOVSS load/store (scalar)
; ---------------------------------------------------------------------------
    mov    dword [result], 0x3F800000   ; 1.0f
    movss  xmm2, [result]              ; xmm2[0] = 1.0, rest cleared
    movaps [result], xmm2
    ASSERT_EQ_MEM result,    0x3F800000 ; 20 1.0f
    ASSERT_EQ_MEM result+4,  0x00000000 ; 21 upper cleared by MOVSS load

; ---------------------------------------------------------------------------
; 8. CVTSI2SS + CVTTSS2SI: int->float->int round-trip
; ---------------------------------------------------------------------------
    mov    eax, 42
    cvtsi2ss xmm3, eax                 ; xmm3[0] = 42.0f
    cvttss2si ebx, xmm3                ; ebx = 42
    ASSERT_EQ ebx, 42                   ; 22

; ---------------------------------------------------------------------------
; 9. COMISS sets EFLAGS: 3.0 vs 10.0 -> CF=1 (less)
; ---------------------------------------------------------------------------
    movss  xmm4, [scalar_3]            ; 3.0
    comiss xmm4, [scalar_10]           ; 3.0 < 10.0 -> CF=1
    ASSERT_FLAGS CF, CF                  ; 23

; ---------------------------------------------------------------------------
; 10. SHUFPS with immediate
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]            ; {1,2,3,4}
    shufps xmm0, xmm0, 0x1B           ; reverse: {4,3,2,1} (0b 00_01_10_11)
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40800000 ; 24 4.0f
    ASSERT_EQ_MEM result+4,  0x40400000 ; 25 3.0f
    ASSERT_EQ_MEM result+8,  0x40000000 ; 26 2.0f
    ASSERT_EQ_MEM result+12, 0x3F800000 ; 27 1.0f

; ---------------------------------------------------------------------------
; 11. MOVUPS (unaligned load/store)
; ---------------------------------------------------------------------------
    movups xmm5, [vec_ones]
    movups [result], xmm5
    ASSERT_EQ_MEM result,   0x3F800000  ; 28 1.0f
    ASSERT_EQ_MEM result+4, 0x3F800000  ; 29 1.0f

; ---------------------------------------------------------------------------
; 12. ANDPS identity
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]
    andps  xmm0, xmm0                  ; identity
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x3F800000 ; 30 1.0f (unchanged)

; ---------------------------------------------------------------------------
; 13. ORPS: 0 | vec = vec
; ---------------------------------------------------------------------------
    xorps  xmm0, xmm0                  ; zero
    orps   xmm0, [vec_1234]            ; {1,2,3,4}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x3F800000 ; 31 1.0f
    ASSERT_EQ_MEM result+12, 0x40800000 ; 32 4.0f

; ---------------------------------------------------------------------------
; 14. SQRTSS: sqrt(4.0) = 2.0 = 0x40000000
; ---------------------------------------------------------------------------
    movss  xmm0, [scalar_4]
    sqrtss xmm0, xmm0
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40000000 ; 33 2.0f

; ---------------------------------------------------------------------------
; 15. MINPS / MAXPS
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]            ; {1,2,3,4}
    movaps xmm1, [vec_twos]            ; {2,2,2,2}
    minps  xmm0, xmm1                  ; {1,2,2,2}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x3F800000 ; 34 min(1,2)=1.0f
    ASSERT_EQ_MEM result+4,  0x40000000 ; 35 min(2,2)=2.0f
    ASSERT_EQ_MEM result+8,  0x40000000 ; 36 min(3,2)=2.0f

    movaps xmm0, [vec_1234]            ; {1,2,3,4}
    maxps  xmm0, [vec_twos]            ; {2,2,3,4}
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40000000 ; 37 max(1,2)=2.0f
    ASSERT_EQ_MEM result+4,  0x40000000 ; 38 max(2,2)=2.0f
    ASSERT_EQ_MEM result+8,  0x40400000 ; 39 max(3,2)=3.0f

; ---------------------------------------------------------------------------
; 16. UNPCKLPS: interleave low halves
;     xmm0={1,2,3,4}, xmm1={5,6,7,8} -> {1,5,2,6}
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]
    movaps xmm1, [vec_5678]
    unpcklps xmm0, xmm1
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x3F800000 ; 40 1.0f
    ASSERT_EQ_MEM result+4,  0x40A00000 ; 41 5.0f
    ASSERT_EQ_MEM result+8,  0x40000000 ; 42 2.0f
    ASSERT_EQ_MEM result+12, 0x40C00000 ; 43 6.0f

; ---------------------------------------------------------------------------
; 17. UNPCKHPS: interleave high halves
;     xmm0={1,2,3,4}, xmm1={5,6,7,8} -> {3,7,4,8}
; ---------------------------------------------------------------------------
    movaps xmm0, [vec_1234]
    movaps xmm1, [vec_5678]
    unpckhps xmm0, xmm1
    movaps [result], xmm0
    ASSERT_EQ_MEM result,    0x40400000 ; 44 3.0f
    ASSERT_EQ_MEM result+4,  0x40E00000 ; 45 7.0f
    ASSERT_EQ_MEM result+8,  0x40800000 ; 46 4.0f
    ASSERT_EQ_MEM result+12, 0x41000000 ; 47 8.0f

; ---------------------------------------------------------------------------
; 18. LDMXCSR / STMXCSR round-trip
; ---------------------------------------------------------------------------
    stmxcsr [result]                    ; save current MXCSR
    mov     eax, [result]
    and     eax, 0x0000FFBF            ; mask out FZ, DAZ, exception bits
    or      eax, 0x00001F80            ; set default mask-all bits
    mov     [result+4], eax
    ldmxcsr [result+4]                 ; load modified
    stmxcsr [result+8]                 ; read back
    mov     ebx, [result+8]
    and     ebx, 0x00001F80
    ASSERT_EQ ebx, 0x00001F80           ; 48 mask bits preserved

; ============================== PASS =======================================
    PASS
