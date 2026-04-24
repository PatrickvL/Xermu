; ===========================================================================
; segment.asm — FS/GS segment override tests.
;
; Test runner sets:
;   FS base = 0x70000
;   GS base = 0
; ===========================================================================

%include "harness.inc"

ORG 0x1000

%define FS_BASE 0x70000

; ---- MOV to FS:[offset], then read back via FS and via flat addr ----
    mov dword [fs:0x00], 0xDEAD0001
    ASSERT_EQ_MEM FS_BASE, 0xDEAD0001            ; 1: flat readback matches

    mov dword [fs:0x04], 0xCAFE0002
    ASSERT_EQ_MEM FS_BASE+4, 0xCAFE0002          ; 2

; ---- Read via FS override ----
    mov eax, [fs:0x00]
    ASSERT_EQ eax, 0xDEAD0001                     ; 3

    mov ebx, [fs:0x04]
    ASSERT_EQ ebx, 0xCAFE0002                     ; 4

; ---- FS with register base ----
    mov ecx, 0x10
    mov dword [fs:ecx], 0xBEEF0003
    mov eax, FS_BASE + 0x10
    ASSERT_EQ_MEM eax, 0xBEEF0003                ; 5

    mov edx, [fs:ecx]
    ASSERT_EQ edx, 0xBEEF0003                     ; 6

; ---- FS with base + displacement ----
    mov ebx, 0x20
    mov dword [fs:ebx+4], 0x12340005
    mov eax, FS_BASE + 0x24
    ASSERT_EQ_MEM eax, 0x12340005                 ; 7

    mov eax, [fs:ebx+4]
    ASSERT_EQ eax, 0x12340005                      ; 8

; ---- FS with base + index*scale ----
    mov ecx, 3
    mov dword [fs:ecx*4], 0xAAAA0006
    mov eax, FS_BASE + 12
    ASSERT_EQ_MEM eax, 0xAAAA0006                 ; 9

; ---- FS with base + index*scale + disp ----
    mov esi, 0x40
    mov ecx, 2
    mov dword [fs:esi+ecx*4], 0xBBBB0007
    mov eax, FS_BASE + 0x48
    ASSERT_EQ_MEM eax, 0xBBBB0007                 ; 10

    mov eax, [fs:esi+ecx*4]
    ASSERT_EQ eax, 0xBBBB0007                      ; 11

; ---- 8-bit and 16-bit FS access ----
    mov byte [fs:0x30], 0x42
    movzx eax, byte [FS_BASE + 0x30]
    ASSERT_EQ eax, 0x42                            ; 12

    mov word [fs:0x32], 0x1234
    movzx eax, word [FS_BASE + 0x32]
    ASSERT_EQ eax, 0x1234                          ; 13

; ---- ADD to FS memory (ALU mem form) ----
    mov dword [fs:0x50], 100
    add dword [fs:0x50], 50
    mov eax, [fs:0x50]
    ASSERT_EQ eax, 150                             ; 14

; ---- CMP with FS memory ----
    mov dword [fs:0x54], 42
    cmp dword [fs:0x54], 42
    ASSERT_FLAGS 0x40, 0x40                        ; 15: ZF set (equal)

; ---- MOVZX from FS memory ----
    mov byte [fs:0x58], 0xFF
    movzx eax, byte [fs:0x58]
    ASSERT_EQ eax, 0xFF                            ; 16

; ---- MOVSX from FS memory ----
    mov byte [fs:0x59], 0x80
    movsx eax, byte [fs:0x59]
    ASSERT_EQ eax, 0xFFFFFF80                      ; 17

; ---- Multiple FS accesses in sequence (different offsets) ----
    mov dword [fs:0x60], 1
    mov dword [fs:0x64], 2
    mov dword [fs:0x68], 3
    mov eax, [fs:0x60]
    mov ebx, [fs:0x64]
    mov ecx, [fs:0x68]
    add eax, ebx
    add eax, ecx
    ASSERT_EQ eax, 6                               ; 18

; ---- FS store from register ----
    mov eax, 0x55AA55AA
    mov [fs:0x70], eax
    ASSERT_EQ_MEM FS_BASE+0x70, 0x55AA55AA        ; 19

; ---- EBP-based FS access (EBP is special in ModRM) ----
    mov ebp, 0x80
    mov dword [fs:ebp], 0xFACEB00B
    mov eax, FS_BASE + 0x80
    ASSERT_EQ_MEM eax, 0xFACEB00B                   ; 20

    mov eax, [fs:ebp]
    ASSERT_EQ eax, 0xFACEB00B                        ; 21

; ---- SUB from FS memory ----
    mov dword [fs:0x90], 200
    sub dword [fs:0x90], 75
    mov eax, [fs:0x90]
    ASSERT_EQ eax, 125                              ; 22

    PASS
