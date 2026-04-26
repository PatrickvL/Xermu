; ===========================================================================
; pfifo.asm — NV2A PFIFO DMA pusher command parsing tests (--xbox mode).
;
; Writes NV2A push buffer commands into guest RAM at a known address,
; programs the DMA pusher, spins until drained, and verifies command
; parsing stats via emulator extension registers.
;
; Tests:
;   1.  Single INCREASING method (1 data dword) — 1 method dispatched
;   2.  INCREASING method with count=3 — 3 methods dispatched
;   3.  NON_INCREASING method with count=2 — 2 methods dispatched
;   4.  NOP (all-zero dword) skipped correctly
;   5.  JUMP command redirects GET
;   6.  Total dwords consumed is correct
;   7.  Total methods dispatched is correct
;   8.  CALL/RETURN: subroutine dispatches method, GET resumes after CALL
;   9.  CALL count incremented
;  10.  Cumulative methods correct after CALL/RETURN
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A equ 0xFD000000

; PFIFO DMA pusher registers
CACHE1_PUSH0      equ NV2A + 0x003200
CACHE1_DMA_PUSH   equ NV2A + 0x003220
CACHE1_DMA_PUT    equ NV2A + 0x003240
CACHE1_DMA_GET    equ NV2A + 0x003244

; Emulator extension: PFIFO stats
FIFO_METHODS      equ NV2A + 0x003F00
FIFO_DWORDS       equ NV2A + 0x003F04
FIFO_JUMPS        equ NV2A + 0x003F08
FIFO_CALLS        equ NV2A + 0x003F0C

; Push buffer lives at PA 0x60000 (safe area, well within RAM).
PB_BASE equ 0x00060000

; ---------------------------------------------------------------------------
; Helper: Build an NV2A push buffer command header.
;   type=0: INCREASING, type=4: NON_INCREASING
;   method is in bytes (e.g. 0x100), subchannel 0-7, count 1-2047.
; ---------------------------------------------------------------------------
%define PB_HDR_INC(method, subchan, count)  ((count) << 18) | ((subchan) << 13) | (method)
%define PB_HDR_NI(method, subchan, count)   (4 << 29) | ((count) << 18) | ((subchan) << 13) | (method)
%define PB_JUMP(addr)                       (0x40000000 | (addr))
%define PB_CALL(addr)                       ((addr) | 2)
%define PB_RETURN                           0x00020000

; ===========================================================================
; Write push buffer commands into RAM at PB_BASE.
; ===========================================================================

    ; --- Command 0: INCREASING method 0x100, subchannel 0, count=1 ---
    ; Header (1 dword) + data (1 dword) = 2 dwords total
    mov dword [PB_BASE +  0], PB_HDR_INC(0x100, 0, 1)
    mov dword [PB_BASE +  4], 0xDEADBEEF       ; data for method 0x100

    ; --- Command 1: INCREASING method 0x200, subchannel 1, count=3 ---
    ; Header (1 dword) + data (3 dwords) = 4 dwords total
    mov dword [PB_BASE +  8], PB_HDR_INC(0x200, 1, 3)
    mov dword [PB_BASE + 12], 0x11111111       ; data for method 0x200
    mov dword [PB_BASE + 16], 0x22222222       ; data for method 0x204
    mov dword [PB_BASE + 20], 0x33333333       ; data for method 0x208

    ; --- Command 2: NON_INCREASING method 0x400, subchannel 2, count=2 ---
    ; Header (1 dword) + data (2 dwords) = 3 dwords total
    mov dword [PB_BASE + 24], PB_HDR_NI(0x400, 2, 2)
    mov dword [PB_BASE + 28], 0xAAAAAAAA       ; data for method 0x400 (1st)
    mov dword [PB_BASE + 32], 0xBBBBBBBB       ; data for method 0x400 (2nd)

    ; --- Command 3: NOP (zero dword, should be skipped) ---
    mov dword [PB_BASE + 36], 0x00000000

    ; Total so far: 10 dwords at PB_BASE+0 .. PB_BASE+36
    ; Methods dispatched: 1 + 3 + 2 = 6
    ; Total dwords consumed: 10 (4 headers + 6 data)

    ; -----------------------------------------------------------------------
    ; Program the DMA pusher.
    ; -----------------------------------------------------------------------
    ; PUT = PB_BASE + 40 (10 dwords * 4 bytes = 40)
    mov dword [CACHE1_DMA_PUT], PB_BASE + 40
    mov dword [CACHE1_DMA_GET], PB_BASE
    mov dword [CACHE1_DMA_PUSH], 1      ; enable DMA pusher
    mov dword [CACHE1_PUSH0], 1          ; enable CACHE1

    ; Spin until GET == PUT.
    mov ecx, 400000
.spin1:
    mov eax, [CACHE1_DMA_GET]
    cmp eax, PB_BASE + 40
    je .done1
    dec ecx
    jnz .spin1
    mov eax, 0xFF
    hlt
.done1:

    ; === 1. Methods dispatched = 6 (1 + 3 + 2) ===
    mov eax, [FIFO_METHODS]
    ASSERT_EQ eax, 6

    ; === 2. Dwords consumed = 10 (4 headers/NOPs + 6 data) ===
    mov eax, [FIFO_DWORDS]
    ASSERT_EQ eax, 10

    ; -----------------------------------------------------------------------
    ; Phase 2: Test JUMP command.
    ; Write commands at PB_BASE+0x100 with a JUMP in the middle.
    ; -----------------------------------------------------------------------

    ; Write a JUMP at PB_BASE+0x100 that jumps to PB_BASE+0x200.
    mov dword [PB_BASE + 0x100], PB_JUMP(PB_BASE + 0x200)

    ; At PB_BASE+0x200: one INCREASING method, count=1.
    mov dword [PB_BASE + 0x200], PB_HDR_INC(0x300, 0, 1)
    mov dword [PB_BASE + 0x204], 0x42424242

    ; Disable pusher before reprogramming GET/PUT to avoid a race where
    ; the thread processes from the old GET through uninitialised memory.
    mov dword [CACHE1_DMA_PUSH], 0

    ; Program pusher: GET = PB_BASE+0x100, PUT = PB_BASE+0x208.
    ; The JUMP at 0x100 redirects to 0x200, then 2 dwords (hdr+data) at 0x200.
    mov dword [CACHE1_DMA_GET], PB_BASE + 0x100
    mov dword [CACHE1_DMA_PUT], PB_BASE + 0x208

    ; Re-enable pusher — thread drains from the correct GET.
    mov dword [CACHE1_DMA_PUSH], 1

    ; Spin until GET == PUT.
    mov ecx, 400000
.spin2:
    mov eax, [CACHE1_DMA_GET]
    cmp eax, PB_BASE + 0x208
    je .done2
    dec ecx
    jnz .spin2
    mov eax, 0xFE
    hlt
.done2:

    ; === 3. JUMP count incremented ===
    mov eax, [FIFO_JUMPS]
    ASSERT_EQ eax, 1

    ; === 4. Cumulative methods = 6 + 1 = 7 ===
    mov eax, [FIFO_METHODS]
    ASSERT_EQ eax, 7

    ; === 5. Cumulative dwords consumed = 10 + 3 = 13 (1 JUMP hdr + 1 method hdr + 1 data) ===
    mov eax, [FIFO_DWORDS]
    ASSERT_EQ eax, 13

    ; -----------------------------------------------------------------------
    ; Phase 3: Test CALL/RETURN (single-level subroutine).
    ;
    ; Layout:
    ;   PB_BASE+0x300: CALL (PB_BASE+0x400)   — calls subroutine
    ;   PB_BASE+0x304: INC method 0x500, count=1, data 0x77777777  — after return
    ;   PB_BASE+0x30C: <end of main stream>
    ;
    ;   PB_BASE+0x400: INC method 0x600, count=1, data 0x88888888  — subroutine body
    ;   PB_BASE+0x408: RETURN
    ;   PB_BASE+0x40C: <never reached>
    ; -----------------------------------------------------------------------

    ; Main stream
    mov dword [PB_BASE + 0x300], PB_CALL(PB_BASE + 0x400)
    mov dword [PB_BASE + 0x304], PB_HDR_INC(0x500, 0, 1)
    mov dword [PB_BASE + 0x308], 0x77777777

    ; Subroutine at PB_BASE+0x400
    mov dword [PB_BASE + 0x400], PB_HDR_INC(0x600, 0, 1)
    mov dword [PB_BASE + 0x404], 0x88888888
    mov dword [PB_BASE + 0x408], PB_RETURN

    ; Disable pusher before reprogramming (same race avoidance as Phase 2).
    mov dword [CACHE1_DMA_PUSH], 0

    ; Program pusher: GET = 0x300, PUT = 0x30C
    mov dword [CACHE1_DMA_GET], PB_BASE + 0x300
    mov dword [CACHE1_DMA_PUT], PB_BASE + 0x30C

    ; Re-enable pusher.
    mov dword [CACHE1_DMA_PUSH], 1

    ; Spin until GET == PUT (PUT = 0x30C, after the post-return method)
    mov ecx, 400000
.spin3:
    mov eax, [CACHE1_DMA_GET]
    cmp eax, PB_BASE + 0x30C
    je .done3
    dec ecx
    jnz .spin3
    mov eax, 0xFD
    hlt
.done3:

    ; === 6. CALL count incremented ===
    mov eax, [FIFO_CALLS]
    ASSERT_EQ eax, 1

    ; === 7. Cumulative methods = 7 + 2 = 9 (subroutine method + post-return method) ===
    mov eax, [FIFO_METHODS]
    ASSERT_EQ eax, 9

    ; === 8. Cumulative dwords = 13 + 6 = 19 ===
    ; Phase 3: CALL hdr(1) + subr hdr(1) + subr data(1) + RETURN(1) + main hdr(1) + main data(1) = 6
    mov eax, [FIFO_DWORDS]
    ASSERT_EQ eax, 19

    PASS
