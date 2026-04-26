; ===========================================================================
; ide.asm — IDE (ATA) controller tests (--xbox mode).
;
; Tests the dual-channel ATA controller:
;   Primary (0x1F0–0x1F7, 0x3F6): HDD on master
;   Secondary (0x170–0x177, 0x376): DVD on master
;
;   1. Primary status = 0x50 (DRDY|DSC) on init
;   2. Secondary status = 0x50 on init
;   3. Primary error = 0x01 on init (no error)
;   4. Primary sector count write/read
;   5. Primary LBA low write/read
;   6. Primary LBA mid write/read
;   7. Primary LBA high write/read
;   8. Primary device/head write/read
;   9. IDENTIFY DEVICE sets DRQ (status = 0x58)
;  10. IDENTIFY data port returns identify[0] (word 0 = 0x0040)
;  11. IDENTIFY drains 256 words, status clears DRQ
;  12. Secondary IDENTIFY PACKET DEVICE sets DRQ
;  13. SET FEATURES succeeds (status = 0x50)
;  14. Unknown command → error (status bit 0 set)
;  15. Alternate status (0x3F6) matches status (0x1F7)
;  16. Software reset via control register
;  17. Secondary ATAPI signature (LBA_MID=0x14, LBA_HI=0xEB)
;  18. ATAPI PACKET command → DRQ for packet data
;  19. INQUIRY packet → CD-ROM device type (0x05)
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; Primary channel (HDD)
PRI_DATA      equ 0x1F0
PRI_ERROR     equ 0x1F1
PRI_FEATURES  equ 0x1F1
PRI_SECT_CNT  equ 0x1F2
PRI_LBA_LO    equ 0x1F3
PRI_LBA_MI    equ 0x1F4
PRI_LBA_HI    equ 0x1F5
PRI_DEVICE    equ 0x1F6
PRI_STATUS    equ 0x1F7
PRI_COMMAND   equ 0x1F7
PRI_CONTROL   equ 0x3F6
PRI_ALT_STAT  equ 0x3F6

; Secondary channel (DVD)
SEC_DATA      equ 0x170
SEC_SECT_CNT  equ 0x172
SEC_LBA_MI    equ 0x174
SEC_LBA_HI    equ 0x175
SEC_STATUS    equ 0x177
SEC_COMMAND   equ 0x177
SEC_ALT_STAT  equ 0x376

; === 1. Primary status = 0x50 (DRDY|DSC) on init ===
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x50

; === 2. Secondary status = 0x50 on init ===
    mov dx, SEC_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x50

; === 3. Primary error = 0x01 on init ===
    mov dx, PRI_ERROR
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x01

; === 4. Primary sector count write/read ===
    mov dx, PRI_SECT_CNT
    mov al, 0x42
    out dx, al
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x42

; === 5. Primary LBA low write/read ===
    mov dx, PRI_LBA_LO
    mov al, 0xAB
    out dx, al
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xAB

; === 6. Primary LBA mid write/read ===
    mov dx, PRI_LBA_MI
    mov al, 0xCD
    out dx, al
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xCD

; === 7. Primary LBA high write/read ===
    mov dx, PRI_LBA_HI
    mov al, 0xEF
    out dx, al
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xEF

; === 8. Primary device/head write/read ===
    mov dx, PRI_DEVICE
    mov al, 0xE0          ; LBA mode, master
    out dx, al
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xE0

; === 9. IDENTIFY DEVICE (0xEC) sets DRQ ===
    mov dx, PRI_COMMAND
    mov al, 0xEC
    out dx, al
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x58     ; DRDY | DSC | DRQ

; === 10. IDENTIFY data port returns word 0 = 0x0040 ===
    mov dx, PRI_DATA
    in  ax, dx              ; 16-bit read of first identify word
    movzx eax, ax
    ASSERT_EQ eax, 0x0040   ; HDD general config word

; === 11. Drain remaining 255 words, status clears DRQ ===
    mov ecx, 255
.drain_id:
    mov dx, PRI_DATA
    in  ax, dx              ; discard
    dec ecx
    jnz .drain_id
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x50     ; DRDY | DSC (no DRQ — transfer complete)

; === 12. Secondary IDENTIFY PACKET DEVICE (0xA1) sets DRQ ===
    mov dx, SEC_COMMAND
    mov al, 0xA1
    out dx, al
    mov dx, SEC_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x58     ; DRDY | DSC | DRQ

; === 13. SET FEATURES (0xEF) succeeds ===
    mov dx, PRI_COMMAND
    mov al, 0xEF
    out dx, al
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x50     ; DRDY | DSC (no DRQ, no ERR)

; === 14. Unknown command → error ===
    mov dx, PRI_COMMAND
    mov al, 0xFF            ; bogus command
    out dx, al
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x51     ; DRDY | ERR

; === 15. Alternate status matches status ===
    ; After the unknown command, primary status = 0x51.
    mov dx, PRI_ALT_STAT
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x51

; === 16. Software reset via control register ===
    mov dx, PRI_CONTROL
    mov al, 0x04            ; SRST bit
    out dx, al
    mov al, 0x00            ; clear SRST
    out dx, al
    mov dx, PRI_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x50     ; back to DRDY|DSC after reset

; === 17. Secondary ATAPI signature (LBA_MID=0x14, LBA_HI=0xEB) ===
    mov dx, SEC_LBA_MI
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x14
    mov dx, SEC_LBA_HI
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0xEB

; === 18. ATAPI PACKET cmd → DRQ for packet data ===
    ; First drain the leftover IDENTIFY PACKET DEVICE from test 12
    mov ecx, 256
.drain_sec:
    mov dx, SEC_DATA
    in  ax, dx
    dec ecx
    jnz .drain_sec

    mov dx, SEC_COMMAND
    mov al, 0xA0            ; PACKET command
    out dx, al
    mov dx, SEC_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x58     ; DRQ — ready for 12-byte packet

; === 19. Send INQUIRY packet (0x12), get device type = 0x05 (CD-ROM) ===
    ; Send 12-byte INQUIRY packet: opcode=0x12, alloc_len=36
    mov dx, SEC_DATA
    mov ax, 0x0012          ; word 0: opcode=0x12, page=0x00
    out dx, ax
    mov ax, 0x0000          ; word 1
    out dx, ax
    mov ax, 0x2400          ; word 2: alloc_len=0x24 (36) in high byte
    out dx, ax
    mov ax, 0x0000          ; word 3
    out dx, ax
    mov ax, 0x0000          ; word 4
    out dx, ax
    mov ax, 0x0000          ; word 5
    out dx, ax

    ; After sending full packet, drive should have data ready
    mov dx, SEC_STATUS
    in  al, dx
    movzx eax, al
    ASSERT_EQ eax, 0x58     ; DRQ — data ready

    ; Read first word of INQUIRY data: byte 0 = device type (0x05), byte 1 = 0x80 (removable)
    mov dx, SEC_DATA
    in  ax, dx
    movzx eax, ax
    ASSERT_EQ eax, 0x8005   ; 0x05 (CD-ROM) | 0x80<<8 (removable)

    PASS
