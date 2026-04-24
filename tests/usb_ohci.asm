; ===========================================================================
; usb_ohci.asm — USB OHCI frame counter + namespace tests (--xbox mode).
;
; Tests expanded OHCI register emulation:
;   1. HcFmNumber starts at 0
;   2. HcFmInterval = 0x27782EDF (OHCI 1.1 default)
;   3. HcFmRemaining reads ~0x2710
;   4. Set operational mode (HcControl HCFS=0x80)
;   5. HcPeriodicStart write + readback
;   6. HCCA pointer write + readback
;   7. Control list head ED write + readback
;   8. Bulk list head ED write + readback
;   9. Interrupt enable: MIE + specific bits
;  10. Interrupt disable clears enable
; ===========================================================================

%include "harness.inc"

ORG 0x1000

USB0 equ 0xFED00000

; === 1. HcFmNumber starts at 0 ===
    mov eax, [USB0 + 0x3C]         ; FM_NUMBER
    ASSERT_EQ eax, 0

; === 2. HcFmInterval default ===
    mov eax, [USB0 + 0x34]         ; FM_INTERVAL
    ASSERT_EQ eax, 0x27782EDF

; === 3. HcFmRemaining reads ~0x2710 ===
    mov eax, [USB0 + 0x38]         ; FM_REMAINING
    ASSERT_EQ eax, 0x2710

; === 4. Set operational mode ===
    mov dword [USB0 + 0x04], 0x80  ; HCFS = Operational
    mov eax, [USB0 + 0x04]
    and eax, 0xC0
    ASSERT_EQ eax, 0x80

; === 5. HcPeriodicStart ===
    mov dword [USB0 + 0x40], 0x00002A2F
    mov eax, [USB0 + 0x40]
    ASSERT_EQ eax, 0x00002A2F

; === 6. HCCA pointer ===
    mov dword [USB0 + 0x18], 0x10000000
    mov eax, [USB0 + 0x18]
    ASSERT_EQ eax, 0x10000000

; === 7. Control head ED ===
    mov dword [USB0 + 0x20], 0xABCD0000
    mov eax, [USB0 + 0x20]
    ASSERT_EQ eax, 0xABCD0000

; === 8. Bulk head ED ===
    mov dword [USB0 + 0x28], 0x12340000
    mov eax, [USB0 + 0x28]
    ASSERT_EQ eax, 0x12340000

; === 9. Interrupt enable ===
    mov dword [USB0 + 0x10], 0x8000003F  ; MIE + SO+WDH+SF+RD+FNO+RHSC
    mov eax, [USB0 + 0x10]
    ASSERT_EQ eax, 0x8000003F

; === 10. Interrupt disable ===
    mov dword [USB0 + 0x14], 0x0000000A  ; disable WDH+RD
    mov eax, [USB0 + 0x10]               ; read enable
    ASSERT_EQ eax, 0x80000035            ; 0x3F & ~0x0A = 0x35, MIE intact

    PASS
