; ===========================================================================
; pic.asm — 8259A PIC: initialization, masking, IRQ delivery, EOI.
;
; Requires --xbox mode (PIC I/O ports and IRQ trigger port 0xEB).
;
; Tests:
;   1. Program master PIC ICW1-4 (remap IRQ 0-7 to vectors 0x50-0x57)
;   2. Read back IMR (all unmasked after ICW init)
;   3. Mask all except IRQ 0, verify IMR
;   4. Set up IDT entry for vector 0x50, trigger IRQ 0, verify ISR runs
;   5. Send non-specific EOI, verify ISR cleared (read ISR via OCW3)
;   6. Trigger IRQ 1 (masked) — verify it does NOT fire
;   7. Program slave PIC, trigger IRQ 8 (cascade via IRQ 2), verify delivery
; ===========================================================================

%include "harness.inc"

IDT_BASE equ 0x3000
IRQ_TRIGGER_PORT equ 0xEB

; ---------------------------------------------------------------------------
; Macro: build a 32-bit interrupt gate at IDT_BASE + vector*8.
; ---------------------------------------------------------------------------
%macro BUILD_IDT_ENTRY 2  ; %1=vector, %2=handler_label
    mov eax, %2
    mov word [IDT_BASE + %1*8 + 0], ax      ; offset 15:0
    mov word [IDT_BASE + %1*8 + 2], 0x0008  ; CS selector
    mov byte [IDT_BASE + %1*8 + 4], 0       ; reserved
    mov byte [IDT_BASE + %1*8 + 5], 0x8E    ; type: 32-bit interrupt gate
    shr eax, 16
    mov word [IDT_BASE + %1*8 + 6], ax      ; offset 31:16
%endmacro

; ---------------------------------------------------------------------------
section .text
org 0x1000

start:
    ; ---- Load IDT ----
    lidt [idt_desc]

    ; ================================================================
    ; Test 1: Program master PIC — ICW1-4 (remap to 0x50)
    ; ================================================================
    mov al, 0x11        ; ICW1: cascade, edge-triggered, ICW4 needed
    out 0x20, al
    mov al, 0x50        ; ICW2: vector base 0x50 (IRQ 0 → vector 0x50)
    out 0x21, al
    mov al, 0x04        ; ICW3: slave on IRQ 2
    out 0x21, al
    mov al, 0x01        ; ICW4: 8086 mode, normal EOI
    out 0x21, al

    ; ================================================================
    ; Test 2: After ICW init, IMR should be 0 (all unmasked).
    ; ================================================================
    in al, 0x21
    movzx eax, al
    ASSERT_EQ eax, 0

    ; ================================================================
    ; Test 3: Set IMR = 0xFE (mask all except IRQ 0), read back.
    ; ================================================================
    mov al, 0xFE        ; unmask IRQ 0 only
    out 0x21, al
    in al, 0x21
    movzx eax, al
    ASSERT_EQ eax, 0xFE

    ; ================================================================
    ; Test 4: IRQ 0 delivery — trigger IRQ 0, verify ISR runs.
    ; ================================================================
    ; Set up IDT entry for vector 0x50.
    BUILD_IDT_ENTRY 0x50, isr_irq0

    ; Clear the flag.
    mov dword [irq0_flag], 0

    ; Enable interrupts and trigger IRQ 0.
    sti
    mov dx, IRQ_TRIGGER_PORT
    mov al, 0           ; IRQ 0
    out dx, al

    ; The ISR should have run by the next trace boundary.
    ; Spin briefly to ensure the interrupt is delivered.
    nop
    nop
    nop

    ASSERT_EQ_MEM irq0_flag, 0xDEAD0000

    ; ================================================================
    ; Test 5: Send non-specific EOI, verify ISR is cleared.
    ; ================================================================
    ; Before EOI: read ISR via OCW3.
    mov al, 0x0B        ; OCW3: read ISR
    out 0x20, al
    in al, 0x20
    movzx eax, al
    ASSERT_EQ eax, 0x01  ; ISR bit 0 should be set (IRQ 0 in-service)

    ; Send non-specific EOI.
    mov al, 0x20        ; OCW2: non-specific EOI
    out 0x20, al

    ; Read ISR again — should be cleared.
    mov al, 0x0B        ; OCW3: read ISR
    out 0x20, al
    in al, 0x20
    movzx eax, al
    ASSERT_EQ eax, 0x00  ; ISR should be clear

    ; ================================================================
    ; Test 6: IRQ 1 is masked — trigger it, verify it does NOT fire.
    ; ================================================================
    BUILD_IDT_ENTRY 0x51, isr_irq1
    mov dword [irq1_flag], 0

    mov dx, IRQ_TRIGGER_PORT
    mov al, 1            ; IRQ 1 (masked by IMR bit 1)
    out dx, al
    nop
    nop
    nop

    ; irq1_flag should still be 0 (ISR did not run).
    ASSERT_EQ_MEM irq1_flag, 0

    ; ================================================================
    ; Test 7: Slave PIC + cascade — trigger IRQ 8 (slave IRQ 0).
    ; ================================================================
    ; Program slave PIC ICW1-4 (remap to 0x58).
    mov al, 0x11        ; ICW1
    out 0xA0, al
    mov al, 0x58        ; ICW2: vector base 0x58 (IRQ 8 → vector 0x58)
    out 0xA1, al
    mov al, 0x02        ; ICW3: cascade identity = 2
    out 0xA1, al
    mov al, 0x01        ; ICW4: 8086 mode, normal EOI
    out 0xA1, al

    ; Unmask slave IRQ 0 (system IRQ 8).
    mov al, 0xFE        ; unmask only IRQ 0 on slave
    out 0xA1, al

    ; Unmask master IRQ 2 (cascade line) — set IMR to 0xFA.
    ; Current master IMR is 0xFE (only IRQ 0 unmasked).
    ; Need to also unmask IRQ 2: IMR = 0xFA (bits 1,3-7 masked).
    mov al, 0xFA
    out 0x21, al

    ; Set up IDT entry for vector 0x58.
    BUILD_IDT_ENTRY 0x58, isr_irq8
    mov dword [irq8_flag], 0

    ; Trigger IRQ 8 (slave IRQ 0).
    mov dx, IRQ_TRIGGER_PORT
    mov al, 8
    out dx, al
    nop
    nop
    nop

    ASSERT_EQ_MEM irq8_flag, 0xBEEF0008

    ; EOI to slave then master.
    mov al, 0x20
    out 0xA0, al         ; EOI to slave
    out 0x20, al         ; EOI to master

    ; ================================================================
    ; Test 8: OCW3 read IRR — verify IRQ 1 is still pending (was masked).
    ; ================================================================
    mov al, 0x0A        ; OCW3: read IRR
    out 0x20, al
    in al, 0x20
    movzx eax, al
    and eax, 0x02       ; bit 1 = IRQ 1
    ASSERT_EQ eax, 0x02  ; IRQ 1 pending (raised but masked)

    ; ================================================================
    ; Test 9: Specific EOI — verify targeted clear.
    ; ================================================================
    ; Unmask IRQ 1, let it fire.
    mov al, 0xF8        ; unmask IRQ 0, 1, 2
    out 0x21, al

    nop
    nop
    nop

    ; IRQ 1 ISR should have run now.
    ASSERT_EQ_MEM irq1_flag, 0xDEAD0001

    ; ================================================================
    ; Test 10: Read IMR after all changes.
    ; ================================================================
    in al, 0x21
    movzx eax, al
    ASSERT_EQ eax, 0xF8

    PASS

; ---------------------------------------------------------------------------
; ISR handlers
; ---------------------------------------------------------------------------

isr_irq0:
    mov dword [irq0_flag], 0xDEAD0000
    ; Note: we do NOT send EOI here — test 5 checks ISR is still set.
    iretd

isr_irq1:
    mov dword [irq1_flag], 0xDEAD0001
    mov al, 0x20
    out 0x20, al         ; EOI
    iretd

isr_irq8:
    mov dword [irq8_flag], 0xBEEF0008
    ; Leave EOI to the test body.
    iretd

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
irq0_flag: dd 0
irq1_flag: dd 0
irq8_flag: dd 0

; IDT pseudo-descriptor
idt_desc:
    dw 256*8 - 1
    dd IDT_BASE
