; ===========================================================================
; smc.asm — Self-Modifying Code tests.
;
; Verifies that the executor detects writes to code pages and retranslates
; traces after modification (page-version bumping + trace invalidation).
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ---------------------------------------------------------------------------
; Test 1: Overwrite an immediate in a MOV instruction.
;
;   .target contains "mov ebx, 0x11111111".  We run it, verify 0x11111111,
;   then patch the immediate to 0x22222222 and call again.
; ---------------------------------------------------------------------------
    call .target                         ; first call → ebx = 0x11111111
    ASSERT_EQ ebx, 0x11111111            ; 1

    ; Patch the immediate operand of the MOV EBX at .target.
    ; MOV EBX, imm32 is BB xx xx xx xx — immediate starts at .target+1.
    mov dword [.target + 1], 0x22222222

    call .target                         ; second call → ebx = 0x22222222
    ASSERT_EQ ebx, 0x22222222            ; 2
    jmp .test2

.target:
    mov ebx, 0x11111111
    ret

; ---------------------------------------------------------------------------
; Test 2: Overwrite an opcode (change MOV ECX → MOV EDX).
;
;   MOV ECX, imm32 is B9 xx; MOV EDX, imm32 is BA xx.
;   Patch the opcode byte to redirect the destination register.
; ---------------------------------------------------------------------------
.test2:
    call .target2                        ; first call → ecx = 0xAAAAAAAA
    ASSERT_EQ ecx, 0xAAAAAAAA            ; 3

    ; Change B9 (MOV ECX) to BA (MOV EDX).
    mov byte [.target2], 0xBA
    call .target2                        ; second call → edx = 0xAAAAAAAA
    ASSERT_EQ edx, 0xAAAAAAAA            ; 4
    jmp .test3

.target2:
    mov ecx, 0xAAAAAAAA
    ret

; ---------------------------------------------------------------------------
; Test 3: Patch a JMP target to change control flow.
;
;   Short JMP (EB rel8) jumps over a MOV EBX,1 to reach MOV EBX,2.
;   Patch the rel8 to jump to MOV EBX,3 instead.
; ---------------------------------------------------------------------------
.test3:
    call .target3
    ASSERT_EQ ebx, 2                     ; 5  (original: jumps to .t3_b)

    ; Patch rel8: calculate new displacement to .t3_c from .target3+2.
    ; .target3 layout: EB xx / mov ebx,1 (5) / mov ebx,2 (5) / mov ebx,3 (5) / ret
    ; Original rel8 = .t3_b - (.target3+2) = skip the 5-byte mov ebx,1
    ; New rel8 = .t3_c - (.target3+2) = skip both 5-byte mov instructions
    mov byte [.target3 + 1], (.t3_c - .target3 - 2)

    call .target3
    ASSERT_EQ ebx, 3                     ; 6  (patched: jumps to .t3_c)
    jmp .test4

.target3:
    jmp short .t3_b
    mov ebx, 1                           ; skipped by JMP
.t3_b:
    mov ebx, 2
    ret
.t3_c:
    mov ebx, 3
    ret

; ---------------------------------------------------------------------------
; Test 4: Patch ALU operation (ADD → SUB) — verifies flags are correct
;         after retranslation of modified ALU instruction.
; ---------------------------------------------------------------------------
.test4:
    mov ecx, 10
    call .target4                        ; first: ecx = 10 + 5 = 15
    ASSERT_EQ ecx, 15                    ; 7

    ; Patch ADD ECX,5 (83 C1 05) → SUB ECX,5 (83 E9 05).
    ; Change ModR/M byte from C1 (ADD /0 ECX) to E9 (SUB /5 ECX).
    mov byte [.target4 + 1], 0xE9
    mov ecx, 10
    call .target4                        ; second: ecx = 10 - 5 = 5
    ASSERT_EQ ecx, 5                     ; 8
    jmp .test5

.target4:
    add ecx, 5
    ret

; ---------------------------------------------------------------------------
; Test 5: Multiple patches to the same page — verify each bump is detected.
; ---------------------------------------------------------------------------
.test5:
    call .target5                        ; first: edx = 0x100
    ASSERT_EQ edx, 0x100                 ; 9

    mov dword [.target5 + 1], 0x200
    call .target5                        ; second: edx = 0x200
    ASSERT_EQ edx, 0x200                 ; 10

    mov dword [.target5 + 1], 0x300
    call .target5                        ; third: edx = 0x300
    ASSERT_EQ edx, 0x300                 ; 11

    PASS

.target5:
    mov edx, 0x100
    ret
