; ===========================================================================
; xbox.asm — Xbox physical address map tests (--xbox mode).
;
; Tests:
;   1. RAM mirror: write to main RAM, read from mirror at 0x0C000000
;   2. RAM mirror: write via mirror, read from main RAM
;   3. PCI config: read Host Bridge vendor ID (0x10DE)
;   4. PCI config: read Host Bridge device ID (0x02A5)
;   5. PCI config: read NV2A vendor ID (0x10DE) at bus 0, dev 2
;   6. NV2A MMIO: read PMC_BOOT_0 (chip ID)
;   7. Flash ROM: reads 0xFF (empty flash)
;   8. I/O APIC: write IOREGSEL, read back
;   9. SMBus: read status register (should be 0 initially)
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; === 1. RAM mirror: write to main RAM at PA 0x2000, read from mirror ===
    mov dword [0x2000], 0xCAFEBABE
    mov eax, [0x0C002000]       ; mirror of PA 0x2000
    ASSERT_EQ eax, 0xCAFEBABE

; === 2. RAM mirror: write via mirror, read from main RAM ===
    mov dword [0x0C003000], 0x12345678
    mov eax, [0x3000]           ; should see it in main RAM
    ASSERT_EQ eax, 0x12345678

; === 3. PCI config: read Host Bridge vendor ID ===
; Write config address: enable=1, bus=0, dev=0, fn=0, offset=0x00
    mov eax, 0x80000000         ; enable | bus=0 | dev=0 | fn=0 | reg=0x00
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in  eax, dx                 ; read 32 bits: [device_id:16 | vendor_id:16]
    mov ecx, eax                ; save full dword
    and eax, 0x0000FFFF         ; vendor ID in low 16 bits
    ASSERT_EQ eax, 0x10DE       ; NVIDIA

; === 4. PCI config: read Host Bridge device ID ===
    shr ecx, 16                 ; device ID in high 16 bits
    ASSERT_EQ ecx, 0x02A5       ; MCPX Host Bridge

; === 5. PCI config: read NV2A (bus 0, dev 2, fn 0) vendor ID ===
    mov eax, 0x80001000         ; enable | bus=0 | dev=2 | fn=0 | reg=0x00
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in  eax, dx
    and eax, 0x0000FFFF
    ASSERT_EQ eax, 0x10DE       ; NVIDIA NV2A

; === 6. NV2A MMIO: read PMC_BOOT_0 at 0xFD000000 ===
    mov eax, [0xFD000000]
    ASSERT_EQ eax, 0x02A000A1

; === 7. Flash ROM: read from 0xF0000000 (should be 0xFF empty) ===
    mov eax, [0xF0000000]
    ASSERT_EQ eax, 0xFFFFFFFF

; === 8. I/O APIC: write IOREGSEL (offset 0x00), read it back ===
    mov dword [0xFEC00000], 0x01   ; select register 1 (IOAPIC_VER)
    mov eax, [0xFEC00000]          ; read IOREGSEL back
    ASSERT_EQ eax, 0x01

; === 9. SMBus: read status register (port 0xC000) ===
    mov dx, 0xC000
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0               ; initial status = 0

    PASS
