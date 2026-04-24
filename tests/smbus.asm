; ===========================================================================
; smbus.asm — SMBus controller tests (--xbox mode).
;
; Tests the I/O port interface at 0xC000-0xC00F:
;   1. Read EEPROM game region (offset 0x2C) = 0x01 (NTSC-NA)
;   2. Read EEPROM serial number byte (offset 0x34) = '0' (0x30)
;   3. Read EEPROM language (offset 0xE8) = 0x01 (English)
;   4. Read SMC version (device 0x10, command 0x01) = 0xD0
;   5. Read SMC CPU temperature (device 0x10, command 0x09) = 25
;   6. Write EEPROM byte, read it back
;   7. Status register W1C (write-1-to-clear)
;
; SMBus I/O protocol:
;   Port+0x04 (ADDRESS): device address byte (7-bit addr << 1 | R/W bit)
;   Port+0x08 (COMMAND): register/offset byte
;   Port+0x06 (DATA):    data byte (read result or write value)
;   Port+0x02 (CONTROL): write to trigger transaction
;   Port+0x00 (STATUS):  bit 4 = done, W1C
; ===========================================================================

%include "harness.inc"

ORG 0x1000

%define SMBUS 0xC000

; Macro: SMBus read byte from device/command, result in AL.
; Clobbers EAX, EDX.
%macro SMBUS_READ 2 ; %1=device_addr_7bit, %2=command_byte
    mov dx, SMBUS + 0x04
    mov al, ((%1) << 1) | 1    ; address byte: addr<<1 | READ bit
    out dx, al
    mov dx, SMBUS + 0x08
    mov al, %2                  ; command/offset
    out dx, al
    mov dx, SMBUS + 0x02
    mov al, 0x0A                ; trigger read-byte protocol
    out dx, al
    ; Read result
    mov dx, SMBUS + 0x06
    in  al, dx
    movzx eax, al
%endmacro

; Macro: SMBus write byte to device/command.
; Clobbers EDX.
%macro SMBUS_WRITE 3 ; %1=device_addr_7bit, %2=command_byte, %3=data_byte
    mov dx, SMBUS + 0x04
    mov al, ((%1) << 1)        ; address byte: addr<<1 | WRITE bit
    out dx, al
    mov dx, SMBUS + 0x08
    mov al, %2                  ; command/offset
    out dx, al
    mov dx, SMBUS + 0x06
    mov al, %3                  ; data to write
    out dx, al
    mov dx, SMBUS + 0x02
    mov al, 0x0A                ; trigger write-byte protocol
    out dx, al
%endmacro

; === 1. EEPROM game region (offset 0x2C) = 0x01 ===
    SMBUS_READ 0x54, 0x2C
    ASSERT_EQ eax, 0x01         ; NTSC-NA region

; === 2. EEPROM serial number first byte (offset 0x34) = '0' = 0x30 ===
    SMBUS_READ 0x54, 0x34
    ASSERT_EQ eax, 0x30         ; ASCII '0'

; === 3. EEPROM language (offset 0xE8) = 0x01 (English) ===
    SMBUS_READ 0x54, 0xE8
    ASSERT_EQ eax, 0x01

; === 4. SMC version (device 0x10, cmd 0x01) = 0xD0 ===
    SMBUS_READ 0x10, 0x01
    ASSERT_EQ eax, 0xD0

; === 5. SMC CPU temperature (device 0x10, cmd 0x09) = 40 ===
    SMBUS_READ 0x10, 0x09
    ASSERT_EQ eax, 40

; === 6. Write EEPROM byte, read it back ===
; Write 0x42 to EEPROM offset 0xF0 (audio flags — safe to modify)
    SMBUS_WRITE 0x54, 0xF0, 0x42
; Read it back
    SMBUS_READ 0x54, 0xF0
    ASSERT_EQ eax, 0x42

; === 7. Status register W1C ===
; After a transaction, status bit 4 (0x10) should be set (done).
; Reading status should return 0x10.
    mov dx, SMBUS
    in  al, dx
    movzx ecx, al
    ASSERT_EQ ecx, 0x10         ; done bit set
; Write 0x10 to clear the done bit.
    mov al, 0x10
    out dx, al
    in  al, dx
    movzx ecx, al
    ASSERT_EQ ecx, 0x00         ; done bit cleared

    PASS
