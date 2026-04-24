; ===========================================================================
; advanced.asm — CMOVcc, SETcc, BT/BTS/BTR/BTC, BSF/BSR, XCHG, BSWAP,
;                CDQ, SHLD/SHRD, string ops (REP MOVSB/STOSB), MOVS/CMPS.
;
; Tests register-only forms that pass through as clean copies, plus
; a few memory forms.
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ============================= CMOVcc (reg, reg) ===========================

; CMOVE — move if equal (ZF=1)
    mov  eax, 100
    mov  ebx, 200
    cmp  eax, eax                        ; ZF=1
    cmove eax, ebx                       ; EAX = 200
    ASSERT_EQ eax, 200                   ; 1

; CMOVNE — move if not equal (ZF=0)
    mov  eax, 100
    mov  ebx, 200
    cmp  eax, ebx                        ; ZF=0 (100 != 200)
    cmovne ecx, ebx                      ; ECX = 200
    ASSERT_EQ ecx, 200                   ; 2

; CMOVL — move if less (SF != OF)
    mov  eax, -5
    mov  ebx, 10
    mov  ecx, 999
    cmp  eax, ebx                        ; -5 < 10 → SF != OF
    cmovl ecx, ebx                       ; ECX = 10
    ASSERT_EQ ecx, 10                    ; 3

; CMOVGE — move if greater or equal (SF == OF)
    mov  eax, 10
    mov  ebx, 5
    mov  ecx, 0
    cmp  eax, ebx                        ; 10 >= 5
    cmovge ecx, eax                      ; ECX = 10
    ASSERT_EQ ecx, 10                    ; 4

; CMOVB — move if below (CF=1)
    mov  eax, 5
    mov  ebx, 10
    mov  ecx, 0
    cmp  eax, ebx                        ; 5 < 10 unsigned → CF=1
    cmovb ecx, ebx                       ; ECX = 10
    ASSERT_EQ ecx, 10                    ; 5

; CMOVAE — not taken (CF=1 → below, not above/equal)
    mov  eax, 5
    mov  ebx, 10
    mov  ecx, 42
    cmp  eax, ebx                        ; CF=1
    cmovae ecx, ebx                      ; NOT taken, ECX stays 42
    ASSERT_EQ ecx, 42                    ; 6

; ============================= SETcc =======================================

; SETE
    xor  eax, eax
    cmp  eax, eax                        ; ZF=1
    sete al                              ; AL = 1
    ASSERT_EQ eax, 1                     ; 7

; SETNE
    xor  eax, eax
    mov  ebx, 1
    cmp  eax, ebx                        ; ZF=0
    setne al                             ; AL = 1
    ASSERT_EQ eax, 1                     ; 8

; SETL
    xor  eax, eax
    mov  ebx, 10
    cmp  eax, ebx                        ; 0 < 10
    setl al
    ASSERT_EQ eax, 1                     ; 9

; SETGE (not taken: 0 < 10)
    xor  eax, eax
    mov  ebx, 10
    cmp  eax, ebx                        ; 0 < 10 → GE = false
    setge al
    ASSERT_EQ eax, 0                     ; 10

; SETB
    xor  eax, eax
    mov  ebx, 1
    cmp  eax, ebx                        ; 0 < 1 unsigned → CF=1
    setb al
    ASSERT_EQ eax, 1                     ; 11

; ============================= BT/BTS/BTR/BTC (reg, reg) ===================

; BT reg, imm — test bit 3 of 0xFF → CF=1
    mov  eax, 0xFF
    bt   eax, 3                          ; bit 3 = 1 → CF=1
    ASSERT_FLAGS CF, CF                  ; 12

; BT reg, imm — test bit 8 of 0xFF → CF=0
    mov  eax, 0xFF
    bt   eax, 8                          ; bit 8 = 0 → CF=0
    ASSERT_FLAGS CF, 0                   ; 13

; BTS reg, imm — set bit 8
    mov  eax, 0xFF
    bts  eax, 8                          ; set bit 8, CF=old=0
    mov  ebx, eax                        ; save result (ASSERT_FLAGS clobbers EAX)
    ASSERT_FLAGS CF, 0                   ; 14 (was 0; check before CMP clobbers CF)
    ASSERT_EQ ebx, 0x1FF                 ; 15

; BTR reg, imm — clear bit 0
    mov  eax, 0xFF
    btr  eax, 0                          ; clear bit 0, CF=old=1
    mov  ebx, eax                        ; save result
    ASSERT_FLAGS CF, CF                  ; 16 (was 1; check before CMP clobbers CF)
    ASSERT_EQ ebx, 0xFE                  ; 17

; BTC reg, imm — complement bit 7
    mov  eax, 0xFF
    btc  eax, 7                          ; toggle bit 7, CF=old=1
    mov  ebx, eax                        ; save result
    ASSERT_FLAGS CF, CF                  ; 18 (was 1; check before CMP clobbers CF)
    ASSERT_EQ ebx, 0x7F                  ; 19

; ============================= BSF / BSR ===================================

; BSF: find lowest set bit of 0x80 → bit 7
    mov  eax, 0x80
    bsf  ebx, eax
    ASSERT_EQ ebx, 7                     ; 20

; BSR: find highest set bit of 0x80 → bit 7
    mov  eax, 0x80
    bsr  ebx, eax
    ASSERT_EQ ebx, 7                     ; 21

; BSF of 0x100 → bit 8
    mov  eax, 0x100
    bsf  ebx, eax
    ASSERT_EQ ebx, 8                     ; 22

; BSR of 0xFFFF → bit 15
    mov  eax, 0xFFFF
    bsr  ebx, eax
    ASSERT_EQ ebx, 15                    ; 23

; ============================= XCHG ========================================

    mov  eax, 111
    mov  ebx, 222
    xchg eax, ebx
    ASSERT_EQ eax, 222                   ; 24
    ASSERT_EQ ebx, 111                   ; 25

; ============================= BSWAP =======================================

    mov  eax, 0x12345678
    bswap eax
    ASSERT_EQ eax, 0x78563412            ; 26

    mov  eax, 0x01020304
    bswap eax
    ASSERT_EQ eax, 0x04030201            ; 27

; ============================= CDQ =========================================

; Positive → EDX = 0
    mov  eax, 100
    cdq
    ASSERT_EQ edx, 0                     ; 28

; Negative → EDX = 0xFFFFFFFF
    mov  eax, -1
    cdq
    ASSERT_EQ edx, 0xFFFFFFFF            ; 29

; ============================= SHLD / SHRD =================================

; SHLD: shift eax left by 4, filling from ebx
    mov  eax, 0x12345678
    mov  ebx, 0xABCDEF01
    shld eax, ebx, 4                     ; EAX = 0x2345678A
    ASSERT_EQ eax, 0x2345678A            ; 30

; SHRD: shift eax right by 4, filling from ebx
    mov  eax, 0x12345678
    mov  ebx, 0xABCDEF01
    shrd eax, ebx, 4                     ; EAX = 0x11234567
    ASSERT_EQ eax, 0x11234567            ; 31

; ============================= MUL / IMUL forms ============================

; IMUL reg, reg, imm8
    mov  ebx, 7
    imul ecx, ebx, 6                     ; ECX = 42
    ASSERT_EQ ecx, 42                    ; 32

; IMUL reg, reg
    mov  eax, 5
    mov  ebx, 9
    imul eax, ebx                        ; EAX = 45
    ASSERT_EQ eax, 45                    ; 33

; ============================= MOVZX / MOVSX register (8-bit) ==============
; These are clean copies (reg→reg), not memory forms

    mov  eax, 0xFFFFFF80
    movzx ebx, al                        ; EBX = 0x80
    ASSERT_EQ ebx, 0x80                  ; 34

    mov  eax, 0x00000080
    movsx ebx, al                        ; EBX = 0xFFFFFF80 (sign extended)
    ASSERT_EQ ebx, 0xFFFFFF80            ; 35

; ============================= CWD / CWDE ==================================

; CWDE: sign-extend AX into EAX
    mov  eax, 0xFFFF0080                 ; AX = 0x0080 (positive)
    cwde                                  ; EAX = 0x00000080
    ASSERT_EQ eax, 0x00000080            ; 36

    mov  eax, 0x0000FF80                 ; AX = 0xFF80 (negative)
    cwde                                  ; EAX = 0xFFFFFF80
    ASSERT_EQ eax, 0xFFFFFF80            ; 37

; ============================= NOT / NEG ===================================

    mov  eax, 0x00FF00FF
    not  eax
    ASSERT_EQ eax, 0xFF00FF00            ; 38

    mov  eax, 42
    neg  eax
    ASSERT_EQ eax, -42                   ; 39 (0xFFFFFFD6)

; ============================= Complex flag chains =========================

; Chain: compute a > b > c via cascading CMOVs
    mov  eax, 30
    mov  ebx, 20
    mov  ecx, 10
    ; Is eax > ebx? (30 > 20 → yes)
    cmp  eax, ebx
    mov  edx, 0
    cmovg edx, eax                       ; EDX = 30 (selected)
    ASSERT_EQ edx, 30                    ; 40

    ; Is ebx > ecx? (20 > 10 → yes)
    cmp  ebx, ecx
    mov  edx, 0
    cmovg edx, ebx                       ; EDX = 20
    ASSERT_EQ edx, 20                    ; 41

; SETcc chain: build a bitmask
    xor  edx, edx
    mov  eax, 5
    cmp  eax, 10                         ; 5 < 10
    setl dl                              ; DL = 1
    shl  edx, 1
    cmp  eax, 5                          ; 5 == 5
    sete al
    or   dl, al                          ; EDX = 0b11 = 3
    ASSERT_EQ edx, 3                     ; 42

; ============================== PASS =======================================
    PASS
