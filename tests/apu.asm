; ===========================================================================
; apu.asm — MCPX APU register tests (--xbox mode).
;
; Tests the Xbox audio processing unit MMIO stub at 0xFE800000:
;   1. ISTS reads 0 on init (no pending interrupts)
;   2. IEN write/read
;   3. FECTL write/read
;   4. FESTATE reads 0 (idle) on init
;   5. FESTATUS reads 0 (not busy) on init
;   6. SESTATUS reads 0 (idle) on init
;   7. GPRST write/read
;   8. GPSADDR write/read
;   9. EPRST write/read
;  10. EPSADDR write/read
;  11. ISTS W1C behavior
;  12. SECTL write/read
; ===========================================================================

%include "harness.inc"

ORG 0x1000

APU equ 0xFE800000

; General APU registers
NV_PAPU_ISTS      equ APU + 0x1000
NV_PAPU_IEN       equ APU + 0x1004
NV_PAPU_FECTL     equ APU + 0x1100
NV_PAPU_FECV      equ APU + 0x1104
NV_PAPU_FESTATE   equ APU + 0x1108
NV_PAPU_FESTATUS  equ APU + 0x110C

; Setup engine
NV_PAPU_SECTL     equ APU + 0x2000
NV_PAPU_SESTATUS  equ APU + 0x200C

; Global Processor
NV_PAPU_GPRST     equ APU + 0x3000
NV_PAPU_GPISTS    equ APU + 0x3004
NV_PAPU_GPSADDR   equ APU + 0x3008

; Encode Processor
NV_PAPU_EPRST     equ APU + 0x4000
NV_PAPU_EPISTS    equ APU + 0x4004
NV_PAPU_EPSADDR   equ APU + 0x4008

; === 1. ISTS = 0 on init (no pending interrupts) ===
    mov eax, [NV_PAPU_ISTS]
    ASSERT_EQ eax, 0

; === 2. IEN write/read ===
    mov dword [NV_PAPU_IEN], 0x0000003F
    mov eax, [NV_PAPU_IEN]
    ASSERT_EQ eax, 0x0000003F

; === 3. FECTL write/read ===
    mov dword [NV_PAPU_FECTL], 0x00000001
    mov eax, [NV_PAPU_FECTL]
    ASSERT_EQ eax, 0x00000001

; === 4. FESTATE = 0 (idle) on init ===
    mov eax, [NV_PAPU_FESTATE]
    ASSERT_EQ eax, 0

; === 5. FESTATUS = 0 (not busy) on init ===
    mov eax, [NV_PAPU_FESTATUS]
    ASSERT_EQ eax, 0

; === 6. SESTATUS = 0 (idle) on init ===
    mov eax, [NV_PAPU_SESTATUS]
    ASSERT_EQ eax, 0

; === 7. GPRST write/read ===
    mov dword [NV_PAPU_GPRST], 0x00000001
    mov eax, [NV_PAPU_GPRST]
    ASSERT_EQ eax, 0x00000001

; === 8. GPSADDR write/read ===
    mov dword [NV_PAPU_GPSADDR], 0x00100000
    mov eax, [NV_PAPU_GPSADDR]
    ASSERT_EQ eax, 0x00100000

; === 9. EPRST write/read ===
    mov dword [NV_PAPU_EPRST], 0x00000001
    mov eax, [NV_PAPU_EPRST]
    ASSERT_EQ eax, 0x00000001

; === 10. EPSADDR write/read ===
    mov dword [NV_PAPU_EPSADDR], 0x00200000
    mov eax, [NV_PAPU_EPSADDR]
    ASSERT_EQ eax, 0x00200000

; === 11. ISTS W1C behavior ===
    ; Write a known value via FESTATE (which writes to festate, not ists),
    ; so instead: directly test the W1C semantics.
    ; Set IEN to enable, then manually check that writing 1s to ISTS clears bits.
    ; Since ISTS starts at 0, write 0xFFFFFFFF should leave it at 0 (clearing zeros = 0).
    mov dword [NV_PAPU_ISTS], 0xFFFFFFFF
    mov eax, [NV_PAPU_ISTS]
    ASSERT_EQ eax, 0

; === 12. SECTL write/read ===
    mov dword [NV_PAPU_SECTL], 0x00000002
    mov eax, [NV_PAPU_SECTL]
    ASSERT_EQ eax, 0x00000002

    PASS
