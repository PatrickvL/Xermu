; ===========================================================================
; pit.asm — 8254 PIT: programming, periodic IRQ, counter read-back.
;
; Requires --xbox mode (PIT I/O ports, PIC, IRQ trigger callback).
;
; Tests:
;   1. Program PIT ch0 mode 2 (rate generator), count=10, verify IRQ fires
;   2. Multiple timer ticks — count ≥3 IRQs in a spin loop
;   3. Reprogram PIT with different count, verify still fires
;   4. Read back counter value (latch command)
;   5. PIT mode 0 (one-shot) — fires once then stops
; ===========================================================================

%include "harness.inc"

IDT_BASE equ 0x3000

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

    ; ---- Program master PIC: remap IRQ 0-7 to vectors 0x40-0x47 ----
    mov al, 0x11        ; ICW1
    out 0x20, al
    mov al, 0x40        ; ICW2: vector base 0x40
    out 0x21, al
    mov al, 0x04        ; ICW3: slave on IRQ 2
    out 0x21, al
    mov al, 0x01        ; ICW4: 8086 mode
    out 0x21, al
    ; Unmask IRQ 0 only.
    mov al, 0xFE
    out 0x21, al

    ; Set up IDT entry for vector 0x40 (IRQ 0 → timer ISR).
    BUILD_IDT_ENTRY 0x40, timer_isr

    ; ================================================================
    ; Test 1: Program PIT ch0 mode 2 (rate generator), count=10.
    ;         Verify at least 1 IRQ fires during a spin loop.
    ; ================================================================
    mov dword [timer_count], 0

    ; PIT command: channel 0, lo/hi access, mode 2, binary.
    mov al, 0x34        ; 00 11 010 0 = ch0, lo/hi, mode 2, binary
    out 0x43, al
    mov al, 10          ; count lo byte = 10
    out 0x40, al
    mov al, 0           ; count hi byte = 0
    out 0x40, al

    ; Enable interrupts.
    sti

    ; Spin until at least 1 timer IRQ fires.
    mov ecx, 50000
.spin1:
    cmp dword [timer_count], 0
    jne .got_irq1
    dec ecx
    jnz .spin1
    ; Timeout — fail.
    mov eax, __test_num + 1
    hlt
.got_irq1:
    %assign __test_num __test_num + 1
    ; timer_count > 0 — pass.

    ; ================================================================
    ; Test 2: Wait for ≥3 timer IRQs.
    ; ================================================================
    mov dword [timer_count], 0
    mov ecx, 200000
.spin2:
    cmp dword [timer_count], 3
    jge .got_irq2
    dec ecx
    jnz .spin2
    mov eax, __test_num + 1
    hlt
.got_irq2:
    %assign __test_num __test_num + 1

    ; ================================================================
    ; Test 3: Reprogram PIT with count=20, verify still fires.
    ; ================================================================
    cli
    mov dword [timer_count], 0

    mov al, 0x34        ; ch0, lo/hi, mode 2, binary
    out 0x43, al
    mov al, 20
    out 0x40, al
    mov al, 0
    out 0x40, al

    sti
    mov ecx, 100000
.spin3:
    cmp dword [timer_count], 0
    jne .got_irq3
    dec ecx
    jnz .spin3
    mov eax, __test_num + 1
    hlt
.got_irq3:
    %assign __test_num __test_num + 1

    ; ================================================================
    ; Test 4: Latch counter and read back.
    ;         After programming count=100, latch and read — value should
    ;         be less than the reload value (counter is counting down).
    ; ================================================================
    cli
    mov al, 0x34
    out 0x43, al
    mov al, 100
    out 0x40, al
    mov al, 0
    out 0x40, al

    ; Latch command: channel 0 (bits 7:6 = 00), access = 00 (latch).
    mov al, 0x00        ; 00 00 xxx x = ch0 latch
    out 0x43, al

    ; Read lo then hi.
    in al, 0x40
    movzx ebx, al
    in al, 0x40
    movzx eax, al
    shl eax, 8
    or ebx, eax         ; EBX = latched counter value

    ; Counter should be 0 < value <= 100.
    cmp ebx, 0
    je .latch_fail
    cmp ebx, 100
    jbe .latch_ok
.latch_fail:
    mov eax, __test_num + 1
    hlt
.latch_ok:
    %assign __test_num __test_num + 1

    ; ================================================================
    ; Test 5: Mode 0 (one-shot) — fires once, then stops.
    ; ================================================================
    mov dword [timer_count], 0

    ; PIT command: ch0, lo/hi, mode 0, binary.
    mov al, 0x30        ; 00 11 000 0 = ch0, lo/hi, mode 0, binary
    out 0x43, al
    mov al, 5           ; count = 5
    out 0x40, al
    mov al, 0
    out 0x40, al

    sti

    ; Wait for 1 IRQ.
    mov ecx, 50000
.spin5a:
    cmp dword [timer_count], 0
    jne .got_oneshot
    dec ecx
    jnz .spin5a
    mov eax, __test_num + 1
    hlt
.got_oneshot:

    ; Save count, spin more — should NOT get another IRQ (one-shot).
    mov edx, [timer_count]
    mov ecx, 1000
.spin5b:
    nop
    dec ecx
    jnz .spin5b

    ; Count should still be the same (no additional IRQs).
    ASSERT_EQ_MEM timer_count, edx

    PASS

; ---------------------------------------------------------------------------
; Timer ISR: increment counter, send EOI.
; ---------------------------------------------------------------------------
timer_isr:
    inc dword [timer_count]
    mov al, 0x20        ; non-specific EOI
    out 0x20, al
    iretd

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
timer_count: dd 0

idt_desc:
    dw 256*8 - 1
    dd IDT_BASE
