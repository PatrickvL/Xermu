; esp_lea.asm — Test LEA with ESP base (guest ESP in ctx, not host RSP)
%include "harness.inc"

    ; =========================================================================
    ; Test 1: LEA EBP, [ESP+0x10]  — the exact pattern from MSVC __SEH_prolog
    ; =========================================================================
    mov  esp, 0x70000           ; set guest ESP to a known value
    lea  ebp, [esp+0x10]       ; EBP should be 0x70010
    ASSERT_EQ ebp, 0x70010

    ; =========================================================================
    ; Test 2: LEA EAX, [ESP+0x20]
    ; =========================================================================
    mov  esp, 0x60000
    lea  eax, [esp+0x20]
    ASSERT_EQ eax, 0x60020

    ; =========================================================================
    ; Test 3: LEA ECX, [ESP-4]
    ; =========================================================================
    mov  esp, 0x50000
    lea  ecx, [esp-4]
    ASSERT_EQ ecx, 0x4FFFC

    ; =========================================================================
    ; Test 4: LEA EBP, [ESP] (no displacement)
    ; =========================================================================
    mov  esp, 0x40000
    lea  ebp, [esp]
    ASSERT_EQ ebp, 0x40000

    ; =========================================================================
    ; Test 5: MOV EBP, ESP  (direct register copy from ESP)
    ; =========================================================================
    mov  esp, 0x30000
    mov  ebp, esp
    ASSERT_EQ ebp, 0x30000

    ; =========================================================================
    ; Test 6: PUSH/LEA sequence (simulating __SEH_prolog)
    ; Caller pushed ret addr, args. Now inside prolog:
    ;   PUSH handler; PUSH old_exc; MOV EAX, [ESP+0x10]; LEA EBP, [ESP+0x10]
    ; =========================================================================
    mov  esp, 0x70100           ; pretend caller left ESP here
    push dword 0xAAAAAAAA       ; simulate PUSH handler
    push dword 0xBBBBBBBB       ; simulate PUSH old_exc_list
    ; Now ESP = 0x70100 - 8 = 0x700F8
    lea  ebp, [esp+0x10]       ; EBP = 0x700F8 + 0x10 = 0x70108
    ASSERT_EQ ebp, 0x70108

    ; =========================================================================
    ; Test 7: SUB ESP after LEA (allocate locals)
    ; =========================================================================
    mov  eax, 0x180
    sub  esp, eax              ; ESP = 0x700F8 - 0x180 = 0x6FF78
    ; EBP should still be 0x70108
    ASSERT_EQ ebp, 0x70108
    ; Write to [EBP-0x18] to verify addressing works
    mov  dword [ebp-0x18], 0xDEADBEEF
    mov  eax, [ebp-0x18]
    ASSERT_EQ eax, 0xDEADBEEF

    ; =========================================================================
    ; Test 8: LEAVE instruction (MOV ESP, EBP; POP EBP)
    ; =========================================================================
    ; Set up frame: EBP=0x70100, [0x70100]=old_ebp=0x12345678
    mov  esp, 0x70100
    push dword 0x12345678       ; [0x700FC] = old EBP
    mov  ebp, esp               ; EBP = 0x700FC
    sub  esp, 0x20              ; allocate some locals
    leave                       ; ESP = EBP = 0x700FC, POP EBP → EBP = 0x12345678, ESP = 0x70100
    ASSERT_EQ ebp, 0x12345678
    ASSERT_EQ esp, 0x70100

    ; Restore stack for clean exit
    mov  esp, 0x80000
    PASS
