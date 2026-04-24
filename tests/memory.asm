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

; NOTE: LEA with ESP base is NOT tested here because guest ESP is not mapped
; to host RSP.  The JIT manages ESP through ctx->gp[GP_ESP] in dedicated
; PUSH/POP/CALL/RET/LEAVE handlers.

; ============================= MOVZX =======================================
    mov  byte [0x4300], 0x80
    movzx eax, byte [0x4300]
    ASSERT_EQ eax, 0x00000080                ; 16

    mov  word [0x4302], 0x8000
    movzx eax, word [0x4302]
    ASSERT_EQ eax, 0x00008000                ; 17

; Zero-extend positive byte
    mov  byte [0x4304], 0x7F
    movzx eax, byte [0x4304]
    ASSERT_EQ eax, 0x0000007F                ; 18

; ============================= MOVSX =======================================
    mov  byte [0x4306], 0x80
    movsx eax, byte [0x4306]
    ASSERT_EQ eax, 0xFFFFFF80                ; 19  sign-extended

    mov  word [0x4308], 0x8000
    movsx eax, word [0x4308]
    ASSERT_EQ eax, 0xFFFF8000                ; 20  sign-extended

; Sign-extend positive byte
    mov  byte [0x430A], 0x7F
    movsx eax, byte [0x430A]
    ASSERT_EQ eax, 0x0000007F                ; 21  positive unchanged

; ============================= PUSH / POP ==================================
; PUSH reg, POP reg
    mov  eax, 0xCAFEBABE
    push eax
    pop  ebx
    ASSERT_EQ ebx, 0xCAFEBABE                ; 22

; PUSH imm32
    push dword 0x12345678
    pop  ecx
    ASSERT_EQ ecx, 0x12345678                ; 23

; Stack ordering (LIFO)
    push dword 1
    push dword 2
    push dword 3
    pop  eax                                  ; 3
    pop  ebx                                  ; 2
    pop  ecx                                  ; 1
    ASSERT_EQ eax, 3                          ; 24
    ASSERT_EQ ebx, 2                          ; 25
    ASSERT_EQ ecx, 1                          ; 26

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
    ASSERT_EQ eax, 0x11                       ; 27
    ASSERT_EQ ecx, 0x22                       ; 28
    ASSERT_EQ edx, 0x33                       ; 29
    ASSERT_EQ ebx, 0x44                       ; 30

; ============================= LEAVE =======================================
; LEAVE = MOV ESP, EBP ; POP EBP
; The handler operates on ctx->gp[GP_ESP/GP_EBP], not host registers.
; Set up EBP pointing to a valid area, store a known value there.
    mov  ebp, 0x7F000                        ; guest EBP → valid memory
    mov  dword [0x7F000], 0xAAAAAAAA         ; "saved EBP" to pop
    leave                                     ; ESP=EBP(0x7F000), POP EBP→0xAAAAAAAA
    ASSERT_EQ ebp, 0xAAAAAAAA                 ; 31
; NOTE: ESP is now 0x7F004 — stack operations after this point would use
; that address.  We avoid PUSH/POP until PASS (which only does XOR+HLT).

; ============================= Addressing mode stress ======================
; [base + index*scale + disp8]
    mov  ebx, 0x4500
    mov  ecx, 4
    mov  dword [0x4510], 0x99887766           ; 0x4500 + 4*4 + 0 = 0x4510
    mov  eax, [ebx + ecx*4]
    ASSERT_EQ eax, 0x99887766                 ; 32

; [base + index*scale + disp32]
    mov  ebx, 0x4000
    mov  ecx, 2
    mov  dword [0x4508], 0xAABBCCDD           ; 0x4000 + 2*4 + 0x500 = 0x4508
    mov  eax, [ebx + ecx*4 + 0x500]
    ASSERT_EQ eax, 0xAABBCCDD                 ; 33

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
    mov  eax, 34
    hlt
.ok_flags1:

; CMP sets CF, memory store must not clobber it
    mov  eax, 5
    cmp  eax, 10                              ; 5 < 10 → CF=1
    mov  [0x5004], eax                        ; must preserve CF
    jnb  .fail_flags2
    jmp  .ok_flags2
.fail_flags2:
    mov  eax, 35
    hlt
.ok_flags2:

; TEST with memory operand
    mov  dword [0x5008], 0xFF00
    test dword [0x5008], 0x00FF
    jnz  .fail_test1
    jmp  .ok_test1
.fail_test1:
    mov  eax, 36
    hlt
.ok_test1:

    mov  dword [0x500C], 0xFF00
    test dword [0x500C], 0x0F00
    jz   .fail_test2
    jmp  .ok_test2
.fail_test2:
    mov  eax, 37
    hlt
.ok_test2:

; ADD to memory, read back
    mov  dword [0x5010], 100
    add  dword [0x5010], 50
    ASSERT_EQ_MEM 0x5010, 150                ; 38

; SUB from memory
    mov  dword [0x5014], 100
    sub  dword [0x5014], 30
    ASSERT_EQ_MEM 0x5014, 70                 ; 39

; INC/DEC memory
    mov  dword [0x5018], 10
    inc  dword [0x5018]
    ASSERT_EQ_MEM 0x5018, 11                 ; 40

    dec  dword [0x5018]
    ASSERT_EQ_MEM 0x5018, 10                 ; 41

; ========================= 8-bit register MOV [mem] ========================
; MOV [mem], r8 (byte store from register)
    mov  dword [0x5020], 0                   ; clear target
    mov  al, 0xAB
    mov  [0x5020], al
    movzx ecx, byte [0x5020]
    ASSERT_EQ ecx, 0xAB                      ; 42

; MOV r8, [mem] (byte load to register)
    mov  byte [0x5024], 0x7F
    mov  cl, [0x5024]
    movzx eax, cl
    ASSERT_EQ eax, 0x7F                      ; 43
    mov  eax, 0

; MOV [reg+disp], r8
    mov  ebx, 0x5020
    mov  dl, 0xCD
    mov  [ebx+8], dl
    movzx ecx, byte [0x5028]
    ASSERT_EQ ecx, 0xCD                      ; 44

; MOV r8, [reg+disp]
    mov  byte [0x502C], 0xEF
    mov  bl, [0x502C]
    movzx eax, bl
    ASSERT_EQ eax, 0xEF                      ; 45
    mov  eax, 0

; ========================= 16-bit register MOV [mem] =======================
; MOV [mem], r16 (word store from register)
    mov  dword [0x5030], 0                   ; clear
    mov  ax, 0xBEEF
    mov  [0x5030], ax
    movzx ecx, word [0x5030]
    ASSERT_EQ ecx, 0xBEEF                    ; 46
    mov  eax, 0

; MOV r16, [mem] (word load to register)
    mov  word [0x5034], 0x1234
    mov  cx, [0x5034]
    movzx eax, cx
    ASSERT_EQ eax, 0x1234                    ; 47
    mov  eax, 0

; MOV [reg+disp], r16
    mov  ebx, 0x5030
    mov  dx, 0xFACE
    mov  [ebx+8], dx
    movzx ecx, word [0x5038]
    ASSERT_EQ ecx, 0xFACE                    ; 48

; MOV r16, [reg+disp]
    mov  word [0x503C], 0x5678
    mov  ebx, 0x5030
    mov  si, [ebx+0xC]
    movzx eax, si
    ASSERT_EQ eax, 0x5678                    ; 49
    mov  eax, 0

; 8-bit MOV preserves upper bytes of register
    mov  eax, 0xFF00FF00
    mov  al, 0x42
    ASSERT_EQ eax, 0xFF00FF42                ; 50

; 16-bit MOV preserves upper word of register
    mov  ecx, 0xDEAD0000
    mov  cx, 0xBEEF
    ASSERT_EQ ecx, 0xDEADBEEF               ; 51

; ========================= PUSHAD / POPAD ==================================
; PUSHAD saves all 8 GP regs; POPAD restores them (skipping ESP slot).
    mov  eax, 0x11111111
    mov  ecx, 0x22222222
    mov  edx, 0x33333333
    mov  ebx, 0x44444444
    mov  ebp, 0x66666666
    mov  esi, 0x77777777
    mov  edi, 0x88888888
    pushad
    ; Clobber all registers
    xor  eax, eax
    xor  ecx, ecx
    xor  edx, edx
    xor  ebx, ebx
    xor  ebp, ebp
    xor  esi, esi
    xor  edi, edi
    popad
    ASSERT_EQ eax, 0x11111111                ; 52
    ASSERT_EQ ecx, 0x22222222                ; 53
    ASSERT_EQ edx, 0x33333333                ; 54
    ASSERT_EQ ebx, 0x44444444                ; 55
    ASSERT_EQ ebp, 0x66666666                ; 56
    ASSERT_EQ esi, 0x77777777                ; 57
    ASSERT_EQ edi, 0x88888888                ; 58
    mov  eax, 0                              ; reset for next test

; ============================== PASS =======================================
    PASS
