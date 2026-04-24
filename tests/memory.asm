; ===========================================================================
; memory.asm — Memory operations, addressing modes, PUSH/POP, LEA.
;
; Tests:  MOV r,[m]  MOV [m],r  MOV [m],imm  LEA  MOVZX  MOVSX
;         PUSH r32  POP r32  PUSH imm  LEAVE
;         Various addressing modes  (base, base+disp, base+index*scale+disp)
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ============================= MOV reg ↔ mem ===============================
    mov  eax, 0xDEADBEEF
    mov  [0x4000], eax
    mov  ebx, [0x4000]
    ASSERT_EQ ebx, 0xDEADBEEF               ; 1

; MOV [mem], imm32
    mov  dword [0x4004], 0x12345678
    mov  ecx, [0x4004]
    ASSERT_EQ ecx, 0x12345678                ; 2

; MOV [mem], imm8 (byte store)
    mov  byte [0x4008], 0xAB
    movzx eax, byte [0x4008]
    ASSERT_EQ eax, 0xAB                      ; 3

; MOV [mem], imm16 (word store)
    mov  word [0x400A], 0xBEEF
    movzx eax, word [0x400A]
    ASSERT_EQ eax, 0xBEEF                    ; 4

; Multiple stores to adjacent locations
    mov  dword [0x4100], 1
    mov  dword [0x4104], 2
    mov  dword [0x4108], 3
    mov  eax, [0x4100]
    mov  ebx, [0x4104]
    mov  ecx, [0x4108]
    ASSERT_EQ eax, 1                         ; 5
    ASSERT_EQ ebx, 2                         ; 6
    ASSERT_EQ ecx, 3                         ; 7

; ============================= LEA =========================================
; LEA base only
    mov  ecx, 1000
    lea  eax, [ecx]
    ASSERT_EQ eax, 1000                      ; 8

; LEA base + disp
    mov  ecx, 1000
    lea  eax, [ecx + 234]
    ASSERT_EQ eax, 1234                      ; 9

; LEA base + index
    mov  eax, 100
    mov  ecx, 200
    lea  edx, [eax + ecx]
    ASSERT_EQ edx, 300                       ; 10

; LEA base + index*2
    mov  eax, 100
    mov  ecx, 200
    lea  edx, [eax + ecx*2]
    ASSERT_EQ edx, 500                       ; 11

; LEA base + index*4 + disp
    mov  eax, 100
    mov  ecx, 200
    lea  edx, [eax + ecx*4 + 10]
    ASSERT_EQ edx, 910                       ; 12

; LEA index*8 (no base)
    mov  ecx, 200
    lea  edx, [ecx*8]
    ASSERT_EQ edx, 1600                      ; 13

; LEA disp only
    lea  eax, [0x5000]
    ASSERT_EQ eax, 0x5000                    ; 14

; LEA with EBP base (special encoding)
    mov  ebp, 500
    lea  eax, [ebp + 100]
    ASSERT_EQ eax, 600                       ; 15

; LEA with ESP base (SIB encoding)
    ; Save ESP, set a known value, test LEA, restore
    mov  dword [0x4200], esp
    mov  esp, 2000
    lea  eax, [esp + 48]
    ASSERT_EQ eax, 2048                      ; 16
    mov  esp, [0x4200]                       ; restore ESP

; ============================= MOVZX =======================================
    mov  byte [0x4300], 0x80
    movzx eax, byte [0x4300]
    ASSERT_EQ eax, 0x00000080                ; 17

    mov  word [0x4302], 0x8000
    movzx eax, word [0x4302]
    ASSERT_EQ eax, 0x00008000                ; 18

; Zero-extend positive byte
    mov  byte [0x4304], 0x7F
    movzx eax, byte [0x4304]
    ASSERT_EQ eax, 0x0000007F                ; 19

; ============================= MOVSX =======================================
    mov  byte [0x4306], 0x80
    movsx eax, byte [0x4306]
    ASSERT_EQ eax, 0xFFFFFF80                ; 20  sign-extended

    mov  word [0x4308], 0x8000
    movsx eax, word [0x4308]
    ASSERT_EQ eax, 0xFFFF8000                ; 21  sign-extended

; Sign-extend positive byte
    mov  byte [0x430A], 0x7F
    movsx eax, byte [0x430A]
    ASSERT_EQ eax, 0x0000007F                ; 22  positive unchanged

; ============================= PUSH / POP ==================================
; PUSH reg, POP reg
    mov  eax, 0xCAFEBABE
    push eax
    pop  ebx
    ASSERT_EQ ebx, 0xCAFEBABE                ; 23

; PUSH imm32
    push dword 0x12345678
    pop  ecx
    ASSERT_EQ ecx, 0x12345678                ; 24

; Stack ordering (LIFO)
    push dword 1
    push dword 2
    push dword 3
    pop  eax                                  ; 3
    pop  ebx                                  ; 2
    pop  ecx                                  ; 1
    ASSERT_EQ eax, 3                          ; 25
    ASSERT_EQ ebx, 2                          ; 26
    ASSERT_EQ ecx, 1                          ; 27

; PUSH/POP preserves all GP registers
    mov  eax, 0x11
    mov  ecx, 0x22
    mov  edx, 0x33
    mov  ebx, 0x44
    push eax
    push ecx
    push edx
    push ebx
    ; clobber all
    xor  eax, eax
    xor  ecx, ecx
    xor  edx, edx
    xor  ebx, ebx
    pop  ebx
    pop  edx
    pop  ecx
    pop  eax
    ASSERT_EQ eax, 0x11                       ; 28
    ASSERT_EQ ecx, 0x22                       ; 29
    ASSERT_EQ edx, 0x33                       ; 30
    ASSERT_EQ ebx, 0x44                       ; 31

; ============================= LEAVE =======================================
; LEAVE = MOV ESP, EBP ; POP EBP
; Set up a fake frame:  push known EBP, set EBP=ESP
    mov  dword [0x4400], esp                  ; save real ESP
    push dword 0xAAAAAAAA                     ; simulate saved EBP
    mov  ebp, esp                             ; EBP points to saved EBP
    sub  esp, 64                              ; allocate local space
    leave                                     ; ESP=EBP, POP EBP → EBP=0xAAAAAAAA
    ASSERT_EQ ebp, 0xAAAAAAAA                 ; 32
    mov  esp, [0x4400]                        ; restore real ESP

; ============================= Addressing mode stress ======================
; [base + index*scale + disp8]
    mov  ebx, 0x4500
    mov  ecx, 4
    mov  dword [0x4510], 0x99887766           ; 0x4500 + 4*4 + 0 = 0x4510
    mov  eax, [ebx + ecx*4]
    ASSERT_EQ eax, 0x99887766                 ; 33

; [base + index*scale + disp32]
    mov  ebx, 0x4000
    mov  ecx, 2
    mov  dword [0x4508], 0xAABBCCDD           ; 0x4000 + 2*4 + 0x500 = 0x4508
    mov  eax, [ebx + ecx*4 + 0x500]
    ASSERT_EQ eax, 0xAABBCCDD                 ; 34

; ============================= EFLAGS across memory dispatch ===============
; CMP sets ZF, memory load must not clobber it
    mov  eax, 42
    mov  ecx, 42
    mov  dword [0x5000], 99
    cmp  eax, ecx                             ; ZF=1
    mov  edx, [0x5000]                        ; must preserve ZF
    jne  .fail_flags1
    jmp  .ok_flags1
.fail_flags1:
    mov  eax, 35
    hlt
.ok_flags1:

; CMP sets CF, memory store must not clobber it
    mov  eax, 5
    cmp  eax, 10                              ; 5 < 10 → CF=1
    mov  [0x5004], eax                        ; must preserve CF
    jnb  .fail_flags2
    jmp  .ok_flags2
.fail_flags2:
    mov  eax, 36
    hlt
.ok_flags2:

; TEST with memory operand
    mov  dword [0x5008], 0xFF00
    test dword [0x5008], 0x00FF
    jnz  .fail_test1
    jmp  .ok_test1
.fail_test1:
    mov  eax, 37
    hlt
.ok_test1:

    mov  dword [0x500C], 0xFF00
    test dword [0x500C], 0x0F00
    jz   .fail_test2
    jmp  .ok_test2
.fail_test2:
    mov  eax, 38
    hlt
.ok_test2:

; ADD to memory, read back
    mov  dword [0x5010], 100
    add  dword [0x5010], 50
    ASSERT_EQ_MEM 0x5010, 150                ; 39

; SUB from memory
    mov  dword [0x5014], 100
    sub  dword [0x5014], 30
    ASSERT_EQ_MEM 0x5014, 70                 ; 40

; INC/DEC memory
    mov  dword [0x5018], 10
    inc  dword [0x5018]
    ASSERT_EQ_MEM 0x5018, 11                 ; 41

    dec  dword [0x5018]
    ASSERT_EQ_MEM 0x5018, 10                 ; 42

; ============================== PASS =======================================
    PASS
