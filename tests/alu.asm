; ===========================================================================
; alu.asm — ALU operations, shifts, rotates, mul/div, conversions.
;
; Tests:  ADD SUB AND OR XOR CMP INC DEC NEG NOT
;         SHL SHR SAR ROL ROR   ADC SBB
;         MUL IMUL DIV IDIV   BSWAP CBW CWDE XCHG
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ============================= ADD =========================================
    mov  eax, 10
    mov  ecx, 20
    add  eax, ecx
    ASSERT_EQ eax, 30                       ; 1

    mov  eax, 0xFFFFFFFF
    add  eax, 1
    ASSERT_EQ eax, 0                        ; 2  overflow wrap

    mov  eax, 0x7FFFFFFF
    add  eax, 1
    ASSERT_EQ eax, 0x80000000               ; 3  signed overflow

; ADD with immediate
    mov  eax, 100
    add  eax, 50
    ASSERT_EQ eax, 150                      ; 4

; ADD memory operand
    mov  dword [0x4000], 100
    mov  eax, 50
    add  eax, [0x4000]
    ASSERT_EQ eax, 150                      ; 5

; ============================= SUB =========================================
    mov  eax, 30
    mov  ecx, 20
    sub  eax, ecx
    ASSERT_EQ eax, 10                       ; 6

    mov  eax, 0
    sub  eax, 1
    ASSERT_EQ eax, 0xFFFFFFFF               ; 7

; SUB memory operand
    mov  dword [0x4004], 25
    mov  eax, 100
    sub  eax, [0x4004]
    ASSERT_EQ eax, 75                       ; 8

; ============================= AND =========================================
    mov  eax, 0xFF00FF00
    mov  ecx, 0x0FF00FF0
    and  eax, ecx
    ASSERT_EQ eax, 0x0F000F00               ; 9

    mov  eax, 0x12345678
    and  eax, 0xFF
    ASSERT_EQ eax, 0x78                     ; 10

; ============================= OR ==========================================
    mov  eax, 0xFF00FF00
    mov  ecx, 0x00FF00FF
    or   eax, ecx
    ASSERT_EQ eax, 0xFFFFFFFF               ; 11

; ============================= XOR =========================================
    mov  eax, 0xAAAA5555
    mov  ecx, 0xFFFF0000
    xor  eax, ecx
    ASSERT_EQ eax, 0x55555555               ; 12

    xor  eax, eax
    ASSERT_EQ eax, 0                        ; 13

; ============================= INC / DEC ===================================
    mov  eax, 41
    inc  eax
    ASSERT_EQ eax, 42                       ; 14

    mov  eax, 0
    dec  eax
    ASSERT_EQ eax, 0xFFFFFFFF               ; 15

    mov  ecx, 100
    inc  ecx
    inc  ecx
    inc  ecx
    ASSERT_EQ ecx, 103                      ; 16

; ============================= NEG =========================================
    mov  eax, 42
    neg  eax
    ASSERT_EQ eax, 0xFFFFFFD6               ; 17  (-42 twos complement)

    mov  eax, 0
    neg  eax
    ASSERT_EQ eax, 0                        ; 18

; ============================= NOT =========================================
    mov  eax, 0
    not  eax
    ASSERT_EQ eax, 0xFFFFFFFF               ; 19

    mov  eax, 0xAAAAAAAA
    not  eax
    ASSERT_EQ eax, 0x55555555               ; 20

; ============================= SHL =========================================
    mov  eax, 1
    shl  eax, 4
    ASSERT_EQ eax, 16                       ; 21

    mov  eax, 1
    shl  eax, 31
    ASSERT_EQ eax, 0x80000000               ; 22

; SHL by CL
    mov  eax, 1
    mov  cl, 8
    shl  eax, cl
    ASSERT_EQ eax, 256                      ; 23

; ============================= SHR =========================================
    mov  eax, 0x80000000
    shr  eax, 31
    ASSERT_EQ eax, 1                        ; 24

    mov  eax, 256
    shr  eax, 4
    ASSERT_EQ eax, 16                       ; 25

; ============================= SAR =========================================
    mov  eax, 0x80000000
    sar  eax, 31
    ASSERT_EQ eax, 0xFFFFFFFF               ; 26  sign extended

    mov  eax, 0x40000000
    sar  eax, 1
    ASSERT_EQ eax, 0x20000000               ; 27  positive stays positive

; ============================= ROL / ROR ===================================
    mov  eax, 1
    rol  eax, 1
    ASSERT_EQ eax, 2                        ; 28

    mov  eax, 0x80000000
    rol  eax, 1
    ASSERT_EQ eax, 1                        ; 29  bit 31 wraps to bit 0

    mov  eax, 1
    ror  eax, 1
    ASSERT_EQ eax, 0x80000000               ; 30  bit 0 wraps to bit 31

    mov  eax, 0x80000000
    ror  eax, 1
    ASSERT_EQ eax, 0x40000000               ; 31

; ============================= ADC =========================================
; Set CF=1 via subtraction, then ADC
    mov  eax, 0
    sub  eax, 1                             ; EAX=FFFFFFFF, CF=1
    mov  eax, 10
    adc  eax, 20                            ; 10 + 20 + CF(1) = 31
    ASSERT_EQ eax, 31                       ; 32

; ADC with CF=0
    mov  eax, 10
    add  eax, 0                             ; clears CF
    adc  eax, 20                            ; 10 + 20 + 0 = 30
    ASSERT_EQ eax, 30                       ; 33

; ============================= SBB =========================================
    mov  eax, 0
    sub  eax, 1                             ; CF=1
    mov  eax, 30
    sbb  eax, 10                            ; 30 - 10 - CF(1) = 19
    ASSERT_EQ eax, 19                       ; 34

; ============================= MUL =========================================
; MUL r32: EDX:EAX = EAX * r32
    mov  eax, 100
    mov  ecx, 200
    mul  ecx
    ASSERT_EQ eax, 20000                    ; 35
    ASSERT_EQ edx, 0                        ; 36

; MUL with large values producing high 32 bits
    mov  eax, 0x10000
    mov  ecx, 0x10000
    mul  ecx
    ASSERT_EQ eax, 0                        ; 37  (0x100000000 low = 0)
    ASSERT_EQ edx, 1                        ; 38  (0x100000000 high = 1)

; ============================= IMUL (two-operand) ==========================
    mov  eax, 5
    mov  ecx, 10
    imul eax, ecx
    ASSERT_EQ eax, 50                       ; 39

    mov  eax, -10
    mov  ecx, 5
    imul eax, ecx
    ASSERT_EQ eax, 0xFFFFFFCE               ; 40  (-50)

; IMUL three-operand (imm)
    mov  ecx, 12
    imul eax, ecx, 7
    ASSERT_EQ eax, 84                       ; 41

; ============================= DIV =========================================
; DIV r32: EAX = EDX:EAX / r32, EDX = remainder
    xor  edx, edx
    mov  eax, 100
    mov  ecx, 7
    div  ecx
    ASSERT_EQ eax, 14                       ; 42  (100/7 = 14)
    ASSERT_EQ edx, 2                        ; 43  (100%7 = 2)

; ============================= IDIV ========================================
    mov  edx, 0xFFFFFFFF                    ; sign-extend -100 into EDX:EAX
    mov  eax, 0xFFFFFF9C                    ; -100
    mov  ecx, 7
    idiv ecx
    ASSERT_EQ eax, 0xFFFFFFF2               ; 44  (-14)
    ASSERT_EQ edx, 0xFFFFFFFE               ; 45  (-2)

; ============================= BSWAP =======================================
    mov  eax, 0x12345678
    bswap eax
    ASSERT_EQ eax, 0x78563412               ; 46

    mov  ebx, 0x01020304
    bswap ebx
    ASSERT_EQ ebx, 0x04030201               ; 47

; ============================= CBW / CWDE ==================================
    mov  eax, 0x00000080                    ; AL = 0x80 (negative byte)
    cbw                                     ; sign-extend AL → AX (AX=0xFF80)
    ASSERT_EQ eax, 0x0000FF80               ; 48

    mov  eax, 0x00008000                    ; AX = 0x8000 (negative word)
    cwde                                    ; sign-extend AX → EAX
    ASSERT_EQ eax, 0xFFFF8000               ; 49

    mov  eax, 0x0000007F                    ; AL = 0x7F (positive byte)
    cbw                                     ; AX = 0x007F
    ASSERT_EQ eax, 0x0000007F               ; 50

; ============================= XCHG ========================================
    mov  eax, 0x11111111
    mov  ecx, 0x22222222
    xchg eax, ecx
    ASSERT_EQ eax, 0x22222222               ; 51
    ASSERT_EQ ecx, 0x11111111               ; 52

; ============================= CMP + JCC ===================================
; CMP equal → JE taken
    mov  eax, 42
    cmp  eax, 42
    jne  .fail_cmp1
    jmp  .ok_cmp1
.fail_cmp1:
    mov  eax, 53
    hlt
.ok_cmp1:

; CMP less → JL taken
    mov  eax, 10
    cmp  eax, 20
    jge  .fail_cmp2
    jmp  .ok_cmp2
.fail_cmp2:
    mov  eax, 54
    hlt
.ok_cmp2:

; CMP greater → JG taken
    mov  eax, 20
    cmp  eax, 10
    jle  .fail_cmp3
    jmp  .ok_cmp3
.fail_cmp3:
    mov  eax, 55
    hlt
.ok_cmp3:

; CMP unsigned → JB / JA
    mov  eax, 5
    cmp  eax, 0xFFFFFFFF                    ; 5 < 0xFFFFFFFF unsigned
    jae  .fail_cmp4
    jmp  .ok_cmp4
.fail_cmp4:
    mov  eax, 56
    hlt
.ok_cmp4:

; ============================= TEST ========================================
    mov  eax, 0xFF
    test eax, 0x01
    jz   .fail_test1
    jmp  .ok_test1
.fail_test1:
    mov  eax, 57
    hlt
.ok_test1:

    mov  eax, 0xFE
    test eax, 0x01
    jnz  .fail_test2
    jmp  .ok_test2
.fail_test2:
    mov  eax, 58
    hlt
.ok_test2:

; ============================== PASS =======================================
    PASS
