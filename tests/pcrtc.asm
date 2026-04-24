; ===========================================================================
; pcrtc.asm — NV2A PCRTC vblank interrupt test (--xbox mode).
;
; Tests:
;   1. PCRTC_INTR_0 is initially 0 (no vblank pending)
;   2. Enable PCRTC vblank interrupt + PMC master enable
;   3. Wait for vblank — PCRTC_INTR_0 bit 0 becomes set
;   4. Clear vblank by writing 1 to PCRTC_INTR_0 (W1C)
;   5. Vblank IRQ delivered to PIC (IRQ 1 → vector), ISR runs
; ===========================================================================

%include "harness.inc"

IDT_BASE equ 0x3000
NV2A     equ 0xFD000000

; NV2A register offsets
PMC_INTR_0      equ NV2A + 0x000100
PMC_INTR_EN_0   equ NV2A + 0x000140
PCRTC_INTR_0    equ NV2A + 0x600100
PCRTC_INTR_EN_0 equ NV2A + 0x600140

; ---------------------------------------------------------------------------
%macro BUILD_IDT_ENTRY 2
    mov eax, %2
    mov word [IDT_BASE + %1*8 + 0], ax
    mov word [IDT_BASE + %1*8 + 2], 0x0008
    mov byte [IDT_BASE + %1*8 + 4], 0
    mov byte [IDT_BASE + %1*8 + 5], 0x8E
    shr eax, 16
    mov word [IDT_BASE + %1*8 + 6], ax
%endmacro

section .text
org 0x1000

start:
    lidt [idt_desc]

    ; ---- Program master PIC: remap to 0x40 ----
    mov al, 0x11
    out 0x20, al
    mov al, 0x40        ; IRQ 0 → vec 0x40, IRQ 1 → vec 0x41
    out 0x21, al
    mov al, 0x04
    out 0x21, al
    mov al, 0x01
    out 0x21, al
    ; Unmask IRQ 0 and IRQ 1 (bits 0,1 = 0, rest masked).
    mov al, 0xFC
    out 0x21, al

    ; ================================================================
    ; Test 1: PCRTC_INTR_0 initially 0.
    ; ================================================================
    mov eax, [PCRTC_INTR_0]
    ASSERT_EQ eax, 0

    ; ================================================================
    ; Test 2: Enable vblank interrupt.
    ; ================================================================
    ; Enable PCRTC vblank (bit 0).
    mov dword [PCRTC_INTR_EN_0], 1
    ; Enable PMC master interrupt for PCRTC (bit 24).
    mov dword [PMC_INTR_EN_0], 0x01000001   ; bit 0=master enable, bit 24=PCRTC

    ; Read back to verify.
    mov eax, [PCRTC_INTR_EN_0]
    ASSERT_EQ eax, 1

    ; ================================================================
    ; Test 3: Wait for vblank — PCRTC_INTR_0 bit 0 set.
    ; ================================================================
    ; Spin until vblank fires (should happen within VBLANK_PERIOD ticks).
    mov ecx, 200000
.spin_vblank:
    mov eax, [PCRTC_INTR_0]
    test eax, 1
    jnz .got_vblank
    dec ecx
    jnz .spin_vblank
    mov eax, __test_num + 1
    hlt
.got_vblank:
    %assign __test_num __test_num + 1

    ; ================================================================
    ; Test 4: Clear vblank via W1C (write-1-to-clear).
    ; ================================================================
    mov dword [PCRTC_INTR_0], 1     ; clear bit 0
    mov eax, [PCRTC_INTR_0]
    ASSERT_EQ eax, 0

    ; ================================================================
    ; Test 5: Vblank IRQ delivered as hardware interrupt.
    ;         IRQ 1 → PIC vector 0x41.
    ; ================================================================
    BUILD_IDT_ENTRY 0x41, vblank_isr
    mov dword [vblank_flag], 0

    ; Re-enable vblank and unmask interrupts.
    mov dword [PCRTC_INTR_EN_0], 1
    sti

    ; Spin until ISR runs.
    mov ecx, 200000
.spin_irq:
    cmp dword [vblank_flag], 0
    jne .got_irq
    dec ecx
    jnz .spin_irq
    mov eax, __test_num + 1
    hlt
.got_irq:
    %assign __test_num __test_num + 1

    ; Verify ISR ran.
    ASSERT_EQ_MEM vblank_flag, 0xBEEFBEEF

    PASS

; ---------------------------------------------------------------------------
; Vblank ISR: set flag, clear PCRTC interrupt, send EOI to PIC.
; ---------------------------------------------------------------------------
vblank_isr:
    mov dword [vblank_flag], 0xBEEFBEEF
    ; Clear the NV2A PCRTC interrupt.
    mov dword [PCRTC_INTR_0], 1
    ; EOI to PIC.
    mov al, 0x20
    out 0x20, al
    iretd

; ---------------------------------------------------------------------------
vblank_flag: dd 0

idt_desc:
    dw 256*8 - 1
    dd IDT_BASE
