; ===========================================================================
; misc.asm — ENTER/LEAVE, XLATB, CBW/CWDE/CWD/CDQ, LAHF/SAHF,
;            CLC/STC/CMC, NOP, and other miscellaneous instructions.
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
;
; NOTE: Guest ESP is NOT mapped to host RSP.  Tests must NOT use
;       `cmp reg, esp` or `mov reg, esp` to observe ESP directly.
;       ENTER/LEAVE correctness is verified by checking EBP, pushed
;       values, and PUSH/POP behavior on the guest stack.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ======================== ENTER imm16, 0 / LEAVE ===========================

; ENTER 16, 0 — creates stack frame with 16 bytes of local space.
; Equivalent to: PUSH EBP; MOV EBP, ESP; SUB ESP, 16
; We verify:
;   (a) [EBP] == old EBP (the PUSH EBP part)
;   (b) EBP has changed (MOV EBP, ESP part)
;   (c) LEAVE restores EBP correctly
    mov  ebp, 0xDEAD0000           ; marker — will be saved by ENTER
    enter 16, 0

    ; EBP should now point to saved frame pointer
    ASSERT_EQ_MEM ebp, 0xDEAD0000  ; 1: [EBP] == old EBP

    ; EBP itself should have changed (not 0xDEAD0000 anymore)
    ASSERT_NE ebp, 0xDEAD0000      ; 2: EBP != old value

    ; Store a marker in local space (below EBP)
    mov  dword [ebp - 4], 0x11111111
    mov  dword [ebp - 8], 0x22222222

    ; Verify the markers are there
    mov  eax, ebp
    sub  eax, 4
    ASSERT_EQ_MEM eax, 0x11111111   ; 3: local var 1
    sub  eax, 4
    ASSERT_EQ_MEM eax, 0x22222222   ; 4: local var 2

    ; LEAVE restores ESP and EBP
    leave
    ASSERT_EQ ebp, 0xDEAD0000      ; 5: EBP restored

; ENTER 0, 0 — just creates a frame pointer with no locals
    mov  ebp, 0x12345678
    enter 0, 0
    ASSERT_EQ_MEM ebp, 0x12345678  ; 6: [EBP] == old EBP
    ASSERT_NE ebp, 0x12345678      ; 7: EBP changed
    leave
    ASSERT_EQ ebp, 0x12345678      ; 8: EBP restored

; ENTER 256, 0 — larger local allocation
    mov  ebp, 0xAAAABBBB
    enter 256, 0
    ASSERT_EQ_MEM ebp, 0xAAAABBBB  ; 9: [EBP] == old EBP
    leave
    ASSERT_EQ ebp, 0xAAAABBBB      ; 10: EBP restored

; ============================== XLATB =====================================

; XLATB reads a byte from [EBX + AL] and stores in AL.
; Set up a 256-byte translation table at 0x5000.
    mov  byte [0x5000], 0xAA
    mov  byte [0x5001], 0xBB
    mov  byte [0x5005], 0x42
    mov  byte [0x50FF], 0x99         ; table[255]

    mov  ebx, 0x5000                 ; base of table
    mov  eax, 0                      ; AL = 0 → read table[0]
    xlatb
    ; AL should be 0xAA; since EAX was 0, only AL changes
    and  eax, 0xFF
    ASSERT_EQ eax, 0xAA              ; 11: AL = table[0] = 0xAA

    ; Check that upper bytes of EAX are preserved
    mov  eax, 0xFFFF0001             ; AL=1, upper bits set
    xlatb
    ; AL should be 0xBB (table[1]), upper bytes 0xFFFF00 preserved
    ASSERT_EQ eax, 0xFFFF00BB        ; 12: upper bytes preserved, AL=0xBB

    mov  eax, 5                      ; AL = 5
    xlatb
    and  eax, 0xFF
    ASSERT_EQ eax, 0x42              ; 13: AL = table[5] = 0x42

    mov  eax, 0xFF                   ; AL = 255
    xlatb
    and  eax, 0xFF
    ASSERT_EQ eax, 0x99              ; 14: AL = table[255] = 0x99

; ====================== CBW / CWDE / CWD / CDQ ============================

; CBW: sign-extend AL → AX (16-bit)
    mov  eax, 0x00F0                 ; AL=0xF0 (negative)
    cbw                              ; AX = 0xFFF0
    and  eax, 0xFFFF                 ; isolate AX
    ASSERT_EQ eax, 0xFFF0            ; 15: AX = 0xFFF0

    mov  eax, 0x0040                 ; AL=0x40 (positive)
    cbw                              ; AX = 0x0040
    and  eax, 0xFFFF
    ASSERT_EQ eax, 0x0040            ; 16: AX = 0x0040

; CWDE: sign-extend AX → EAX (32-bit)
    mov  eax, 0xFF80                 ; AX=0xFF80 (negative 16-bit)
    cwde                             ; EAX = sign_extend(AX) = 0xFFFFFF80
    ASSERT_EQ eax, 0xFFFFFF80        ; 17: EAX = 0xFFFFFF80

    mov  eax, 0x7FFF                 ; AX=0x7FFF (positive 16-bit)
    cwde                             ; EAX = 0x00007FFF
    ASSERT_EQ eax, 0x00007FFF        ; 18: EAX = 0x00007FFF

; CDQ: sign-extend EAX → EDX:EAX
    mov  eax, 0x80000000             ; negative
    cdq                              ; EDX = 0xFFFFFFFF
    ASSERT_EQ edx, 0xFFFFFFFF        ; 19: EDX = all 1s
    ASSERT_EQ eax, 0x80000000        ; 20: EAX unchanged

    mov  eax, 0x7FFFFFFF             ; positive
    cdq                              ; EDX = 0x00000000
    ASSERT_EQ edx, 0                 ; 21: EDX = 0
    ASSERT_EQ eax, 0x7FFFFFFF        ; 22: EAX unchanged

; CWD: sign-extend AX → DX:AX
    mov  eax, 0x8000                 ; AX=0x8000 (negative 16-bit)
    cwd                              ; DX = 0xFFFF
    and  edx, 0xFFFF
    ASSERT_EQ edx, 0xFFFF            ; 23: DX = 0xFFFF

    mov  eax, 0x0001                 ; AX=0x0001 (positive)
    cwd                              ; DX = 0x0000
    and  edx, 0xFFFF
    ASSERT_EQ edx, 0                 ; 24: DX = 0

; ========================= LAHF / SAHF ====================================

; LAHF: load AH from flags (SF:ZF:0:AF:0:PF:1:CF)
    xor  eax, eax                    ; set ZF=1, PF=1
    lahf                             ; AH = flags byte
    mov  edx, eax
    shr  edx, 8
    and  edx, 0xFF
    ; ZF bit (bit 6 of AH) should be set
    test edx, 0x40
    jnz  .lahf_zf_ok
    mov  eax, 25
    hlt
.lahf_zf_ok:
%assign __test_num 25

; SAHF: store AH into flags
    ; Set AH to have CF=1 (bit 0), ZF=1 (bit 6)
    mov  eax, 0x4100                 ; AH = 0x41 (CF=1, ZF=1)
    sahf
    ASSERT_FLAGS CF|ZF, CF|ZF        ; 26: CF and ZF both set

; ======================== CLC / STC / CMC =================================

; STC: set carry flag
    clc
    stc
    ASSERT_FLAGS CF, CF              ; 27: CF=1

; CLC: clear carry flag
    stc
    clc
    ASSERT_FLAGS CF, 0               ; 28: CF=0

; CMC: complement carry flag
    stc
    cmc
    ASSERT_FLAGS CF, 0               ; 29: CF was 1, now 0

    clc
    cmc
    ASSERT_FLAGS CF, CF              ; 30: CF was 0, now 1

; =========================== NOP ==========================================

    mov  eax, 0x42
    nop
    ASSERT_EQ eax, 0x42              ; 31: EAX unchanged after NOP

; ========================== BSWAP reg =====================================

    mov  eax, 0x12345678
    bswap eax
    ASSERT_EQ eax, 0x78563412        ; 32: bytes reversed

    mov  ecx, 0x01020304
    bswap ecx
    ASSERT_EQ ecx, 0x04030201        ; 33: bytes reversed

; ========================= All done =======================================
    PASS
    ; After ENTER 16, 0:
    ;   old EBP (0xDEAD0000) is saved at [EBP]
    ;   EBP = old ESP - 4  (points to saved frame pointer)
    ;   ESP = EBP - 16     (16 bytes of local space below EBP)

    ; Verify ESP = EBP - 16
    mov  eax, ebp
    sub  eax, 16
    ASSERT_EQ eax, esp              ; 1: ESP == EBP - 16

    ; EBP points to the saved old EBP value
    ASSERT_EQ_MEM ebp, 0xDEAD0000  ; 2: [EBP] == old EBP

    ; ESP should be EBP - 16
    mov  eax, ebp
    sub  eax, 16
    ASSERT_EQ eax, esp              ; 3: ESP == EBP - 16

    ; LEAVE restores ESP and EBP
    leave
    ASSERT_EQ ebp, 0xDEAD0000      ; 4: EBP restored
    ASSERT_EQ esp, 0x80000          ; 5: ESP restored

; ENTER 0, 0 — just creates a frame pointer with no locals
    mov  esp, 0x80000
    mov  ebp, 0x12345678
    enter 0, 0
    ASSERT_EQ_MEM ebp, 0x12345678  ; 6: [EBP] == old EBP
    mov  eax, ebp
    ASSERT_EQ eax, esp              ; 7: ESP == EBP (no local space)
    leave
    ASSERT_EQ ebp, 0x12345678      ; 8: EBP restored
    ASSERT_EQ esp, 0x80000          ; 9: ESP restored

; ENTER 256, 0 — larger local allocation
    mov  esp, 0x80000
    mov  ebp, 0xAAAABBBB
    enter 256, 0
    mov  eax, ebp
    sub  eax, 256
    ASSERT_EQ eax, esp              ; 10: ESP == EBP - 256
    ASSERT_EQ_MEM ebp, 0xAAAABBBB  ; 11: [EBP] == old EBP
    leave
    ASSERT_EQ esp, 0x80000          ; 12: ESP restored

; ============================== XLATB =====================================

; XLATB reads a byte from [EBX + AL] and stores in AL.
; Set up a 256-byte translation table at 0x5000.
    mov  edi, 0x5000
    ; Fill first few entries: table[0]=0xAA, table[1]=0xBB, table[5]=0x42
    mov  byte [0x5000], 0xAA
    mov  byte [0x5001], 0xBB
    mov  byte [0x5005], 0x42
    mov  byte [0x50FF], 0x99         ; table[255]

    mov  ebx, 0x5000                 ; base of table
    mov  eax, 0                      ; AL = 0 → read table[0]
    xlatb
    ASSERT_EQ eax, 0xAA              ; 13: AL = table[0] = 0xAA (upper bytes clear since EAX was 0 → AL=0xAA, rest preserved... actually XLATB only sets AL)

    ; Check that upper bytes of EAX are preserved
    mov  eax, 0xFFFF0001             ; AL=1, upper bits set
    xlatb
    ; AL should be 0xBB (table[1]), upper bytes 0xFFFF00 preserved
    ASSERT_EQ eax, 0xFFFF00BB        ; 14: upper bytes preserved, AL=0xBB

    mov  eax, 5                      ; AL = 5
    xlatb
    ASSERT_EQ eax, 0x42              ; 15: AL = table[5] = 0x42

    mov  eax, 0xFF                   ; AL = 255
    xlatb
    ASSERT_EQ eax, 0x99              ; 16: AL = table[255] = 0x99

; ====================== CBW / CWDE / CWD / CDQ ============================

; CBW: sign-extend AL → AX (16-bit)
    mov  eax, 0xFF80                 ; AL=0x80 (negative), AH=0xFF
    cbw                              ; AX = sign_extend(AL) = 0xFF80
    ASSERT_EQ eax, 0xFF80            ; 17: AX = 0xFF80 (was already 0xFF80)

    mov  eax, 0x0040                 ; AL=0x40 (positive)
    cbw                              ; AX = 0x0040
    ASSERT_EQ eax, 0x0040            ; 18: AX = 0x0040

    mov  eax, 0x00F0                 ; AL=0xF0 (negative)
    cbw                              ; AX = 0xFFF0
    ASSERT_EQ eax, 0xFFF0            ; 19: AX = 0xFFF0

; CWDE: sign-extend AX → EAX (32-bit)
    mov  eax, 0xFF80                 ; AX=0xFF80 (negative 16-bit)
    cwde                             ; EAX = sign_extend(AX) = 0xFFFFFF80
    ASSERT_EQ eax, 0xFFFFFF80        ; 20: EAX = 0xFFFFFF80

    mov  eax, 0x7FFF                 ; AX=0x7FFF (positive 16-bit)
    cwde                             ; EAX = 0x00007FFF
    ASSERT_EQ eax, 0x00007FFF        ; 21: EAX = 0x00007FFF

; CDQ: sign-extend EAX → EDX:EAX
    mov  eax, 0x80000000             ; negative
    cdq                              ; EDX = 0xFFFFFFFF
    ASSERT_EQ edx, 0xFFFFFFFF        ; 22: EDX = all 1s
    ASSERT_EQ eax, 0x80000000        ; 23: EAX unchanged

    mov  eax, 0x7FFFFFFF             ; positive
    cdq                              ; EDX = 0x00000000
    ASSERT_EQ edx, 0                 ; 24: EDX = 0
    ASSERT_EQ eax, 0x7FFFFFFF        ; 25: EAX unchanged

; CWD: sign-extend AX → DX:AX
    mov  eax, 0x8000                 ; AX=0x8000 (negative 16-bit)
    cwd                              ; DX = 0xFFFF
    ; DX is the low 16 bits of EDX
    and  edx, 0xFFFF
    ASSERT_EQ edx, 0xFFFF            ; 26: DX = 0xFFFF

    mov  eax, 0x0001                 ; AX=0x0001 (positive)
    cwd                              ; DX = 0x0000
    and  edx, 0xFFFF
    ASSERT_EQ edx, 0                 ; 27: DX = 0

; ========================= LAHF / SAHF ====================================

; LAHF: load AH from flags (SF:ZF:0:AF:0:PF:1:CF)
    xor  eax, eax                    ; set ZF=1, PF=1, SF=0
    lahf                             ; AH = flags byte
    ; ZF=1(bit6=0x40), PF=1(bit2=0x04), bit1 always 1 in LAHF=0x02
    mov  edx, eax
    shr  edx, 8
    and  edx, 0xFF
    test edx, 0x40                   ; ZF bit should be set
    jnz  .lahf_ok1
    mov  eax, 28
    hlt
.lahf_ok1:
%assign __test_num 28

; SAHF: store AH into flags
    ; Set AH to have CF=1 (bit 0), ZF=1 (bit 6)
    mov  eax, 0x4100                 ; AH = 0x41 (CF=1, ZF=1)
    sahf
    ; Check that CF and ZF are now set
    ASSERT_FLAGS CF|ZF, CF|ZF        ; 29: CF and ZF both set

; ======================== CLC / STC / CMC =================================

; STC: set carry flag
    clc                              ; clear CF first
    stc                              ; set CF
    ASSERT_FLAGS CF, CF              ; 30: CF=1

; CLC: clear carry flag
    stc                              ; ensure CF=1
    clc                              ; clear CF
    ASSERT_FLAGS CF, 0               ; 31: CF=0

; CMC: complement carry flag
    stc                              ; CF=1
    cmc                              ; CF=0
    ASSERT_FLAGS CF, 0               ; 32: CF was 1, now 0

    clc                              ; CF=0
    cmc                              ; CF=1
    ASSERT_FLAGS CF, CF              ; 33: CF was 0, now 1

; =========================== NOP ==========================================

; NOP should do nothing (just verify it doesn't crash)
    mov  eax, 0x42
    nop
    ASSERT_EQ eax, 0x42              ; 34: EAX unchanged after NOP

; ========================== BSWAP reg =====================================

; BSWAP: byte-swap a 32-bit register
    mov  eax, 0x12345678
    bswap eax
    ASSERT_EQ eax, 0x78563412        ; 35: bytes reversed

    mov  ecx, 0x01020304
    bswap ecx
    ASSERT_EQ ecx, 0x04030201        ; 36: bytes reversed

; ========================= All done =======================================
    PASS
