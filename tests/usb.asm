; ===========================================================================
; usb.asm — USB OHCI controller register stub tests (--xbox mode).
;
; Tests:
;   1. USB0 HcRevision = 0x0110 (OHCI 1.1)
;   2. USB0 HcControl initial = 0 (USBReset)
;   3. USB0 HcRhDescriptorA NDP = 2 (2 downstream ports)
;   4. USB0 HcFmInterval = default (0x27782EDF)
;   5. USB1 HcRevision = 0x0110
;   6. USB1 HcRhDescriptorA NDP = 2
;   7. USB0 HcCommandStatus reset: controller re-inits
;   8. USB0 HcInterruptStatus W1C
;   9. USB0 HcInterruptEnable / HcInterruptDisable
;  10. PCI config: USB0 vendor ID = 0x10DE
;  11. PCI config: USB0 device ID = 0x01C2
;  12. PCI config: USB0 BAR0 = 0xFED00000
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; USB0 base = 0xFED00000, USB1 base = 0xFED08000

; === 1. USB0 HcRevision ===
    mov eax, [0xFED00000]       ; HcRevision
    ASSERT_EQ eax, 0x00000110

; === 2. USB0 HcControl initial ===
    mov eax, [0xFED00004]       ; HcControl
    ASSERT_EQ eax, 0x00000000

; === 3. USB0 HcRhDescriptorA NDP ===
    mov eax, [0xFED00048]       ; HcRhDescriptorA
    and eax, 0xFF               ; NDP = low byte
    ASSERT_EQ eax, 2

; === 4. USB0 HcFmInterval ===
    mov eax, [0xFED00038]       ; HcFmInterval
    ASSERT_EQ eax, 0x27782EDF

; === 5. USB1 HcRevision ===
    mov eax, [0xFED08000]       ; USB1 HcRevision
    ASSERT_EQ eax, 0x00000110

; === 6. USB1 HcRhDescriptorA NDP ===
    mov eax, [0xFED08048]       ; USB1 HcRhDescriptorA
    and eax, 0xFF
    ASSERT_EQ eax, 2

; === 7. USB0 HcCommandStatus reset ===
    ; Write a known value to HcControl, then reset via HcCommandStatus bit 0.
    mov dword [0xFED00004], 0x00000080  ; set HCFS=operational
    mov dword [0xFED00008], 0x00000001  ; HostControllerReset
    mov eax, [0xFED00004]               ; HcControl should be 0 after reset
    ASSERT_EQ eax, 0x00000000

; === 8. USB0 HcInterruptStatus W1C ===
    ; Write some bits to HcInterruptStatus (which resets to 0), then clear them.
    ; First set a bit via direct write (simulate hardware setting it).
    mov dword [0xFED0000C], 0x00000004  ; set bit 2 (SF)
    ; It was 0 and W1C means writing 1 clears: 0 & ~4 = 0, still 0.
    mov eax, [0xFED0000C]
    ASSERT_EQ eax, 0x00000000           ; should be 0 (W1C of 0 = 0)

; === 9. USB0 HcInterruptEnable / Disable ===
    mov dword [0xFED00010], 0x0000003F  ; enable bits
    mov eax, [0xFED00010]
    ASSERT_EQ eax, 0x0000003F
    mov dword [0xFED00014], 0x0000000A  ; disable bits 1,3
    mov eax, [0xFED00010]
    ASSERT_EQ eax, 0x00000035           ; 0x3F & ~0x0A = 0x35

; === 10. PCI config: USB0 vendor ID ===
    ; PCI config address for Bus 0, Dev 4, Fn 0, offset 0
    ;   bit 31 = enable, bits 23:16 = bus, bits 15:11 = dev, bits 10:8 = fn, bits 7:0 = off
    ;   Dev 4 = 4 << 11 = 0x2000
    mov dx, 0xCF8
    mov eax, 0x80002000
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    and eax, 0xFFFF                     ; vendor ID = low 16 bits
    ASSERT_EQ eax, 0x10DE

; === 11. PCI config: USB0 device ID ===
    mov dx, 0xCF8
    mov eax, 0x80002000
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    shr eax, 16                         ; device ID = high 16 bits
    ASSERT_EQ eax, 0x01C2

; === 12. PCI config: USB0 BAR0 ===
    mov dx, 0xCF8
    mov eax, 0x80002010                 ; offset 0x10 = BAR0
    out dx, eax
    mov dx, 0xCFC
    in eax, dx
    ASSERT_EQ eax, 0xFED00000

; === PASS ===
    xor eax, eax
    hlt
