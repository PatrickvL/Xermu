; ===========================================================================
; apu.asm — MCPX APU register tests (--xbox mode).
;
; Tests the Xbox audio processing unit MMIO stub at 0xFE800000:
;   1. ISTS reads 0 on init (no pending interrupts)
;   2. IEN write/read
;   3. FECTL write/read
;   4. FESTATE reads 0 (idle) on init
;   5. FESTATUS reads 0 (not busy) on init
;   6. XGSCNT reads 0 on init
;   7. GPRST write/read
;   8. GPSADDR write/read
;   9. EPRST write/read
;  10. EPSADDR write/read
;  11. ISTS W1C behavior
;  12. SECTL write/read
;  13. FETFORCE1 reports SE2FE_IDLE_VOICE (bit 7) on read
;  14. VPVADDR write/read (voice processor voice list base)
;  15. VPSGEADDR write/read (scatter-gather base)
;  16. TVL2D/NVL2D voice list descriptor write/read
;  17. GPSMAXSGE write/read
;  18. GP output FIFO base/end write/read (channel 0)
;  19. EP output FIFO base/end write/read
;  20. GPFADDR write/read (GP frame address)
; ===========================================================================

%include "harness.inc"

ORG 0x1000

APU equ 0xFE800000

; General APU registers
NV_PAPU_ISTS      equ APU + 0x1000
NV_PAPU_IEN       equ APU + 0x1004
NV_PAPU_FECTL     equ APU + 0x1100
NV_PAPU_FECV      equ APU + 0x1110
NV_PAPU_FESTATE   equ APU + 0x1108
NV_PAPU_FESTATUS  equ APU + 0x110C
NV_PAPU_FETFORCE0 equ APU + 0x1500
NV_PAPU_FETFORCE1 equ APU + 0x1504

; Setup engine
NV_PAPU_SECTL     equ APU + 0x2000
NV_PAPU_XGSCNT    equ APU + 0x200C
NV_PAPU_VPVADDR   equ APU + 0x202C
NV_PAPU_VPSGEADDR equ APU + 0x2030
NV_PAPU_VPSSLADDR equ APU + 0x2034

; Voice list descriptors
NV_PAPU_TVL2D     equ APU + 0x2054
NV_PAPU_NVL2D     equ APU + 0x205C

; SGE limits
NV_PAPU_GPSMAXSGE equ APU + 0x20D4

; Global Processor
NV_PAPU_GPRST     equ APU + 0x3000
NV_PAPU_GPISTS    equ APU + 0x3004
NV_PAPU_GPSADDR   equ APU + 0x2040
NV_PAPU_GPFADDR   equ APU + 0x2044
NV_PAPU_GPOFBASE0 equ APU + 0x3024
NV_PAPU_GPOFEND0  equ APU + 0x3028

; Encode Processor
NV_PAPU_EPRST     equ APU + 0x4000
NV_PAPU_EPISTS    equ APU + 0x4004
NV_PAPU_EPSADDR   equ APU + 0x2048
NV_PAPU_EPOFBASE0 equ APU + 0x4024
NV_PAPU_EPOFEND0  equ APU + 0x4028

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

; === 6. XGSCNT = 0 on init ===
    mov eax, [NV_PAPU_XGSCNT]
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

; === 13. FETFORCE1 reports SE2FE_IDLE_VOICE (bit 7) on read ===
    ; Without writing anything, FETFORCE1 should have bit 7 set (idle)
    mov eax, [NV_PAPU_FETFORCE1]
    and eax, 0x80               ; isolate bit 7
    ASSERT_EQ eax, 0x80

; === 14. VPVADDR write/read ===
    mov dword [NV_PAPU_VPVADDR], 0x00400000
    mov eax, [NV_PAPU_VPVADDR]
    ASSERT_EQ eax, 0x00400000

; === 15. VPSGEADDR write/read ===
    mov dword [NV_PAPU_VPSGEADDR], 0x00500000
    mov eax, [NV_PAPU_VPSGEADDR]
    ASSERT_EQ eax, 0x00500000

; === 16. TVL2D / NVL2D voice list write/read ===
    mov dword [NV_PAPU_TVL2D], 0x00001234
    mov eax, [NV_PAPU_TVL2D]
    ASSERT_EQ eax, 0x00001234
    mov dword [NV_PAPU_NVL2D], 0x00000010
    mov eax, [NV_PAPU_NVL2D]
    ASSERT_EQ eax, 0x00000010

; === 17. GPSMAXSGE write/read ===
    mov dword [NV_PAPU_GPSMAXSGE], 0x000000FF
    mov eax, [NV_PAPU_GPSMAXSGE]
    ASSERT_EQ eax, 0x000000FF

; === 18. GP output FIFO base/end write/read (channel 0) ===
    mov dword [NV_PAPU_GPOFBASE0], 0x00300000
    mov eax, [NV_PAPU_GPOFBASE0]
    ASSERT_EQ eax, 0x00300000
    mov dword [NV_PAPU_GPOFEND0], 0x00301000
    mov eax, [NV_PAPU_GPOFEND0]
    ASSERT_EQ eax, 0x00301000

; === 19. EP output FIFO base/end write/read ===
    mov dword [NV_PAPU_EPOFBASE0], 0x00600000
    mov eax, [NV_PAPU_EPOFBASE0]
    ASSERT_EQ eax, 0x00600000
    mov dword [NV_PAPU_EPOFEND0], 0x00601000
    mov eax, [NV_PAPU_EPOFEND0]
    ASSERT_EQ eax, 0x00601000

; === 20. GPFADDR write/read (GP frame address) ===
    mov dword [NV_PAPU_GPFADDR], 0x00700000
    mov eax, [NV_PAPU_GPFADDR]
    ASSERT_EQ eax, 0x00700000

    PASS
