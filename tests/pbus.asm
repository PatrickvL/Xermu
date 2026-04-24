; ===========================================================================
; pbus.asm — NV2A PBUS register tests (--xbox mode).
;
; Tests PBUS registers at NV2A + 0x001000:
;   1. PBUS_INTR default = 0
;   2. PBUS_INTR_EN write/read
;   3. PBUS_INTR W1C behavior
;   4. PBUS_REG_0 write/read
;   5. PBUS_FBIO_RAM default = 0x03 (DDR)
;   6. PBUS_FBIO_RAM write/read
;   7. PBUS_DEBUG_1 write/read
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A equ 0xFD000000
PB   equ NV2A + 0x001000

; === 1. PBUS_INTR default = 0 ===
    mov eax, [PB + 0x100]
    ASSERT_EQ eax, 0

; === 2. PBUS_INTR_EN write/read ===
    mov dword [PB + 0x140], 0x00000001
    mov eax, [PB + 0x140]
    ASSERT_EQ eax, 0x00000001

; === 3. PBUS_INTR W1C ===
    ; INTR is currently 0; writing 1 to W1C clears nothing (stays 0)
    mov dword [PB + 0x100], 0x01
    mov eax, [PB + 0x100]
    ASSERT_EQ eax, 0

; === 4. PBUS_REG_0 write/read ===
    mov dword [PB + 0x200], 0xDEADBEEF
    mov eax, [PB + 0x200]
    ASSERT_EQ eax, 0xDEADBEEF

; === 5. PBUS_FBIO_RAM default = DDR ===
    mov eax, [PB + 0x218]
    ASSERT_EQ eax, 0x00000003

; === 6. PBUS_FBIO_RAM write/read ===
    mov dword [PB + 0x218], 0x00000007
    mov eax, [PB + 0x218]
    ASSERT_EQ eax, 0x00000007

; === 7. PBUS_DEBUG_1 write/read ===
    mov dword [PB + 0x084], 0x12345678
    mov eax, [PB + 0x084]
    ASSERT_EQ eax, 0x12345678

    PASS
