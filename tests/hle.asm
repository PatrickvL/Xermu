; ===========================================================================
; hle.asm — HLE kernel call tests (--xbox mode with HLE handler).
;
; Tests the INT 0x20 HLE mechanism used by the XBE loader:
;   1. KeGetCurrentThread (ordinal 104) — returns non-zero fake KTHREAD ptr
;   2. MmGetPhysicalAddress (ordinal 173) — identity maps VA to PA
;   3. NtClose (ordinal 187) — returns STATUS_SUCCESS (0)
;   4. HalReturnToFirmware (ordinal 49) — halts execution
;
; Convention: these tests call the HLE stubs directly (not through a thunk
; table). The HLE stub at address 0x80000 + ordinal * 8 does:
;   MOV EAX, ordinal
;   INT 0x20
;   RET
;
; We set up a stack and CALL the stub addresses.
; ===========================================================================

%include "harness.inc"

ORG 0x1000

; HLE stub base address (must match xbe::HLE_STUB_BASE)
%define HLE_BASE 0x00080000

; Stub address for a given ordinal
%define HLE_STUB(ord) (HLE_BASE + (ord) * 8)

; === 1. KeGetCurrentThread (ordinal 104) ===
; Returns a fake KTHREAD pointer (should be non-zero)
    call HLE_STUB(104)
    ASSERT_NE eax, 0           ; should return non-zero KTHREAD pointer

; === 2. MmGetPhysicalAddress (ordinal 173) ===
; stdcall: 1 arg (VA). Returns PA = VA (identity mapping).
    push 0x00012345            ; arg: VA
    call HLE_STUB(173)
    ASSERT_EQ eax, 0x00012345  ; PA should equal VA

; === 3. NtClose (ordinal 187) ===
; stdcall: 1 arg (handle). Returns STATUS_SUCCESS = 0.
    push 0x42                  ; arg: handle
    call HLE_STUB(187)
    ASSERT_EQ eax, 0           ; STATUS_SUCCESS

; === 4. ExAllocatePool (ordinal 15) ===
; stdcall: 1 arg (size). Returns non-zero address.
    push 0x1000                ; arg: 4 KB
    call HLE_STUB(15)
    ASSERT_NE eax, 0           ; should return valid allocation

    PASS
