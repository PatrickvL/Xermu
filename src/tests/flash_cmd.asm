; ===========================================================================
; flash_cmd.asm — Flash ROM command interface tests (--xbox mode).
;
; Tests SST49LF040-compatible CFI command interface:
;   1. Default read returns flash data (0xFF for erased)
;   2. Read status returns 0x80 (ready)
;   3. Read ID mode: manufacturer = 0xBF (SST)
;   4. Read ID mode: device = 0x52 (SST49LF040)
;   5. Return to read array mode after ID read
;   6. Clear status command resets to ready
;   7. Byte program: write + readback
;   8. Flash program only clears bits (1->0)
;   9. Sector erase: 4KB sector filled with 0xFF
;  10. Status after erase = ready
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; Flash is at 0xFF000000 (BIOS alias). We'll use a high offset to avoid
; conflicting with any loaded test code.
FLASH equ 0xFF0F0000

; === 1. Default read = 0xFF (erased flash) ===
    mov eax, [FLASH]
    and eax, 0xFF
    ASSERT_EQ eax, 0xFF

; === 2. Read status register ===
    mov byte [FLASH], 0x70         ; READ_STATUS command
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0x80            ; SR_READY

; Return to read array mode
    mov byte [FLASH], 0xFF

; === 3. Read ID: manufacturer ===
    mov byte [FLASH], 0x90         ; READ_ID command
    ; Read offset 0 in the sector → manufacturer
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0xBF            ; SST

; === 4. Read ID: device ===
    mov al, [FLASH + 1]
    movzx eax, al
    ASSERT_EQ eax, 0x52            ; SST49LF040

; === 5. Return to read array ===
    mov byte [FLASH], 0xFF         ; READ_ARRAY
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0xFF            ; back to normal data

; === 6. Clear status ===
    mov byte [FLASH], 0x70         ; read status mode
    mov byte [FLASH], 0x50         ; clear status
    ; After clear, mode goes back to read array and status = ready
    mov byte [FLASH], 0x70         ; read status again
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0x80

    mov byte [FLASH], 0xFF         ; back to array

; === 7. Byte program: write 0x42 ===
    mov byte [FLASH], 0x40         ; BYTE_PROGRAM command
    mov byte [FLASH], 0x42         ; data byte (0xFF & 0x42 = 0x42)
    ; Read back
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0x42

; === 8. Flash can only clear bits ===
    ; Current value at FLASH is 0x42. Program 0x40 (clears bit 1).
    mov byte [FLASH], 0x40         ; BYTE_PROGRAM
    mov byte [FLASH], 0x40         ; data: 0x42 & 0x40 = 0x40
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0x40

; === 9. Sector erase ===
    mov byte [FLASH], 0x20         ; ERASE_SETUP
    mov byte [FLASH], 0xD0         ; ERASE_CONFIRM
    ; Flash at FLASH should be 0xFF after erase
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0xFF

; === 10. Status after erase = ready ===
    mov byte [FLASH], 0x70
    mov al, [FLASH]
    movzx eax, al
    ASSERT_EQ eax, 0x80

    mov byte [FLASH], 0xFF

    PASS
