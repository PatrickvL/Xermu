; ===========================================================================
; string.asm — String instruction tests (MOVS/STOS/LODS/CMPS/SCAS + REP).
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; Reserve working buffers at known addresses
%define SRC_BUF     0x4000
%define DST_BUF     0x5000
%define SCRATCH     0x6000

; ---- STOSB: fill 8 bytes with 0xAA ----
    cld
    mov edi, DST_BUF
    mov ecx, 8
    mov al, 0xAA
    rep stosb

    ; Check first and last byte
    ASSERT_EQ_MEM DST_BUF, 0xAAAAAAAA           ; 1: first 4 bytes
    ASSERT_EQ_MEM DST_BUF+4, 0xAAAAAAAA         ; 2: next 4 bytes
    ; EDI should have advanced by 8
    mov eax, DST_BUF + 8
    ASSERT_EQ edi, eax                           ; 3: EDI advanced
    ASSERT_EQ ecx, 0                             ; 4: ECX = 0

; ---- STOSD: fill 4 dwords with 0xDEADBEEF ----
    mov edi, DST_BUF
    mov ecx, 4
    mov eax, 0xDEADBEEF
    rep stosd

    ASSERT_EQ_MEM DST_BUF, 0xDEADBEEF           ; 5
    ASSERT_EQ_MEM DST_BUF+4, 0xDEADBEEF         ; 6
    ASSERT_EQ_MEM DST_BUF+8, 0xDEADBEEF         ; 7
    ASSERT_EQ_MEM DST_BUF+12, 0xDEADBEEF        ; 8
    ASSERT_EQ ecx, 0                             ; 9

; ---- MOVSD: copy 4 dwords from SRC to DST ----
    ; Set up source buffer
    mov dword [SRC_BUF],   0x11111111
    mov dword [SRC_BUF+4], 0x22222222
    mov dword [SRC_BUF+8], 0x33333333
    mov dword [SRC_BUF+12],0x44444444

    ; Clear destination
    mov edi, DST_BUF
    mov ecx, 4
    xor eax, eax
    rep stosd

    ; Now copy
    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 4
    rep movsd

    ASSERT_EQ_MEM DST_BUF,   0x11111111          ; 10
    ASSERT_EQ_MEM DST_BUF+4, 0x22222222          ; 11
    ASSERT_EQ_MEM DST_BUF+8, 0x33333333          ; 12
    ASSERT_EQ_MEM DST_BUF+12,0x44444444          ; 13
    ASSERT_EQ ecx, 0                              ; 14

; ---- MOVSB: copy 3 bytes ----
    mov byte [SRC_BUF], 0x41      ; 'A'
    mov byte [SRC_BUF+1], 0x42    ; 'B'
    mov byte [SRC_BUF+2], 0x43    ; 'C'
    mov byte [DST_BUF], 0
    mov byte [DST_BUF+1], 0
    mov byte [DST_BUF+2], 0

    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 3
    rep movsb

    ; Check: DST should contain "ABC"
    movzx eax, byte [DST_BUF]
    ASSERT_EQ eax, 0x41                           ; 15
    movzx eax, byte [DST_BUF+1]
    ASSERT_EQ eax, 0x42                           ; 16
    movzx eax, byte [DST_BUF+2]
    ASSERT_EQ eax, 0x43                           ; 17

; ---- LODSB: load 3 bytes sequentially ----
    mov byte [SRC_BUF], 0x10
    mov byte [SRC_BUF+1], 0x20
    mov byte [SRC_BUF+2], 0x30
    mov esi, SRC_BUF

    lodsb
    movzx ebx, al
    ASSERT_EQ ebx, 0x10                           ; 18

    lodsb
    movzx ebx, al
    ASSERT_EQ ebx, 0x20                           ; 19

    lodsb
    movzx ebx, al
    ASSERT_EQ ebx, 0x30                           ; 20

; ---- LODSD: load a dword ----
    mov dword [SRC_BUF], 0xCAFEBABE
    mov esi, SRC_BUF
    lodsd
    ASSERT_EQ eax, 0xCAFEBABE                     ; 21

; ---- Single STOSB (no REP) ----
    mov byte [SCRATCH], 0
    mov edi, SCRATCH
    mov al, 0x77
    stosb
    movzx eax, byte [SCRATCH]
    ASSERT_EQ eax, 0x77                           ; 22

; ---- Single MOVSD (no REP) ----
    mov dword [SRC_BUF], 0xBAADF00D
    mov dword [DST_BUF], 0
    mov esi, SRC_BUF
    mov edi, DST_BUF
    movsd
    ASSERT_EQ_MEM DST_BUF, 0xBAADF00D            ; 23

; ---- CMPSB: compare equal strings ----
    mov byte [SRC_BUF], 0x41
    mov byte [SRC_BUF+1], 0x42
    mov byte [DST_BUF], 0x41
    mov byte [DST_BUF+1], 0x42
    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 2
    repe cmpsb
    ; Should have compared all 2 bytes, ZF=1 at end
    ASSERT_FLAGS 0x40, 0x40                       ; 24: ZF set (equal)
    ASSERT_EQ ecx, 0                              ; 25

; ---- CMPSB: find first difference ----
    mov byte [SRC_BUF],   0x41    ; 'A'
    mov byte [SRC_BUF+1], 0x42    ; 'B'
    mov byte [SRC_BUF+2], 0x43    ; 'C'
    mov byte [DST_BUF],   0x41    ; 'A'
    mov byte [DST_BUF+1], 0x42    ; 'B'
    mov byte [DST_BUF+2], 0x58    ; 'X' — different!
    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 3
    repe cmpsb
    ; Should stop after 3rd byte mismatch, ZF=0
    ASSERT_FLAGS 0x40, 0x00                       ; 26: ZF clear (not equal)
    ASSERT_EQ ecx, 0                              ; 27: ECX decremented for all 3

; ---- SCASB: search for byte 0x42 in buffer ----
    mov byte [DST_BUF],   0x41
    mov byte [DST_BUF+1], 0x42    ; target
    mov byte [DST_BUF+2], 0x43
    mov edi, DST_BUF
    mov ecx, 3
    mov al, 0x42
    repne scasb
    ; Should find 0x42 at index 1, ECX=1
    ASSERT_FLAGS 0x40, 0x40                       ; 28: ZF set (found)
    ASSERT_EQ ecx, 1                              ; 29: stopped after 2 iterations

; ---- SCASB: byte not found ----
    mov byte [DST_BUF],   0x10
    mov byte [DST_BUF+1], 0x20
    mov byte [DST_BUF+2], 0x30
    mov edi, DST_BUF
    mov ecx, 3
    mov al, 0xFF
    repne scasb
    ASSERT_FLAGS 0x40, 0x00                       ; 30: ZF clear (not found)
    ASSERT_EQ ecx, 0                              ; 31: exhausted

; ---- STOSW: fill with 16-bit values ----
    mov edi, DST_BUF
    mov ecx, 2
    mov ax, 0x1234
    rep stosw
    ASSERT_EQ_MEM DST_BUF, 0x12341234            ; 32

; ---- MOVSW: copy 16-bit values ----
    mov word [SRC_BUF], 0xABCD
    mov word [SRC_BUF+2], 0xEF01
    mov word [DST_BUF], 0
    mov word [DST_BUF+2], 0
    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 2
    rep movsw
    ASSERT_EQ_MEM DST_BUF, 0xEF01ABCD            ; 33: little-endian pair

; ---- STD + REP STOSB (backward direction) ----
    ; Fill DST_BUF with zeros first
    cld
    mov edi, DST_BUF
    mov ecx, 8
    xor eax, eax
    rep stosd

    ; Store 4 bytes backwards from DST_BUF+3
    std
    mov edi, DST_BUF + 3
    mov ecx, 4
    mov al, 0xBB
    rep stosb
    cld                                            ; restore direction flag

    ASSERT_EQ_MEM DST_BUF, 0xBBBBBBBB            ; 34: 4 bytes at DST_BUF

; ---- REP MOVSD with ECX=0 (no-op) ----
    mov dword [DST_BUF], 0x99999999
    mov esi, SRC_BUF
    mov edi, DST_BUF
    mov ecx, 0
    rep movsd
    ASSERT_EQ_MEM DST_BUF, 0x99999999            ; 35: unchanged

; ---- CMPSB single (no REP) ----
    mov byte [SRC_BUF], 0x50
    mov byte [DST_BUF], 0x50
    mov esi, SRC_BUF
    mov edi, DST_BUF
    cmpsb
    ASSERT_FLAGS 0x40, 0x40                       ; 36: ZF set (equal)

    PASS
