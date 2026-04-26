; ===========================================================================
; nv2a_timer.asm — NV2A PTIMER freerunning counter test (--xbox mode).
;
; Tests:
;   1. PTIMER_TIME_0 is non-zero (timer is running)
;   2. PTIMER_TIME_0 increases between two reads
;   3. PTIMER_TIME_0 low 5 bits are always zero (sub-ns granularity)
;   4. Write PTIMER_NUMERATOR, read back
;   5. Write PTIMER_DENOMINATOR, read back
;   6. Read PMC_BOOT_0 sanity check (NV2A chip ID)
; ===========================================================================

%include "harness.inc"

; NV2A MMIO base
NV2A equ 0xFD000000

section .text
org 0x1000

start:
    ; ================================================================
    ; Test 1: PTIMER_TIME_0 should be non-zero after some execution.
    ; ================================================================
    ; Do some work first so the timer advances.
    mov ecx, 100
.warmup:
    nop
    dec ecx
    jnz .warmup

    mov eax, [NV2A + 0x009400]      ; PTIMER_TIME_0
    ASSERT_NE eax, 0

    ; ================================================================
    ; Test 2: PTIMER_TIME_0 increases between reads.
    ; ================================================================
    mov ebx, [NV2A + 0x009400]      ; first read
    ; Burn some cycles.
    mov ecx, 50
.delay:
    nop
    dec ecx
    jnz .delay
    mov eax, [NV2A + 0x009400]      ; second read
    ; eax should be > ebx (timer advanced).
    cmp eax, ebx
    ja .time_advanced
    ; Could wrap — check TIME_1 instead. Extremely unlikely with 100ns ticks.
    mov eax, __test_num + 1
    hlt
.time_advanced:
    %assign __test_num __test_num + 1

    ; ================================================================
    ; Test 3: Low 5 bits of TIME_0 are always zero.
    ; ================================================================
    mov eax, [NV2A + 0x009400]
    and eax, 0x1F               ; bits [4:0]
    ASSERT_EQ eax, 0

    ; ================================================================
    ; Test 4: Write PTIMER_NUMERATOR, read back.
    ; ================================================================
    mov dword [NV2A + 0x009200], 42
    mov eax, [NV2A + 0x009200]
    ASSERT_EQ eax, 42

    ; ================================================================
    ; Test 5: Write PTIMER_DENOMINATOR, read back.
    ; ================================================================
    mov dword [NV2A + 0x009210], 7
    mov eax, [NV2A + 0x009210]
    ASSERT_EQ eax, 7

    ; ================================================================
    ; Test 6: PMC_BOOT_0 sanity check.
    ; ================================================================
    mov eax, [NV2A + 0x000000]
    ASSERT_EQ eax, 0x02A000A1

    PASS
