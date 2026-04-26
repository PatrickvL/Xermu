; ===========================================================================
; mtrr.asm — MTRR MSR read/write tests (8 assertions)
; ===========================================================================
%include "harness.inc"

org 0x1000

    ; ------------------------------------------------------------------
    ; 1. Read IA32_MTRRCAP (MSR 0x2FF) — should report 8 variable ranges
    ; ------------------------------------------------------------------
    mov ecx, 0x2FF
    rdmsr
    ; EAX should have VCNT=8 (bits 7:0) and FIX=1 (bit 8)
    and eax, 0x1FF          ; mask VCNT + FIX bit
    ASSERT_EQ eax, 0x108    ; 8 variable ranges, fixed-range support

    ; ------------------------------------------------------------------
    ; 2. Write + read IA32_MTRR_DEF_TYPE (MSR 0xFE)
    ; ------------------------------------------------------------------
    mov ecx, 0xFE
    mov edx, 0
    mov eax, 0x00000C06     ; enable MTRR (bit 11) + fixed (bit 10) + WB default (6)
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0x00000C06

    ; ------------------------------------------------------------------
    ; 3. Write + read variable range PHYSBASE0 (MSR 0x200)
    ; ------------------------------------------------------------------
    mov ecx, 0x200
    mov edx, 0
    mov eax, 0x00000006     ; base=0, WB type
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0x00000006

    ; ------------------------------------------------------------------
    ; 4. Write + read variable range PHYSMASK0 (MSR 0x201)
    ; ------------------------------------------------------------------
    mov ecx, 0x201
    mov edx, 0x0000000F     ; high bits of mask
    mov eax, 0xFC000800     ; valid bit (11) + mask
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0xFC000800
    ; 5. Check EDX part too
    ASSERT_EQ edx, 0x0000000F

    ; ------------------------------------------------------------------
    ; 6. Write + read FIX64K (MSR 0x250)
    ; ------------------------------------------------------------------
    mov ecx, 0x250
    mov edx, 0x06060606
    mov eax, 0x06060606     ; all 64K chunks = WB
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0x06060606

    ; ------------------------------------------------------------------
    ; 7. Write + read FIX4K_C0000 (MSR 0x268)
    ; ------------------------------------------------------------------
    mov ecx, 0x268
    mov edx, 0
    mov eax, 0x05050505     ; WP type
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0x05050505

    ; ------------------------------------------------------------------
    ; 8. Write + read FIX16K_80000 (MSR 0x258)
    ; ------------------------------------------------------------------
    mov ecx, 0x258
    mov edx, 0x01010101
    mov eax, 0x01010101     ; WC type
    wrmsr
    rdmsr
    ASSERT_EQ eax, 0x01010101

    PASS
