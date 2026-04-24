; ===========================================================================
; pci.asm — PCI configuration space tests (--xbox mode).
;
; Tests PCI CF8/CFC access for Xbox device table:
;   1. Host Bridge: vendor 0x10DE, device 0x02A5
;   2. LPC Bridge: vendor 0x10DE, device 0x01B2
;   3. SMBus: vendor 0x10DE, device 0x01B4
;   4. SMBus BAR1 = 0xC001 (I/O)
;   5. NV2A GPU: vendor 0x10DE, device 0x02A0
;   6. NV2A BAR0 = 0xFD000000
;   7. APU: vendor 0x10DE, device 0x01B0
;   8. APU BAR0 = 0xFE800000
;   9. USB0 BAR0 = 0xFED00000
;  10. USB1 BAR0 = 0xFED08000
;  11. IDE: vendor 0x10DE, device 0x01BC
;  12. IDE IRQ line = 14
;  13. AGP bridge: header type 0x01
;  14. AGP bridge: secondary bus = 1
;  15. Bus 1 Dev 0: NV2A BAR0 = 0xFD000000
;  16. Non-existent device returns 0xFFFF
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; Helper: read PCI config dword.
; Input: EAX = config address (with enable bit 31 set)
; Output: EAX = dword read from CFC
%macro PCI_READ 1
    mov eax, %1
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in  eax, dx
%endmacro

; === 1. Host Bridge: vendor/device ===
    PCI_READ 0x80000000       ; bus 0, dev 0, fn 0, reg 0
    mov ecx, eax
    and eax, 0xFFFF
    ASSERT_EQ eax, 0x10DE
    shr ecx, 16
    ASSERT_EQ ecx, 0x02A5

; === 3. SMBus: vendor/device ===
    PCI_READ 0x80000900       ; bus 0, dev 1, fn 1, reg 0
    and eax, 0xFFFF
    ASSERT_EQ eax, 0x10DE

; === 4. SMBus BAR1 ===
    PCI_READ 0x80000914       ; bus 0, dev 1, fn 1, offset 0x14
    and eax, 0xFFFF
    ASSERT_EQ eax, 0xC001

; === 5. NV2A GPU: vendor/device ===
    PCI_READ 0x80001000       ; bus 0, dev 2, fn 0, reg 0
    mov ecx, eax
    and eax, 0xFFFF
    ASSERT_EQ eax, 0x10DE
    shr ecx, 16
    ASSERT_EQ ecx, 0x02A0

; === 6. NV2A BAR0 ===
    PCI_READ 0x80001010       ; bus 0, dev 2, fn 0, offset 0x10
    ASSERT_EQ eax, 0xFD000000

; === 7. APU: vendor/device ===
    PCI_READ 0x80001800       ; bus 0, dev 3, fn 0, reg 0
    and eax, 0xFFFF
    ASSERT_EQ eax, 0x10DE

; === 8. APU BAR0 ===
    PCI_READ 0x80001810       ; bus 0, dev 3, fn 0, offset 0x10
    ASSERT_EQ eax, 0xFE800000

; === 9. USB0 BAR0 ===
    PCI_READ 0x80002010       ; bus 0, dev 4, fn 0, offset 0x10
    ASSERT_EQ eax, 0xFED00000

; === 10. USB1 BAR0 ===
    PCI_READ 0x80002810       ; bus 0, dev 5, fn 0, offset 0x10
    ASSERT_EQ eax, 0xFED08000

; === 11. IDE: vendor/device ===
    PCI_READ 0x80004800       ; bus 0, dev 9, fn 0, reg 0
    mov ecx, eax
    and eax, 0xFFFF
    ASSERT_EQ eax, 0x10DE
    shr ecx, 16
    ASSERT_EQ ecx, 0x01BC

; === 12. IDE IRQ line ===
    PCI_READ 0x8000483C       ; bus 0, dev 9, fn 0, offset 0x3C
    and eax, 0xFF
    ASSERT_EQ eax, 14

; === 13. AGP bridge header type ===
    PCI_READ 0x8000F00C       ; bus 0, dev 30, fn 0, offset 0x0C
    shr eax, 16               ; bytes 0x0E-0x0F in high 16; header type = bits [23:16]
    and eax, 0xFF
    ASSERT_EQ eax, 0x01

; === 14. AGP bridge secondary bus ===
    PCI_READ 0x8000F018       ; bus 0, dev 30, fn 0, offset 0x18
    shr eax, 8                ; secondary bus at byte 0x19
    and eax, 0xFF
    ASSERT_EQ eax, 1

; === 15. Bus 1 Dev 0 NV2A BAR0 ===
    PCI_READ 0x80010010       ; bus 1, dev 0, fn 0, offset 0x10
    ASSERT_EQ eax, 0xFD000000

; === 16. Non-existent device returns 0xFFFF ===
    PCI_READ 0x80007000       ; bus 0, dev 14, fn 0 (no device)
    and eax, 0xFFFF
    ASSERT_EQ eax, 0xFFFF

    PASS
