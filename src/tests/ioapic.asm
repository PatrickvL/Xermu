; ===========================================================================
; ioapic.asm — I/O APIC register tests (--xbox mode).
;
; Tests indirect register access via IOREGSEL (0x00) / IOWIN (0x10):
;   1. IOREGSEL write/readback
;   2. IOAPIC_VER = 0x00170011 (version 0x11, 24 entries)
;   3. IOAPIC_ID default = 0
;   4. IOAPIC_ID write (bits [27:24]) / readback
;   5. IOAPIC_ARB default = 0
;   6. IOAPIC_VER is read-only (write ignored)
;   7. Redir entry 0 low default = 0x00010000 (masked)
;   8. Redir entry 0 low write/read (mask + vector)
;   9. Redir entry 0 high write/read (destination)
;  10. Redir entry 23 low default = 0x00010000 (masked)
;  11. RO bits preserved (delivery_status, remote_irr)
;  12. Redir entry high only [31:24] writable
; ===========================================================================

%include "harness.inc"

ORG 0x1000

IOAPIC_SEL equ 0xFEC00000
IOAPIC_WIN equ 0xFEC00010

; === 1. IOREGSEL write/readback ===
    mov dword [IOAPIC_SEL], 0x01
    mov eax, [IOAPIC_SEL]
    ASSERT_EQ eax, 0x01

; === 2. IOAPIC_VER = 0x00170011 ===
    ; IOREGSEL still 0x01 from above
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00170011

; === 3. IOAPIC_ID default = 0 ===
    mov dword [IOAPIC_SEL], 0x00
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00000000

; === 4. IOAPIC_ID write (bits [27:24]) / readback ===
    mov dword [IOAPIC_WIN], 0x0F000000
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x0F000000

; === 5. IOAPIC_ARB default = 0 ===
    mov dword [IOAPIC_SEL], 0x02
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00000000

; === 6. IOAPIC_VER is read-only ===
    mov dword [IOAPIC_SEL], 0x01
    mov dword [IOAPIC_WIN], 0xDEADBEEF
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00170011

; === 7. Redir entry 0 low default = masked ===
    mov dword [IOAPIC_SEL], 0x10       ; REDIR_BASE + 0*2
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00010000

; === 8. Redir entry 0 low write/read ===
    ; Write vector=0x42, edge trigger, unmasked
    mov dword [IOAPIC_WIN], 0x00000042
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00000042

; === 9. Redir entry 0 high write/read (destination) ===
    mov dword [IOAPIC_SEL], 0x11       ; REDIR_BASE + 0*2 + 1
    mov dword [IOAPIC_WIN], 0xFF000000
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0xFF000000

; === 10. Redir entry 23 low default = masked ===
    mov dword [IOAPIC_SEL], 0x3E       ; REDIR_BASE + 23*2 = 0x10 + 46
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00010000

; === 11. RO bits preserved (delivery_status bit 12, remote_irr bit 14) ===
    ; Redir 0 low currently 0x00000042 from test 8.
    ; Try writing with RO bits set — they should be masked off.
    mov dword [IOAPIC_SEL], 0x10
    mov dword [IOAPIC_WIN], 0x00005042  ; bits 12+14 set
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0x00000042          ; RO bits cleared

; === 12. Redir entry high only [31:24] writable ===
    mov dword [IOAPIC_SEL], 0x11
    mov dword [IOAPIC_WIN], 0xFFFFFFFF
    mov eax, [IOAPIC_WIN]
    ASSERT_EQ eax, 0xFF000000

    PASS
