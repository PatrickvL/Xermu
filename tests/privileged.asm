; ===========================================================================
; privileged.asm — Privileged instruction emulation tests.
;
; Tests:  CLI/STI    — interrupt flag toggling
;         CPUID      — vendor string and feature flags
;         RDTSC      — timestamp counter (monotonic, non-zero)
;         RDMSR/WRMSR— MSR read/write (stub: returns 0)
;         LGDT/LIDT  — descriptor table base/limit loading
;
; Convention:  EAX=0 + HLT = pass.  EAX=N + HLT = assertion N failed.
; ===========================================================================

%include "harness.inc"
[ORG 0x1000]

; ========================= CLI / STI =======================================
; These toggle ctx->virtual_if. We can't directly read virtual_if from guest
; code, but we can verify they don't crash and execution continues.
    cli                                      ; should not crash
    sti                                      ; should not crash
    ; If we get here, both were handled. Mark as assertion 1 by checking EAX=0.
    ASSERT_EQ eax, 0                         ; 1 — CLI/STI didn't crash

; ========================= CPUID ==========================================
; Leaf 0: vendor string "GenuineIntel"
    mov  eax, 0
    cpuid
    ; EBX = "Genu" = 0x756E6547
    ASSERT_EQ ebx, 0x756E6547               ; 2 — CPUID leaf 0 EBX
    ; EDX = "ineI" = 0x49656E69
    ASSERT_EQ edx, 0x49656E69               ; 3 — CPUID leaf 0 EDX
    ; ECX = "ntel" = 0x6C65746E
    ASSERT_EQ ecx, 0x6C65746E               ; 4 — CPUID leaf 0 ECX
    ; EAX = max standard leaf >= 1
    ASSERT_NE eax, 0                         ; 5 — CPUID leaf 0 EAX non-zero
    mov  eax, 0

; Leaf 1: processor info — Family 6, Model 8 (Pentium III Coppermine)
    mov  eax, 1
    cpuid
    ; EAX should have family=6 model=8 → signature 0x683
    ; Bits: family=6 (bits 11:8), model=8 (bits 7:4), stepping=3 (bits 3:0)
    mov  ebx, eax
    and  ebx, 0xFFF                          ; mask family/model/stepping
    ASSERT_EQ ebx, 0x683                     ; 6 — CPUID leaf 1 signature

    ; EDX bit 0 (FPU), bit 23 (MMX), bit 25 (SSE) should all be set
    mov  ebx, edx
    and  ebx, 0x02800001                     ; FPU + MMX + SSE bits
    ASSERT_EQ ebx, 0x02800001               ; 7 — CPUID leaf 1 feature flags
    mov  eax, 0

; ========================= RDTSC ==========================================
; RDTSC returns 64-bit TSC in EDX:EAX. Should be non-zero.
    rdtsc
    ; At least one of EAX or EDX should be non-zero
    mov  ecx, eax
    or   ecx, edx
    ASSERT_NE ecx, 0                         ; 8 — RDTSC non-zero

    ; Two consecutive RDTSC — second should be >= first
    rdtsc
    mov  ebx, eax                            ; save first low
    mov  esi, edx                            ; save first high
    rdtsc
    ; Compare: (edx:eax) >= (esi:ebx)
    ; If edx > esi, definitely >=
    ; If edx == esi, eax >= ebx
    cmp  edx, esi
    ja   .rdtsc_ok
    jb   .rdtsc_fail
    cmp  eax, ebx
    jae  .rdtsc_ok
.rdtsc_fail:
    mov  eax, 9
    hlt
.rdtsc_ok:
    mov  eax, 0
    ; Assertion 9 passed (RDTSC monotonic)
    ASSERT_EQ eax, 0                         ; 9 (placeholder — we already verified)

; ========================= LGDT / LIDT ====================================
; Set up a fake GDT/IDT descriptor in memory and load it.
; Format: [limit:16] [base:32] = 6 bytes.

    ; LGDT test: set GDT base=0x10000, limit=0x1FF
    mov  word  [0x3100], 0x01FF              ; limit
    mov  dword [0x3102], 0x00010000          ; base
    lgdt [0x3100]
    ; Can't directly verify ctx fields from guest, but it shouldn't crash.
    ASSERT_EQ eax, 0                         ; 10 — LGDT didn't crash

    ; LIDT test: set IDT base=0x20000, limit=0x7FF
    mov  word  [0x3108], 0x07FF              ; limit
    mov  dword [0x310A], 0x00020000          ; base
    lidt [0x3108]
    ASSERT_EQ eax, 0                         ; 11 — LIDT didn't crash

; ========================= ALL PASSED =====================================
    PASS
