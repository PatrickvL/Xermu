; ===========================================================================
; xbox_io.asm — Miscellaneous Xbox I/O port tests (--xbox mode).
;
; Tests system control port (0x61), POST code port (0x80), and
; debug console signature (0xE9):
;   1. System port B read returns value with bit 5 set
;   2. System port B refresh toggle: two reads differ in bit 4
;   3. System port B write bits 0-1, readback preserves them
;   4. POST code write/read (port 0x80)
;   5. POST code second write overwrites first
;   6. Debug console read returns magic 0xE9
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; === 1. System port B read — initial value has bit 5 set ===
    in  al, 0x61
    movzx eax, al
    and eax, 0x20          ; bit 5 = timer 2 output
    ASSERT_EQ eax, 0x20

; === 2. Refresh toggle: bit 4 toggles between reads ===
    in  al, 0x61
    movzx ecx, al
    and ecx, 0x10          ; capture bit 4
    in  al, 0x61
    movzx edx, al
    and edx, 0x10
    xor eax, eax
    cmp ecx, edx
    setne al               ; 1 if they differ
    ASSERT_EQ eax, 1

; === 3. Write bits 0-1, readback preserves them ===
    mov al, 0x03           ; set timer gate + speaker
    out 0x61, al
    in  al, 0x61
    movzx eax, al
    and eax, 0x03
    ASSERT_EQ eax, 0x03

; === 4. POST code write/read (port 0x80) ===
    mov al, 0xAA
    out 0x80, al
    in  al, 0x80
    movzx eax, al
    ASSERT_EQ eax, 0xAA

; === 5. POST code second write overwrites ===
    mov al, 0x55
    out 0x80, al
    in  al, 0x80
    movzx eax, al
    ASSERT_EQ eax, 0x55

; === 6. Debug console read returns magic 0xE9 ===
    mov dx, 0xE9
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xE9

    PASS
