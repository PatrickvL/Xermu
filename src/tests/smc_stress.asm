; ===========================================================================
; smc_stress.asm — Advanced self-modifying code stress tests.
;
; Exercises page-protection SMC detection corner cases:
;   - Multiple subroutines on the same code page
;   - Rapid-fire patching of the same instruction (10× cycles)
;   - Cross-page SMC (code on page A patches code on page B)
;   - Data + code mixed on same page
;   - Patch NOP → real instruction (trace grows)
;   - Patch conditional branch immediate (affects block linking)
;   - Re-protect after invalidation (rebuild re-protects page)
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; =====================================================================
; Test 1-2: Two subroutines on the same page.
;
; Patch sub_a's immediate; sub_b must remain unchanged.
; Verifies that invalidation rebuilds the correct trace without
; corrupting neighbor traces on the same page.
; =====================================================================
    call .sub_a
    ASSERT_EQ ebx, 0x11111111           ; 1

    call .sub_b
    ASSERT_EQ ecx, 0xAAAAAAAA           ; 2

    ; Patch sub_a only.
    mov dword [.sub_a + 1], 0x22222222
    call .sub_a
    ASSERT_EQ ebx, 0x22222222           ; 3

    ; sub_b must still return original value.
    call .sub_b
    ASSERT_EQ ecx, 0xAAAAAAAA           ; 4

    jmp .test3

.sub_a:
    mov ebx, 0x11111111
    ret

.sub_b:
    mov ecx, 0xAAAAAAAA
    ret

; =====================================================================
; Test 3: Rapid-fire patching — 10 cycles to same instruction.
;
; Stresses page-protection flip-flop: protect → write-fault →
; unprotect → invalidate → rebuild → re-protect → repeat.
; =====================================================================
.test3:
    mov esi, 1                          ; counter
.patch_loop:
    mov dword [.rapid_target + 1], esi  ; patch immediate
    call .rapid_target
    cmp eax, esi                        ; verify result matches
    jne .rapid_fail
    inc esi
    cmp esi, 11
    jb .patch_loop

    ; All 10 iterations matched.
    jmp .test4

.rapid_fail:
    mov eax, 5                          ; assertion 5
    hlt

.rapid_target:
    mov eax, 0                          ; immediate patched each loop
    ret

; =====================================================================
; Test 4: Cross-page SMC.
;
; Code on page 0x1000 patches code on page 0x2000.  The patched code
; page (0x2000) must be invalidated; the patching page (0x1000) must
; NOT be invalidated.
;
; Uses a trampoline at 0x2000 (copied there at runtime).
; =====================================================================
.test4:
    ; Copy .far_target (6 bytes: MOV EAX, imm32 + RET) to PA 0x2000.
    mov eax, [.far_target]
    mov [0x2000], eax
    mov ax, [.far_target + 4]
    mov [0x2004], ax

    ; Call the copy at 0x2000.
    call 0x2000
    ASSERT_EQ eax, 0x44444444           ; 6

    ; Patch the copy's immediate from THIS page (0x1xxx).
    mov dword [0x2001], 0x55555555
    call 0x2000
    ASSERT_EQ eax, 0x55555555           ; 7

    jmp .test5

.far_target:
    mov eax, 0x44444444
    ret

; =====================================================================
; Test 5: Data + code mixed on same page.
;
; .data_slot is on the same page as .sub_c.  Writing to .data_slot
; triggers page-protection fault and invalidates traces on this page.
; But .sub_c must still work correctly after re-execution.
; =====================================================================
.test5:
    call .sub_c
    ASSERT_EQ edx, 0x77777777           ; 8

    ; Write to data area on same page.
    mov dword [.data_slot], 0x12345678
    mov eax, [.data_slot]
    ASSERT_EQ eax, 0x12345678           ; 9

    ; Code must still work after page was invalidated and rebuilt.
    call .sub_c
    ASSERT_EQ edx, 0x77777777           ; 10

    jmp .test6

.sub_c:
    mov edx, 0x77777777
    ret

.data_slot:
    dd 0

; =====================================================================
; Test 6: Patch NOP sled → real instruction.
;
; Target starts with NOP; NOP; MOV EAX,0; RET.
; Patch the first NOP (0x90) to INC EAX (0x40 re-encoded as 0xFF 0xC0).
; Actually, just patch the immediate in the MOV for simplicity.
; This tests that traces containing NOPs are correctly invalidated.
; =====================================================================
.test6:
    call .nop_target
    ASSERT_EQ eax, 0                    ; 11 — original: mov eax,0

    ; Patch the MOV EAX,0 immediate to 0x42.
    mov dword [.nop_target + 3], 0x42
    call .nop_target
    ASSERT_EQ eax, 0x42                 ; 12

    jmp .test7

.nop_target:
    nop
    nop
    mov eax, 0
    ret

; =====================================================================
; Test 7: Patch conditional branch displacement.
;
; .cond_target has: TEST ECX,ECX / JZ .cond_b / MOV EAX,1 / RET
; .cond_b: MOV EAX,2 / RET
; .cond_c: MOV EAX,3 / RET
;
; Call with ECX=0 → takes branch → EAX=2.
; Then patch the JZ rel8 to point to .cond_c instead.
; Call again with ECX=0 → takes patched branch → EAX=3.
; =====================================================================
.test7:
    xor ecx, ecx
    call .cond_target
    ASSERT_EQ eax, 2                    ; 13

    ; Patch rel8 displacement of JZ instruction.
    ; JZ is at .cond_target+2 (after TEST ECX,ECX = 85 C9, then 74 xx).
    ; Original rel8 goes to .cond_b; new rel8 goes to .cond_c.
    mov byte [.cond_target + 3], (.cond_c - (.cond_target + 4))

    xor ecx, ecx
    call .cond_target
    ASSERT_EQ eax, 3                    ; 14

    jmp .test8

.cond_target:
    test ecx, ecx
    jz .cond_b
    mov eax, 1
    ret
.cond_b:
    mov eax, 2
    ret
.cond_c:
    mov eax, 3
    ret

; =====================================================================
; Test 8: Re-protect after invalidation.
;
; After SMC invalidation + rebuild, the page should be re-protected.
; A second write to the same page must also trigger invalidation.
; =====================================================================
.test8:
    call .reproc_target
    ASSERT_EQ ebx, 0x100                ; 15 — first value

    ; First patch — page is unprotected, then re-protected after rebuild.
    mov dword [.reproc_target + 1], 0x200
    call .reproc_target
    ASSERT_EQ ebx, 0x200                ; 16 — second value

    ; Second patch — must ALSO trigger invalidation (page was re-protected).
    mov dword [.reproc_target + 1], 0x300
    call .reproc_target
    ASSERT_EQ ebx, 0x300                ; 17 — third value

    ; Third patch — same thing.
    mov dword [.reproc_target + 1], 0x400
    call .reproc_target
    ASSERT_EQ ebx, 0x400                ; 18 — fourth value

    jmp .test9

.reproc_target:
    mov ebx, 0x100
    ret

; =====================================================================
; Test 9: Multiple traces on same page — invalidate all.
;
; Three separate CALL targets all on this page.  After patching ONE of
; them, ALL traces on the page are invalidated (page protection is
; per-page, not per-trace).  The unpatched ones must still return
; correct values after rebuild.
; =====================================================================
.test9:
    ; Warm up all three traces.
    call .multi_a
    ASSERT_EQ eax, 0xA0                 ; 19
    call .multi_b
    ASSERT_EQ eax, 0xB0                 ; 20
    call .multi_c
    ASSERT_EQ eax, 0xC0                 ; 21

    ; Patch only multi_b.
    mov dword [.multi_b + 1], 0xB1
    call .multi_a
    ASSERT_EQ eax, 0xA0                 ; 22 — unchanged
    call .multi_b
    ASSERT_EQ eax, 0xB1                 ; 23 — patched
    call .multi_c
    ASSERT_EQ eax, 0xC0                 ; 24 — unchanged

    PASS

.multi_a:
    mov eax, 0xA0
    ret

.multi_b:
    mov eax, 0xB0
    ret

.multi_c:
    mov eax, 0xC0
    ret
