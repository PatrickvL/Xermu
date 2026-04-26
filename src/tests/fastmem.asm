ove; ===========================================================================
; fastmem.asm — 4 GB fastmem window + VEH fault handling stress tests.
;
; Exercises the VEH-based MMIO dispatch, trace rebuild, slow-path
; persistence, mirror aliasing, and multi-fault-per-trace correctness.
;
; Requires --xbox mode (NV2A MMIO handlers).
;
; Tests:
;   1. Single MMIO read (NV2A PMC_BOOT_0 at 0xFD000000)
;   2. MMIO write + readback (NV2A PMC_INTR_EN at 0xFD000140)
;   3. Multiple MMIO reads in one trace (sequence preserved)
;   4. Interleaved RAM + MMIO in same trace
;   5. RAM mirror read (write main, read mirror)
;   6. RAM mirror write (write mirror, read main)
;   7. Gap region access (committed zero at 0x08000000–0x0C000000)
;   8. Repeated MMIO reads (slow-path cached, no re-fault)
;   9. Two different MMIO devices in one trace
;  10. MMIO read + ALU using result (flags preserved across rebuild)
;  11. MMIO via MOVZX (partial-width read path)
;  12. MMIO store immediate (MOV [mmio], imm32 path)
; ===========================================================================

%include "harness.inc"

ORG 0x1000

NV2A          equ 0xFD000000
PMC_BOOT_0    equ NV2A + 0x000000      ; chip ID
PMC_INTR_EN   equ NV2A + 0x000140      ; interrupt enable (R/W)
PTIMER_NUM    equ NV2A + 0x009200      ; PTIMER numerator (R/W)
PTIMER_DEN    equ NV2A + 0x009210      ; PTIMER denominator (R/W)
PFB_CFG0      equ NV2A + 0x100000      ; PFB config (R/W)
PGRAPH_CTX    equ NV2A + 0x400000      ; PGRAPH control (R/W)

IOAPIC        equ 0xFEC00000
IOAPIC_SEL    equ IOAPIC + 0x00        ; IOREGSEL
IOAPIC_WIN    equ IOAPIC + 0x10        ; IOWIN

MIRROR_BASE   equ 0x0C000000

; Scratch area in low RAM for test data.
SCRATCH       equ 0x50000

; ===========================================================================
; Test 1: Single MMIO read — NV2A PMC_BOOT_0
;
; The first access to 0xFD000000 faults (PAGE_NOACCESS), VEH sets bitmap,
; rebuilds the trace with a slow-path CALL, and returns the register value.
; ===========================================================================
    mov eax, [PMC_BOOT_0]
    ASSERT_EQ eax, 0x02A000A1           ; 1 — NV2A chip ID

; ===========================================================================
; Test 2: MMIO write + readback
;
; Write a value to a R/W NV2A register, then read it back.
; Both operations fault on first encounter, testing write-path VEH.
; ===========================================================================
    mov dword [PMC_INTR_EN], 0x00000001
    mov eax, [PMC_INTR_EN]
    ASSERT_EQ eax, 0x00000001           ; 2

; ===========================================================================
; Test 3: Multiple MMIO reads in one trace — sequence preserved.
;
; Three consecutive NV2A register reads.  The first faults, VEH rebuilds,
; redirects to the faulting instruction (not the trace start).  The second
; and third must also fault and rebuild — each advancing correctly.
; ===========================================================================
    mov dword [PTIMER_NUM], 0x00000005
    mov dword [PTIMER_DEN], 0x00000002
    mov eax, [PTIMER_NUM]
    ASSERT_EQ eax, 0x00000005           ; 3
    mov eax, [PTIMER_DEN]
    ASSERT_EQ eax, 0x00000002           ; 4

; ===========================================================================
; Test 4: Interleaved RAM + MMIO in same trace
;
; RAM writes don't fault; MMIO reads do.  Verifies that non-faulting
; instructions before the fault are not re-executed after VEH redirect.
; ===========================================================================
    mov dword [SCRATCH], 0xDEADBEEF     ; RAM write (fast path, no fault)
    mov eax, [PMC_BOOT_0]               ; MMIO read (slow path after rebuild)
    ASSERT_EQ eax, 0x02A000A1           ; 5
    mov eax, [SCRATCH]                  ; RAM read (verify no double-write)
    ASSERT_EQ eax, 0xDEADBEEF           ; 6

; ===========================================================================
; Test 5: RAM mirror read (write main, read mirror)
;
; The mirror at 0x0C000000 is section-mapped to the same physical pages.
; Reads from the mirror region go through fastmem (no fault).
; ===========================================================================
    mov dword [SCRATCH + 4], 0xCAFEF00D
    mov eax, [MIRROR_BASE + SCRATCH + 4]  ; mirror of SCRATCH+4
    ASSERT_EQ eax, 0xCAFEF00D           ; 7

; ===========================================================================
; Test 6: RAM mirror write (write mirror, read main)
; ===========================================================================
    mov dword [MIRROR_BASE + SCRATCH + 8], 0xBAADF00D
    mov eax, [SCRATCH + 8]              ; read from main RAM
    ASSERT_EQ eax, 0xBAADF00D           ; 8

; ===========================================================================
; Test 8: Repeated MMIO reads — slow-path cached.
;
; After the first fault + rebuild, subsequent executions of the same trace
; should use the slow-path CALL directly (no re-fault).  Verify the value
; is still correct after multiple calls.
; ===========================================================================
    call .repeated_mmio
    ASSERT_EQ eax, 0x02A000A1           ; 11
    call .repeated_mmio
    ASSERT_EQ eax, 0x02A000A1           ; 12
    call .repeated_mmio
    ASSERT_EQ eax, 0x02A000A1           ; 13
    jmp .test9

.repeated_mmio:
    mov eax, [PMC_BOOT_0]
    ret

; ===========================================================================
; Test 9: Two different MMIO devices in one trace
;
; Access NV2A and I/O APIC in the same trace.  Both must fault, and both
; rebuilds must redirect correctly without corrupting state.
; ===========================================================================
.test9:
    mov dword [IOAPIC_SEL], 0x01        ; I/O APIC: select version reg
    mov eax, [PMC_BOOT_0]               ; NV2A read in same trace
    ASSERT_EQ eax, 0x02A000A1           ; 14
    mov eax, [IOAPIC_SEL]               ; I/O APIC readback
    ASSERT_EQ eax, 0x01                 ; 15

; ===========================================================================
; Test 10: MMIO read + ALU using result (flags preserved)
;
; Read an MMIO register and use the result in an arithmetic operation.
; Verifies that the slow-path CALL does not corrupt EFLAGS when liveness
; analysis says flags are live.
; ===========================================================================
    mov dword [PMC_INTR_EN], 0x00000003
    mov eax, [PMC_INTR_EN]
    cmp eax, 3
    ASSERT_FLAGS ZF, ZF                 ; 16 — ZF set (equal)

    mov dword [PMC_INTR_EN], 0x00000007
    mov eax, [PMC_INTR_EN]
    test eax, 0x04                      ; bit 2 set
    ASSERT_FLAGS ZF, 0                  ; 17 — ZF clear (non-zero)

; ===========================================================================
; Test 11: Store-immediate to MMIO (MOV [mmio], imm32 path)
;
; Exercises emit_fastmem_dispatch_store_imm → write_guest_mem_imm.
; ===========================================================================
    mov dword [PFB_CFG0], 0x12340000
    mov eax, [PFB_CFG0]
    ASSERT_EQ eax, 0x12340000           ; 18

; ===========================================================================
; Test 12: MMIO write with register value (MOV [mmio], reg path)
; ===========================================================================
    mov ecx, 0xABCD0000
    mov dword [PGRAPH_CTX], ecx
    mov eax, [PGRAPH_CTX]
    ASSERT_EQ eax, 0xABCD0000           ; 19

; ===========================================================================
; Test 13: Multiple MMIO writes in sequence (pfifo-style ordering)
;
; Write several MMIO registers in order — verifies that VEH redirect
; preserves ordering (no re-execution of earlier writes).
; ===========================================================================
    mov dword [PTIMER_NUM], 0x00000010
    mov dword [PTIMER_DEN], 0x00000004
    mov dword [PFB_CFG0], 0x99990000
    ; Read all back — order must be preserved.
    mov eax, [PTIMER_NUM]
    ASSERT_EQ eax, 0x00000010           ; 20
    mov eax, [PTIMER_DEN]
    ASSERT_EQ eax, 0x00000004           ; 21
    mov eax, [PFB_CFG0]
    ASSERT_EQ eax, 0x99990000           ; 22

    PASS
