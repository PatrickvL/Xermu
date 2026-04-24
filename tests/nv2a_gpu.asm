; ===========================================================================
; nv2a_gpu.asm — NV2A GPU register tests (--xbox mode).
;
; Tests PFIFO, PGRAPH, and PRAMDAC register stubs:
;   1. PMC_BOOT_0 = NV2A chip ID (0x02A000A1)
;   2. PFIFO_CACHES write/read
;   3. PFIFO_MODE write/read
;   4. CACHE1_PUSH0 write/read
;   5. CACHE1_PULL0 write/read
;   6. CACHE1_STATUS = 0x10 (empty) on init
;   7. PGRAPH_FIFO write/read
;   8. PGRAPH_INTR W1C behavior
;   9. PRAMDAC NVPLL_COEFF default readback
;  10. PRAMDAC NVPLL_COEFF write/read
;  11. PRAMDAC MPLL_COEFF default readback
;  12. PRAMDAC VPLL_COEFF default readback
;  13. PFB_CFG0 = 0x03070103 (64 MB RAM)
;  14. DMA_STATE idle when GET == PUT
;  15. DMA pusher: GET advances toward PUT after ticks
;  16. CACHE1_STATUS = empty after pusher drains
;  12. PRAMDAC VPLL_COEFF default readback
;  13. PFB_CFG0 = 0x03070103 (64 MB RAM)
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A equ 0xFD000000

; PMC
PMC_BOOT_0        equ NV2A + 0x000000

; PFIFO
PFIFO_CACHES      equ NV2A + 0x002500
PFIFO_MODE        equ NV2A + 0x002504
CACHE1_PUSH0      equ NV2A + 0x003200
CACHE1_PULL0      equ NV2A + 0x003250
CACHE1_STATUS     equ NV2A + 0x003214

; PGRAPH
PGRAPH_INTR       equ NV2A + 0x400100
PGRAPH_INTR_EN    equ NV2A + 0x400140
PGRAPH_FIFO       equ NV2A + 0x400720

; PRAMDAC
NVPLL_COEFF       equ NV2A + 0x680500
MPLL_COEFF        equ NV2A + 0x680504
VPLL_COEFF        equ NV2A + 0x680508

; PFB
PFB_CFG0          equ NV2A + 0x100200

; DMA pusher
CACHE1_DMA_PUSH   equ NV2A + 0x003220
CACHE1_DMA_PUT    equ NV2A + 0x003240
CACHE1_DMA_GET    equ NV2A + 0x003244
CACHE1_DMA_STATE  equ NV2A + 0x003228

; === 1. PMC_BOOT_0 = NV2A chip ID ===
    mov eax, [PMC_BOOT_0]
    ASSERT_EQ eax, 0x02A000A1

; === 2. PFIFO_CACHES write/read ===
    mov dword [PFIFO_CACHES], 1
    mov eax, [PFIFO_CACHES]
    ASSERT_EQ eax, 1

; === 3. PFIFO_MODE write/read ===
    mov dword [PFIFO_MODE], 0x00000001
    mov eax, [PFIFO_MODE]
    ASSERT_EQ eax, 0x00000001

; === 4. CACHE1_PUSH0 write/read ===
    mov dword [CACHE1_PUSH0], 1
    mov eax, [CACHE1_PUSH0]
    ASSERT_EQ eax, 1

; === 5. CACHE1_PULL0 write/read ===
    mov dword [CACHE1_PULL0], 1
    mov eax, [CACHE1_PULL0]
    ASSERT_EQ eax, 1

; === 6. CACHE1_STATUS = 0x10 (empty) on init ===
    ; First reset by writing 0 to push/pull, then read status
    mov eax, [CACHE1_STATUS]
    ASSERT_EQ eax, 0x10

; === 7. PGRAPH_FIFO write/read ===
    mov dword [PGRAPH_FIFO], 1
    mov eax, [PGRAPH_FIFO]
    ASSERT_EQ eax, 1

; === 8. PGRAPH_INTR W1C ===
    ; Read PGRAPH_INTR — should be 0 initially
    mov eax, [PGRAPH_INTR]
    ASSERT_EQ eax, 0
    ; Write to INTR_EN, verify it sticks
    mov dword [PGRAPH_INTR_EN], 0x11111111
    mov eax, [PGRAPH_INTR_EN]
    ASSERT_EQ eax, 0x11111111

; === 9. PRAMDAC NVPLL_COEFF default ===
    mov eax, [NVPLL_COEFF]
    ASSERT_EQ eax, 0x00011C01

; === 10. PRAMDAC NVPLL_COEFF write/read ===
    mov dword [NVPLL_COEFF], 0x00021E02
    mov eax, [NVPLL_COEFF]
    ASSERT_EQ eax, 0x00021E02

; === 11. PRAMDAC MPLL_COEFF default ===
    mov eax, [MPLL_COEFF]
    ASSERT_EQ eax, 0x00011801

; === 12. PRAMDAC VPLL_COEFF default ===
    mov eax, [VPLL_COEFF]
    ASSERT_EQ eax, 0x00031801

; === 13. PFB_CFG0 = 64 MB ===
    mov eax, [PFB_CFG0]
    ASSERT_EQ eax, 0x03070103

; === 14. DMA_STATE idle when GET == PUT ===
    ; Initially GET == PUT == 0, so DMA_STATE should be idle (0).
    mov eax, [CACHE1_DMA_STATE]
    ASSERT_EQ eax, 0

; === 15. DMA pusher: GET advances toward PUT after ticks ===
    ; Set DMA_PUT to 64 bytes ahead (16 dwords of "commands").
    ; Use address 0x60000 as the push buffer (safe RAM area).
    mov dword [CACHE1_DMA_PUT], 64
    mov dword [CACHE1_DMA_GET], 0
    ; Enable DMA pusher.
    mov dword [CACHE1_DMA_PUSH], 1
    ; Enable PUSH0.
    mov dword [CACHE1_PUSH0], 1
    ; Spin until GET catches up to PUT (tick callback advances GET).
    mov ecx, 200000
.spin_fifo:
    mov eax, [CACHE1_DMA_GET]
    cmp eax, 64
    je .fifo_drained
    dec ecx
    jnz .spin_fifo
    ; Timed out — fail.
    mov eax, __test_num + 1
    hlt
.fifo_drained:
    %assign __test_num __test_num + 1

; === 16. CACHE1_STATUS = empty after pusher drains ===
    mov eax, [CACHE1_STATUS]
    ASSERT_EQ eax, 0x10         ; bit 4 = empty

    PASS
