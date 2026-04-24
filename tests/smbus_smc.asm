; ===========================================================================
; smbus_smc.asm — SMBus SMC (PIC16LC) command tests (--xbox mode).
;
; Tests SMC device (address 0x10) read/write commands:
;   1. SMC version = 0xD0 (v1.0 retail)
;   2. Tray state = 0x60 (closed)
;   3. AV pack type = 0x07 (HDTV)
;   4. CPU temperature = 40 C
;   5. MB temperature = 35 C
;   6. Fan speed read (default ~20)
;   7. Fan speed write + readback
;   8. Scratch register write + readback
;   9. LED override write (accept silently)
;  10. Interrupt reason = 0 (no pending)
;  11. AV type = 0x05
;  12. Challenge bytes all zero
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; SMBus I/O ports
SMB_STATUS  equ 0xC000
SMB_CONTROL equ 0xC002
SMB_ADDRESS equ 0xC004
SMB_DATA    equ 0xC006
SMB_COMMAND equ 0xC008

; Macro: SMBus read from SMC (device 0x10).
; %1 = command byte, result in AL after macro.
%macro SMC_READ 1
    mov al, 0x21
    mov dx, SMB_ADDRESS
    out dx, al
    mov al, %1
    mov dx, SMB_COMMAND
    out dx, al
    mov al, 0x0A
    mov dx, SMB_CONTROL
    out dx, al
    mov dx, SMB_DATA
    in  al, dx
%endmacro

; Macro: SMBus write to SMC (device 0x10).
%macro SMC_WRITE 2
    mov al, 0x20
    mov dx, SMB_ADDRESS
    out dx, al
    mov al, %1
    mov dx, SMB_COMMAND
    out dx, al
    mov al, %2
    mov dx, SMB_DATA
    out dx, al
    mov al, 0x0A
    mov dx, SMB_CONTROL
    out dx, al
%endmacro

; === 1. SMC version ===
    SMC_READ 0x01
    movzx eax, al
    ASSERT_EQ eax, 0xD0

; === 2. Tray state ===
    SMC_READ 0x03
    movzx eax, al
    ASSERT_EQ eax, 0x60

; === 3. AV pack ===
    SMC_READ 0x04
    movzx eax, al
    ASSERT_EQ eax, 0x07

; === 4. CPU temp ===
    SMC_READ 0x09
    movzx eax, al
    ASSERT_EQ eax, 40

; === 5. MB temp ===
    SMC_READ 0x0A
    movzx eax, al
    ASSERT_EQ eax, 35

; === 6. Fan speed default ===
    SMC_READ 0x06
    movzx eax, al
    ASSERT_EQ eax, 20

; === 7. Fan speed write + readback ===
    SMC_WRITE 0x06, 40
    SMC_READ 0x06
    movzx eax, al
    ASSERT_EQ eax, 40

; === 8. Scratch register ===
    SMC_WRITE 0x0E, 0xAB
    SMC_READ 0x0E
    movzx eax, al
    ASSERT_EQ eax, 0xAB

; === 9. LED override write (no crash) ===
    SMC_WRITE 0x07, 0x01
    %assign __test_num __test_num + 1

; === 10. Interrupt reason ===
    SMC_READ 0x11
    movzx eax, al
    ASSERT_EQ eax, 0

; === 11. AV type ===
    SMC_READ 0x0F
    movzx eax, al
    ASSERT_EQ eax, 0x05

; === 12. Challenge bytes ===
    SMC_READ 0x1C
    movzx eax, al
    ASSERT_EQ eax, 0

    PASS
