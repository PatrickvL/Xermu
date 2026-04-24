; ===========================================================================
; segment.asm additions — GDT/TSS descriptor table tests
; ===========================================================================
; This test is in tests/gdt_tss.asm to test LGDT/SGDT/LIDT/SIDT/LTR/STR/LLDT/SLDT
; ===========================================================================
%include "harness.inc"

org 0x1000

    ; ------------------------------------------------------------------
    ; Setup: build a minimal GDT at address 0x2000
    ; ------------------------------------------------------------------

    ; GDT pseudo-descriptor at 0x3000 (6 bytes: limit, base)
    mov word  [0x3000], 0x001F     ; limit = 31 (4 entries × 8 − 1)
    mov dword [0x3002], 0x00002000 ; base  = 0x2000

    ; GDT entry 0: null descriptor (required)
    mov dword [0x2000], 0x00000000
    mov dword [0x2004], 0x00000000

    ; GDT entry 1 (selector 0x08): code segment, base=0, limit=4GB
    mov dword [0x2008], 0x0000FFFF ; limit[15:0]=FFFF, base[15:0]=0
    mov dword [0x200C], 0x00CF9A00 ; base[31:24]=0, G=1, D=1, limit[19:16]=F, P=1, DPL=0, S=1, type=A (exec/read)

    ; GDT entry 2 (selector 0x10): data segment, base=0, limit=4GB
    mov dword [0x2010], 0x0000FFFF
    mov dword [0x2014], 0x00CF9200

    ; GDT entry 3 (selector 0x18): 32-bit TSS descriptor, base=0x4000, limit=0x67
    ; TSS type = 0x9 (available 32-bit TSS), P=1, DPL=0
    mov dword [0x2018], 0x40000067 ; base[15:0]=0x4000, limit[15:0]=0x67
    mov dword [0x201C], 0x00008900 ; base[31:24]=0, G=0, limit[19:16]=0, P=1, DPL=0, type=9

    ; ------------------------------------------------------------------
    ; 1. LGDT — load the GDT
    ; ------------------------------------------------------------------
    lgdt [0x3000]

    ; ------------------------------------------------------------------
    ; 2. SGDT — store it back to 0x3010 and verify
    ; ------------------------------------------------------------------
    sgdt [0x3010]
    mov ax, [0x3010]
    movzx eax, ax
    ASSERT_EQ eax, 0x001F          ; limit

    mov eax, [0x3012]
    ASSERT_EQ eax, 0x00002000      ; base

    ; ------------------------------------------------------------------
    ; 3. IDT pseudo-descriptor at 0x3020
    ; ------------------------------------------------------------------
    mov word  [0x3020], 0x07FF     ; limit = 2047 (256 entries × 8 − 1)
    mov dword [0x3022], 0x00005000 ; base  = 0x5000

    lidt [0x3020]

    ; ------------------------------------------------------------------
    ; 4. SIDT — store it back and verify
    ; ------------------------------------------------------------------
    sidt [0x3030]
    mov ax, [0x3030]
    movzx eax, ax
    ASSERT_EQ eax, 0x07FF          ; limit

    mov eax, [0x3032]
    ASSERT_EQ eax, 0x00005000      ; base

    ; ------------------------------------------------------------------
    ; 5. LLDT — load LDT selector
    ; ------------------------------------------------------------------
    mov ax, 0
    lldt ax                         ; selector 0 = no LDT

    ; ------------------------------------------------------------------
    ; 6. SLDT — read back LDT selector
    ; ------------------------------------------------------------------
    sldt ax
    movzx eax, ax
    ASSERT_EQ eax, 0               ; should be 0

    ; ------------------------------------------------------------------
    ; 7. LTR — load Task Register with selector 0x18 (TSS entry)
    ; ------------------------------------------------------------------
    mov ax, 0x18
    ltr ax

    ; ------------------------------------------------------------------
    ; 8. STR — read back Task Register selector
    ; ------------------------------------------------------------------
    str ax
    movzx eax, ax
    ASSERT_EQ eax, 0x18

    ; ------------------------------------------------------------------
    ; 9. LLDT with non-zero selector, then SLDT to verify
    ; ------------------------------------------------------------------
    mov ax, 0x28                   ; hypothetical LDT selector
    lldt ax
    sldt bx
    movzx eax, bx
    ASSERT_EQ eax, 0x28

    ; ------------------------------------------------------------------
    ; 10. SGDT from a register-indirect address
    ; ------------------------------------------------------------------
    mov ebx, 0x3040
    sgdt [ebx]
    mov eax, [0x3042]
    ASSERT_EQ eax, 0x00002000      ; GDT base

    PASS
