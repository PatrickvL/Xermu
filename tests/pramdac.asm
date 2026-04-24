; ===========================================================================
; pramdac.asm — NV2A PCRTC + PRAMDAC register tests (--xbox mode).
;
; Tests expanded PCRTC and PRAMDAC registers:
;   1. PCRTC_START write/read
;   2. PCRTC_CONFIG write/read
;   3. PCRTC_RASTER is readable (returns scanline 0..524)
;   4. PRAMDAC NVPLL default
;   5. PRAMDAC MPLL default
;   6. PRAMDAC VPLL default
;   7. PRAMDAC TV_SETUP write/read
;   8. PRAMDAC TV_VTOTAL write/read
;   9. PRAMDAC FP_TMDS write/read
;  10. PRAMDAC PLL_TEST write/read
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A equ 0xFD000000
CRTC equ NV2A + 0x600000
RAMD equ NV2A + 0x680000

; === 1. PCRTC_START write/read ===
    mov dword [CRTC + 0x800], 0x03C00000   ; framebuffer at ~60 MB
    mov eax, [CRTC + 0x800]
    ASSERT_EQ eax, 0x03C00000

; === 2. PCRTC_CONFIG write/read ===
    mov dword [CRTC + 0x804], 0x00000001
    mov eax, [CRTC + 0x804]
    ASSERT_EQ eax, 0x00000001

; === 3. PCRTC_RASTER is readable (0..524) ===
    mov eax, [CRTC + 0x808]
    cmp eax, 525
    jb .raster_ok
    mov eax, __test_num + 1
    hlt
.raster_ok:
    %assign __test_num __test_num + 1

; === 4. PRAMDAC NVPLL default ===
    mov eax, [RAMD + 0x500]
    ASSERT_EQ eax, 0x00011C01

; === 5. PRAMDAC MPLL default ===
    mov eax, [RAMD + 0x504]
    ASSERT_EQ eax, 0x00011801

; === 6. PRAMDAC VPLL default ===
    mov eax, [RAMD + 0x508]
    ASSERT_EQ eax, 0x00031801

; === 7. PRAMDAC TV_SETUP write/read ===
    mov dword [RAMD + 0x700], 0x00000001
    mov eax, [RAMD + 0x700]
    ASSERT_EQ eax, 0x00000001

; === 8. PRAMDAC TV_VTOTAL write/read ===
    mov dword [RAMD + 0x720], 525
    mov eax, [RAMD + 0x720]
    ASSERT_EQ eax, 525

; === 9. PRAMDAC FP_TMDS write/read ===
    mov dword [RAMD + 0x8C0], 0x00AABBCC
    mov eax, [RAMD + 0x8C0]
    ASSERT_EQ eax, 0x00AABBCC

; === 10. PRAMDAC PLL_TEST write/read ===
    mov dword [RAMD + 0x514], 0x12340000
    mov eax, [RAMD + 0x514]
    ASSERT_EQ eax, 0x12340000

    PASS
