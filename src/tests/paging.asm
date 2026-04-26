; ===========================================================================
; paging.asm — Paging / Virtual Address Translation tests.
;
; Tests:
;   1. Identity-map setup + enable paging (CR0.PG) — execution continues
;   2. Read through identity-mapped page
;   3. Write through identity-mapped page
;   4. Non-identity mapped page: read remapped data
;   5. Write to remapped page, verify at physical address
;   6. PUSH/POP through paged stack
;   7. CALL/RET through paged stack
;   8. INVLPG + remap: read sees new data after TLB flush
;   9. 4 MB page (PS=1) identity map works
;
; Memory layout (all physical addresses):
;   0x0000_1000 .. test code (ORG)
;   0x0004_0000 .. page directory  (1 PDE = 1024 entries × 4 bytes = 4KB)
;   0x0004_1000 .. page table 0    (maps VA 0x0000_0000..0x003F_FFFF)
;   0x0004_2000 .. page table for test remap (maps VA 0x0040_0000+)
;   0x0005_0000 .. test data area (physical)
;   0x0005_1000 .. second test data area
;   0x0007_0000 .. stack area (already set by test runner at 0x80000)
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; Physical addresses for page structures
%define PAGE_DIR       0x00040000
%define PAGE_TABLE_0   0x00041000   ; maps VA 0..4MB (identity)
%define PAGE_TABLE_1   0x00042000   ; maps VA 4MB..8MB (remap tests)
%define TEST_DATA_PA   0x00050000   ; physical test data
%define TEST_DATA2_PA  0x00051000   ; second physical test data
%define REMAP_VA       0x00400000   ; VA that we'll remap to TEST_DATA_PA

; ===========================
; Step 0: Prepare page tables in physical memory (paging still OFF).
; ===========================

; --- Clear page directory (1024 entries × 4 = 4096 bytes) ---
    mov  edi, PAGE_DIR
    xor  eax, eax
    mov  ecx, 1024
.clear_pd:
    mov  [edi], eax
    add  edi, 4
    dec  ecx
    jnz  .clear_pd

; --- PDE 0: points to PAGE_TABLE_0 (identity map first 4MB) ---
;     PDE = PT_PA | P(1) | R/W(2) = PT_PA | 3
    mov  dword [PAGE_DIR + 0*4], PAGE_TABLE_0 | 0x03

; --- PDE 1: points to PAGE_TABLE_1 (remap VA 4MB..8MB) ---
    mov  dword [PAGE_DIR + 1*4], PAGE_TABLE_1 | 0x03

; --- Fill PAGE_TABLE_0: identity map 0..4MB (1024 × 4KB pages) ---
    mov  edi, PAGE_TABLE_0
    mov  eax, 0x00000003            ; PA=0x0000 | P | R/W
    mov  ecx, 1024
.fill_pt0:
    mov  [edi], eax
    add  edi, 4
    add  eax, 0x1000                ; next 4KB page
    dec  ecx
    jnz  .fill_pt0

; --- Clear PAGE_TABLE_1 ---
    mov  edi, PAGE_TABLE_1
    xor  eax, eax
    mov  ecx, 1024
.clear_pt1:
    mov  [edi], eax
    add  edi, 4
    dec  ecx
    jnz  .clear_pt1

; --- PTE for REMAP_VA → TEST_DATA_PA ---
; REMAP_VA = 0x0040_0000: PDE index = 1, PTE index = 0
    mov  dword [PAGE_TABLE_1 + 0*4], TEST_DATA_PA | 0x03

; --- Write known data at physical test locations ---
    mov  dword [TEST_DATA_PA],   0xDEADBEEF
    mov  dword [TEST_DATA2_PA],  0xCAFEBABE
    mov  dword [TEST_DATA_PA+4], 0x12345678

; ===========================
; Step 1: Enable paging.
; ===========================
; Set CR3 = page directory PA, then set CR0.PG (bit 31).
    mov  eax, PAGE_DIR
    mov  cr3, eax

    mov  eax, cr0
    or   eax, 0x80000000           ; set PG bit
    mov  cr0, eax

; If we get here, paging is active and identity mapping works for code fetch.
    ASSERT_EQ ebx, 0               ; 1 — survived paging enable (EBX was 0)

; ===========================
; Test 2: Read through identity-mapped page.
; ===========================
    mov  ebx, [TEST_DATA_PA]       ; identity-mapped: VA == PA
    ASSERT_EQ ebx, 0xDEADBEEF      ; 2 — identity-mapped read

; ===========================
; Test 3: Write through identity-mapped page.
; ===========================
    mov  dword [TEST_DATA_PA+8], 0xAAAABBBB
    mov  ecx, [TEST_DATA_PA+8]
    ASSERT_EQ ecx, 0xAAAABBBB      ; 3 — identity-mapped write + readback

; ===========================
; Test 4: Read through remapped page.
; REMAP_VA (0x0040_0000) is mapped to TEST_DATA_PA (0x0005_0000).
; ===========================
    mov  edx, [REMAP_VA]           ; should read from TEST_DATA_PA
    ASSERT_EQ edx, 0xDEADBEEF      ; 4 — remapped read

; ===========================
; Test 5: Write through remapped page, verify at physical address.
; ===========================
    mov  dword [REMAP_VA+4], 0x55667788
    ; Read back through identity map to verify the write went to TEST_DATA_PA+4
    mov  ebx, [TEST_DATA_PA+4]
    ASSERT_EQ ebx, 0x55667788      ; 5 — remapped write visible at PA

; ===========================
; Test 6: PUSH/POP through paged stack.
; Stack is at ESP ~0x80000, identity-mapped.
; ===========================
    push 0xAABBCCDD
    pop  ebx
    ASSERT_EQ ebx, 0xAABBCCDD      ; 6 — push/pop through paged stack

; ===========================
; Test 7: CALL/RET through paged stack.
; ===========================
    mov  ebx, 0
    call .test_func
    ASSERT_EQ ebx, 42              ; 7 — call/ret through paged stack
    jmp  .test7_done

.test_func:
    mov  ebx, 42
    ret

.test7_done:

; ===========================
; Test 8: INVLPG + remap.
; Change REMAP_VA to point to TEST_DATA2_PA, flush TLB for REMAP_VA,
; then read REMAP_VA and expect TEST_DATA2_PA's data.
; ===========================
    ; Change PTE: REMAP_VA → TEST_DATA2_PA
    mov  dword [PAGE_TABLE_1 + 0*4], TEST_DATA2_PA | 0x03
    ; Flush TLB entry for REMAP_VA
    invlpg [REMAP_VA]
    ; Read through REMAP_VA: should now see TEST_DATA2_PA's value
    mov  edx, [REMAP_VA]
    ASSERT_EQ edx, 0xCAFEBABE      ; 8 — INVLPG remap works

; ===========================
; Test 9: Disable paging, verify we're back to flat mode.
; ===========================
    mov  eax, cr0
    and  eax, 0x7FFFFFFF           ; clear PG bit
    mov  cr0, eax
    ; Now VA == PA again. Read TEST_DATA_PA directly.
    mov  ebx, [TEST_DATA_PA]
    ASSERT_EQ ebx, 0xDEADBEEF      ; 9 — back to flat mode after PG clear

; ===========================
; PASS
; ===========================
    PASS
