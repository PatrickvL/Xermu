; ===========================================================================
; apu_voice.asm — APU voice processor memory tests (--xbox mode).
;
; Tests VP voice slot read/write at APU_BASE + 0x20000:
;   1. Voice 0 config default = 0
;   2. Voice 0 config write + readback
;   3. Voice 0 target pitch write + readback
;   4. Voice 0 current volume write + readback
;   5. Voice 1 config (slot offset 128 bytes)
;   6. Voice envelope attack rate
;   7. FETFORCE1 SE2FE_IDLE bit always set
;   8. VP voice base address register writable
;   9. VP scatter-gather base writable
;  10. VP SSL base address writable
; ===========================================================================

%include "harness.inc"

ORG 0x1000

APU     equ 0xFE800000
VP_BASE equ APU + 0x20000

; Each voice = 128 bytes = 0x80
VOICE0  equ VP_BASE
VOICE1  equ VP_BASE + 0x80

; === 1. Voice 0 config default = 0 ===
    mov eax, [VOICE0 + 0x00]      ; V_CFG
    ASSERT_EQ eax, 0

; === 2. Voice 0 config write + readback ===
    mov dword [VOICE0 + 0x00], 0x00010001  ; e.g. format + active
    mov eax, [VOICE0 + 0x00]
    ASSERT_EQ eax, 0x00010001

; === 3. Voice 0 target pitch ===
    mov dword [VOICE0 + 0x08], 0x00001000  ; pitch value
    mov eax, [VOICE0 + 0x08]
    ASSERT_EQ eax, 0x00001000

; === 4. Voice 0 current volume ===
    mov dword [VOICE0 + 0x10], 0x0FFF0FFF
    mov eax, [VOICE0 + 0x10]
    ASSERT_EQ eax, 0x0FFF0FFF

; === 5. Voice 1 config ===
    mov dword [VOICE1 + 0x00], 0xABCD0002
    mov eax, [VOICE1 + 0x00]
    ASSERT_EQ eax, 0xABCD0002

; === 6. Voice 0 envelope attack rate ===
    mov dword [VOICE0 + 0x40], 0x00FF00FF  ; EAR
    mov eax, [VOICE0 + 0x40]
    ASSERT_EQ eax, 0x00FF00FF

; === 7. FETFORCE1 bit 7 always set ===
    mov eax, [APU + 0x1504]        ; FETFORCE1
    and eax, 0x80
    ASSERT_EQ eax, 0x80

; === 8. VP voice base address ===
    mov dword [APU + 0x202C], 0x10000000
    mov eax, [APU + 0x202C]
    ASSERT_EQ eax, 0x10000000

; === 9. VP scatter-gather base ===
    mov dword [APU + 0x2030], 0x20000000
    mov eax, [APU + 0x2030]
    ASSERT_EQ eax, 0x20000000

; === 10. VP SSL base address ===
    mov dword [APU + 0x2034], 0x30000000
    mov eax, [APU + 0x2034]
    ASSERT_EQ eax, 0x30000000

    PASS
