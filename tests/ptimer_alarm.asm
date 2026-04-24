; ===========================================================================
; ptimer_alarm.asm — NV2A PTIMER alarm tests (--xbox mode).
;
; Tests PTIMER alarm and interrupt functionality:
;   1. PTIMER_INTR default = 0
;   2. PTIMER_INTR_EN write/read
;   3. PTIMER_ALARM_0 write/read
;   4. PTIMER_NUM write/read
;   5. PTIMER_DEN write/read
;   6. TIME_0 advances over time (non-zero after spinning)
;   7. PTIMER_INTR fires when TIME_0 >= ALARM_0
;   8. PTIMER_INTR W1C clears interrupt
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A equ 0xFD000000
PT   equ NV2A + 0x009000

; === 1. PTIMER_INTR default = 0 ===
    mov eax, [PT + 0x100]
    ASSERT_EQ eax, 0

; === 2. PTIMER_INTR_EN write/read ===
    mov dword [PT + 0x140], 1
    mov eax, [PT + 0x140]
    ASSERT_EQ eax, 1

; === 3. PTIMER_ALARM_0 write/read ===
    mov dword [PT + 0x420], 0x00001000
    mov eax, [PT + 0x420]
    ASSERT_EQ eax, 0x00001000

; === 4. PTIMER_NUM write/read ===
    mov dword [PT + 0x200], 5
    mov eax, [PT + 0x200]
    ASSERT_EQ eax, 5
    ; Restore default
    mov dword [PT + 0x200], 1

; === 5. PTIMER_DEN write/read ===
    mov dword [PT + 0x210], 3
    mov eax, [PT + 0x210]
    ASSERT_EQ eax, 3
    ; Restore default
    mov dword [PT + 0x210], 1

; === 6. TIME_0 advances (spin until non-zero) ===
    ; Set alarm far away so it doesn't interfere
    mov dword [PT + 0x420], 0xFFFFFFE0
    ; Clear any pending INTR
    mov dword [PT + 0x100], 0xFFFFFFFF
    mov ecx, 500000
.spin_time:
    mov eax, [PT + 0x400]    ; TIME_0
    test eax, eax
    jnz .time_nonzero
    dec ecx
    jnz .spin_time
    mov eax, __test_num + 1
    hlt
.time_nonzero:
    %assign __test_num __test_num + 1

; === 7. PTIMER_INTR fires when alarm reached ===
    ; Set alarm to a low value that we've already passed
    mov eax, [PT + 0x400]    ; read current TIME_0
    ; Set alarm to current time + small delta (will be reached quickly)
    add eax, 0x00000100
    and eax, 0xFFFFFFE0       ; align like TIME_0
    mov [PT + 0x420], eax
    ; Clear INTR first
    mov dword [PT + 0x100], 0xFFFFFFFF
    ; Spin until INTR fires
    mov ecx, 500000
.spin_alarm:
    mov eax, [PT + 0x100]
    test eax, 1
    jnz .alarm_fired
    dec ecx
    jnz .spin_alarm
    mov eax, __test_num + 1
    hlt
.alarm_fired:
    %assign __test_num __test_num + 1

; === 8. PTIMER_INTR W1C ===
    mov dword [PT + 0x100], 1
    mov eax, [PT + 0x100]
    ASSERT_EQ eax, 0

    PASS
