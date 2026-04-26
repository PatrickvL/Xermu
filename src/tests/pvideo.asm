; ===========================================================================
; pvideo.asm — NV2A PVIDEO (video overlay) register tests (--xbox mode).
;
; Tests PVIDEO registers at NV2A + 0x008000:
;   1. PVIDEO_INTR default = 0
;   2. PVIDEO_INTR_EN write/read
;   3. PVIDEO_BUFFER write/read (bit 4 = active buffer select)
;   4. PVIDEO_STOP write/read
;   5. PVIDEO_BASE0 write/read
;   6. PVIDEO_BASE1 write/read
;   7. PVIDEO_LIMIT0 write/read
;   8. PVIDEO_OFFSET0 write/read
;   9. PVIDEO_SIZE_IN0 write/read
;  10. PVIDEO_DS_DX0 write/read (horizontal scale factor)
;  11. PVIDEO_DT_DY0 write/read (vertical scale factor)
;  12. PVIDEO_POINT_OUT0 write/read (output position)
;  13. PVIDEO_SIZE_OUT0 write/read (output size)
;  14. PVIDEO_FORMAT0 write/read (color format + pitch)
;  15. PVIDEO_INTR W1C behavior
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A   equ 0xFD000000
PV     equ NV2A + 0x008000    ; PVIDEO base

; === 1. PVIDEO_INTR default = 0 ===
    mov eax, [PV + 0x100]
    ASSERT_EQ eax, 0

; === 2. PVIDEO_INTR_EN write/read ===
    mov dword [PV + 0x140], 0x00000001
    mov eax, [PV + 0x140]
    ASSERT_EQ eax, 0x00000001

; === 3. PVIDEO_BUFFER write/read ===
    mov dword [PV + 0x700], 0x10    ; bit 4 = buffer 1 active
    mov eax, [PV + 0x700]
    ASSERT_EQ eax, 0x10

; === 4. PVIDEO_STOP write/read ===
    mov dword [PV + 0x704], 0x11    ; stop both buffers
    mov eax, [PV + 0x704]
    ASSERT_EQ eax, 0x11

; === 5. PVIDEO_BASE0 write/read ===
    mov dword [PV + 0x900], 0x00100000
    mov eax, [PV + 0x900]
    ASSERT_EQ eax, 0x00100000

; === 6. PVIDEO_BASE1 write/read ===
    mov dword [PV + 0x904], 0x00200000
    mov eax, [PV + 0x904]
    ASSERT_EQ eax, 0x00200000

; === 7. PVIDEO_LIMIT0 write/read ===
    mov dword [PV + 0x908], 0x001FFFFF
    mov eax, [PV + 0x908]
    ASSERT_EQ eax, 0x001FFFFF

; === 8. PVIDEO_OFFSET0 write/read ===
    mov dword [PV + 0x920], 0x00000400
    mov eax, [PV + 0x920]
    ASSERT_EQ eax, 0x00000400

; === 9. PVIDEO_SIZE_IN0 write/read ===
    ; Width=640 in low 16, height=480 in high 16
    mov dword [PV + 0x928], 0x01E00280
    mov eax, [PV + 0x928]
    ASSERT_EQ eax, 0x01E00280

; === 10. PVIDEO_DS_DX0 write/read (horizontal scale) ===
    mov dword [PV + 0x938], 0x00100000    ; 1.0 in 20.12 fixed point
    mov eax, [PV + 0x938]
    ASSERT_EQ eax, 0x00100000

; === 11. PVIDEO_DT_DY0 write/read (vertical scale) ===
    mov dword [PV + 0x940], 0x00100000
    mov eax, [PV + 0x940]
    ASSERT_EQ eax, 0x00100000

; === 12. PVIDEO_POINT_OUT0 write/read ===
    mov dword [PV + 0x948], 0x00640032    ; x=100, y=50
    mov eax, [PV + 0x948]
    ASSERT_EQ eax, 0x00640032

; === 13. PVIDEO_SIZE_OUT0 write/read ===
    mov dword [PV + 0x950], 0x01E00280    ; 640x480
    mov eax, [PV + 0x950]
    ASSERT_EQ eax, 0x01E00280

; === 14. PVIDEO_FORMAT0 write/read ===
    mov dword [PV + 0x958], 0x00000A00    ; pitch 2560 (640*4)
    mov eax, [PV + 0x958]
    ASSERT_EQ eax, 0x00000A00

; === 15. PVIDEO_INTR W1C ===
    ; Force INTR bit set, then W1C
    mov dword [PV + 0x100], 0x00000001    ; won't stick (W1C clears)
    mov eax, [PV + 0x100]
    ASSERT_EQ eax, 0                      ; 0 AND ~1 = 0

    PASS
