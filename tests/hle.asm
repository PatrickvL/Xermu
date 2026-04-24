; ===========================================================================
; hle.asm — HLE kernel call tests (--xbox mode with HLE handler).
;
; Tests the INT 0x20 HLE mechanism used by the XBE loader:
;   1. KeGetCurrentThread (ordinal 104) — returns non-zero fake KTHREAD ptr
;   2. MmGetPhysicalAddress (ordinal 173) — identity maps VA to PA
;   3. NtClose (ordinal 187) — returns STATUS_SUCCESS (0)
;   4. ExAllocatePool (ordinal 15) — returns non-zero address
;   5. KeQueryPerformanceCounter (ordinal 126) — returns non-zero 64-bit TSC
;   6. KeQueryPerformanceFrequency (ordinal 127) — returns 733333333
;   7. KeQuerySystemTime (ordinal 131) — writes non-zero to buffer
;   8. RtlInitializeCriticalSection (ordinal 270) — returns cleanly
;   9. RtlEnterCriticalSection (ordinal 264) — returns cleanly
;  10. RtlLeaveCriticalSection (ordinal 267) — returns cleanly
;  11. KeCancelTimer (ordinal 96) — returns 0 (not in queue)
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

; === 5. KeQueryPerformanceCounter (ordinal 126) ===
; cdecl-ish: no args. Returns 64-bit counter in EDX:EAX.
    call HLE_STUB(126)
    ; At least one of EDX:EAX should be non-zero (host TSC is huge)
    mov ecx, eax
    or  ecx, edx
    ASSERT_NE ecx, 0           ; counter should be non-zero

; === 6. KeQueryPerformanceFrequency (ordinal 127) ===
; Returns 64-bit frequency in EDX:EAX.  Low 32 bits = 733333333 = 0x2BB59865.
    call HLE_STUB(127)
    ASSERT_EQ eax, 0x2BB5C755  ; 733333333 & 0xFFFFFFFF

; === 7. KeQuerySystemTime (ordinal 131) ===
; stdcall: 1 arg (PLARGE_INTEGER). Writes 8-byte time value.
; We pass a fixed guest-RAM address as the buffer (avoid PUSH ESP).
    mov dword [0x60000], 0     ; clear 8-byte buffer at a known address
    mov dword [0x60004], 0
    push 0x60000               ; arg: pointer to buffer
    call HLE_STUB(131)
    ; Check that at least one dword of the time is non-zero
    mov ecx, [0x60000]
    or  ecx, [0x60004]
    ASSERT_NE ecx, 0

; === 8. RtlInitializeCriticalSection (ordinal 270) ===
; stdcall: 1 arg (PRTL_CRITICAL_SECTION). Just needs to return cleanly.
    push 0x50000               ; arg: fake critical section pointer
    call HLE_STUB(270)
    ; If we get here, it returned OK. Use stack-check: ESP should be correct.
    ; (stdcall cleaned 1 arg = 4 bytes)

; === 9. RtlEnterCriticalSection (ordinal 264) ===
    push 0x50000
    call HLE_STUB(264)

; === 10. RtlLeaveCriticalSection (ordinal 267) ===
    push 0x50000
    call HLE_STUB(267)

; === 11. KeCancelTimer (ordinal 96) ===
; stdcall: 1 arg (PKTIMER). Returns BOOLEAN (0 = not in queue).
    push 0x50100               ; arg: fake timer pointer
    call HLE_STUB(96)
    ASSERT_EQ eax, 0           ; timer was not in queue

    PASS
