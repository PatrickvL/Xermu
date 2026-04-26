; ===========================================================================
; hle.asm — HLE kernel call tests (--xbox mode with HLE handler).
;
; Tests the INT 0x20 HLE mechanism used by the XBE loader:
;   1. KeGetCurrentThread (ordinal 104) — returns non-zero fake KTHREAD ptr
;   2. MmGetPhysicalAddress (ordinal 173) — identity maps VA to PA
;   3. NtClose (ordinal 187) — returns STATUS_SUCCESS (0)
;   4. ExAllocatePool (ordinal 14) — returns non-zero address
;   5. KeQueryPerformanceCounter (ordinal 126) — returns non-zero 64-bit TSC
;   6. KeQueryPerformanceFrequency (ordinal 127) — returns 733333333
;   7. KeQuerySystemTime (ordinal 128) — writes non-zero to buffer
;   8. RtlInitializeCriticalSection (ordinal 291) — returns cleanly
;   9. RtlEnterCriticalSection (ordinal 277) — returns cleanly
;  10. RtlLeaveCriticalSection (ordinal 294) — returns cleanly
;  11. KeCancelTimer (ordinal 97) — returns 0 (not in queue)
;  12. InterlockedIncrement (ordinal 53) — increments [ptr]
;  13. InterlockedDecrement (ordinal 52) — decrements [ptr]
;  14. InterlockedExchange (ordinal 54) — swaps value
;  15. KeSetEvent (ordinal 145) — returns 0 (previous state)
;  16. KeWaitForSingleObject (ordinal 159) — returns STATUS_SUCCESS
;  17. NtCreateEvent (ordinal 189) — writes handle, returns SUCCESS
;  18. RtlZeroMemory (ordinal 320) — zeroes buffer
;  19. RtlFillMemory (ordinal 284) — fills buffer
;  20. RtlCompareMemory (ordinal 268) — returns matching byte count
;  21. MmMapIoSpace (ordinal 177) — returns physical address
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

; === 4. ExAllocatePool (ordinal 14) ===
; stdcall: 1 arg (size). Returns non-zero address.
    push 0x1000                ; arg: 4 KB
    call HLE_STUB(14)
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

; === 7. KeQuerySystemTime (ordinal 128) ===
; stdcall: 1 arg (PLARGE_INTEGER). Writes 8-byte time value.
; We pass a fixed guest-RAM address as the buffer (avoid PUSH ESP).
    mov dword [0x60000], 0     ; clear 8-byte buffer at a known address
    mov dword [0x60004], 0
    push 0x60000               ; arg: pointer to buffer
    call HLE_STUB(128)
    ; Check that at least one dword of the time is non-zero
    mov ecx, [0x60000]
    or  ecx, [0x60004]
    ASSERT_NE ecx, 0

; === 8. RtlInitializeCriticalSection (ordinal 291) ===
; stdcall: 1 arg (PRTL_CRITICAL_SECTION). Just needs to return cleanly.
    push 0x50000               ; arg: fake critical section pointer
    call HLE_STUB(291)
    ; If we get here, it returned OK. Use stack-check: ESP should be correct.
    ; (stdcall cleaned 1 arg = 4 bytes)

; === 9. RtlEnterCriticalSection (ordinal 277) ===
    push 0x50000
    call HLE_STUB(277)

; === 10. RtlLeaveCriticalSection (ordinal 294) ===
    push 0x50000
    call HLE_STUB(294)

; === 11. KeCancelTimer (ordinal 97) ===
; stdcall: 1 arg (PKTIMER). Returns BOOLEAN (0 = not in queue).
    push 0x50100               ; arg: fake timer pointer
    call HLE_STUB(97)
    ASSERT_EQ eax, 0           ; timer was not in queue

; === 12. InterlockedIncrement (ordinal 53) ===
; stdcall: 1 arg (ptr to LONG). Increments and returns new value.
    mov dword [0x60100], 41    ; initial value = 41
    push 0x60100               ; arg: pointer to value
    call HLE_STUB(53)
    ASSERT_EQ eax, 42          ; should return 42 after increment

; === 13. InterlockedDecrement (ordinal 52) ===
; stdcall: 1 arg (ptr to LONG). Decrements and returns new value.
    mov dword [0x60100], 10
    push 0x60100
    call HLE_STUB(52)
    ASSERT_EQ eax, 9           ; should return 9 after decrement

; === 14. InterlockedExchange (ordinal 54) ===
; stdcall: 2 args (ptr, new value). Returns old value.
    mov dword [0x60100], 0xAA
    push 0xBB                  ; new value
    push 0x60100               ; target pointer
    call HLE_STUB(54)
    ASSERT_EQ eax, 0xAA        ; returns old value
    mov eax, [0x60100]
    ASSERT_EQ eax, 0xBB        ; memory now has new value

; === 15. KeSetEvent (ordinal 145) ===
; stdcall: 3 args. Returns previous state (0).
    push 0                     ; Wait = FALSE
    push 0                     ; Increment
    push 0x50200               ; PKEVENT
    call HLE_STUB(145)
    ASSERT_EQ eax, 0           ; previous state = not-signaled

; === 16. KeWaitForSingleObject (ordinal 159) ===
; stdcall: 5 args. Returns STATUS_SUCCESS (0).
    push 0                     ; Timeout = NULL
    push 0                     ; Alertable = FALSE
    push 0                     ; ProcessorMode
    push 0                     ; WaitReason
    push 0x50200               ; Object
    call HLE_STUB(159)
    ASSERT_EQ eax, 0           ; STATUS_SUCCESS

; === 17. NtCreateEvent (ordinal 189) ===
; stdcall: 5 args. Writes handle to first arg pointer.
    mov dword [0x60200], 0     ; clear handle location
    push 0                     ; InitialState
    push 0                     ; EventType
    push 0                     ; ObjectAttributes
    push 0                     ; DesiredAccess
    push 0x60200               ; PHANDLE
    call HLE_STUB(189)
    ASSERT_EQ eax, 0           ; STATUS_SUCCESS
    mov eax, [0x60200]
    ASSERT_NE eax, 0           ; handle should be non-zero

; === 18. RtlZeroMemory (ordinal 320) ===
; stdcall: 2 args (dest, length). Zeroes memory.
    mov dword [0x60300], 0xDEADBEEF
    mov dword [0x60304], 0xCAFEBABE
    push 8                     ; length
    push 0x60300               ; dest
    call HLE_STUB(320)
    mov eax, [0x60300]
    ASSERT_EQ eax, 0
    mov eax, [0x60304]
    ASSERT_EQ eax, 0

; === 19. RtlFillMemory (ordinal 284) ===
; stdcall: 3 args (dest, length, fill byte).
    push 0xAA                  ; fill byte
    push 4                     ; length
    push 0x60300               ; dest
    call HLE_STUB(284)
    mov eax, [0x60300]
    ASSERT_EQ eax, 0xAAAAAAAA  ; 4 bytes filled with 0xAA

; === 20. RtlCompareMemory (ordinal 268) ===
; stdcall: 3 args (src1, src2, length). Returns matching count.
    mov dword [0x60400], 0x11223344
    mov dword [0x60410], 0x11223344   ; identical
    mov byte  [0x60412], 0xFF        ; differs at byte 2 (byte index 2 of the dword)
    push 4                     ; length = 4 bytes
    push 0x60410               ; src2
    push 0x60400               ; src1
    call HLE_STUB(268)
    ASSERT_EQ eax, 2           ; first 2 bytes match (0x44, 0x33), byte 2 differs

; === 21. MmMapIoSpace (ordinal 177) ===
; stdcall: 3 args. Returns physical address (identity-mapped).
    push 0                     ; CacheType
    push 0x1000                ; Size
    push 0xFD000000            ; PhysAddr (NV2A base)
    call HLE_STUB(177)
    ASSERT_EQ eax, 0xFD000000  ; returns same address

    PASS
