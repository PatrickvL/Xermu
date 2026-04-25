; dispatch_trace.asm — MASM x64 trampoline for Windows
;
; void dispatch_trace(GuestContext* ctx, void* host_code)
; Windows x64 calling convention: RCX = ctx, RDX = host_code
;
; Pinned registers inside a trace:
;   R12 = fastmem_base   (callee-saved)
;   R13 = GuestContext*   (callee-saved)
;   R14 = EA scratch      (callee-saved — we push/pop it ourselves)
;   R15 = unused          (callee-saved — push/pop for ABI compliance)
;
; Guest GP registers live in host EAX–EDI while a trace runs.
; ESP is NOT mapped to host RSP; it stays in ctx->gp[GP_ESP].
;
; FPU/SSE state is saved/restored via FXSAVE/FXRSTOR on entry/exit.

CTX_GUEST_FPU EQU 128
CTX_HOST_FPU  EQU 640

.code

dispatch_trace PROC
    ; Save callee-saved registers
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    push    rdi
    push    rsi

    ; Windows x64 args: RCX = ctx, RDX = host_code
    mov     rdi, rcx            ; normalise: ctx -> RDI
    mov     rsi, rdx            ; host_code -> RSI

    ; Set up pinned executor registers
    mov     r13, rdi                        ; R13 = ctx
    mov     r12, QWORD PTR [r13+48]        ; R12 = ctx->fastmem_base
    ; R15 is no longer used (was ram_size); push/pop for ABI compliance only.
    mov     r14, rsi                        ; Stash host_code in R14

    ; Save host FPU/SSE state, load guest FPU/SSE state
    lea     rax, [r13 + CTX_HOST_FPU]
    fxsave  [rax]
    lea     rax, [r13 + CTX_GUEST_FPU]
    fxrstor [rax]

    ; Load guest GP registers into host registers
    ; (ESP intentionally skipped — stays in ctx->gp[4])
    mov     eax, DWORD PTR [r13+0]
    mov     ecx, DWORD PTR [r13+4]
    mov     edx, DWORD PTR [r13+8]
    mov     ebx, DWORD PTR [r13+12]
    mov     ebp, DWORD PTR [r13+20]
    mov     esi, DWORD PTR [r13+24]
    mov     edi, DWORD PTR [r13+28]

    ; Restore guest EFLAGS into host RFLAGS.
    ; R14 still holds host_code, so use the stack.
    push    r14                             ; save host_code
    mov     r14d, DWORD PTR [r13+36]        ; R14D = ctx->eflags
    push    r14
    popfq                                   ; load guest EFLAGS
    pop     r14                             ; restore host_code

    ; Dispatch into trace (ends with RET back here)
    call    r14

    ; Save guest EFLAGS from host RFLAGS.
    pushfq
    pop     r14                             ; R14 = guest EFLAGS
    mov     DWORD PTR [r13+36], r14d        ; ctx->eflags = R14D

    ; Save guest GP registers back to context
    mov     DWORD PTR [r13+0],  eax
    mov     DWORD PTR [r13+4],  ecx
    mov     DWORD PTR [r13+8],  edx
    mov     DWORD PTR [r13+12], ebx
    mov     DWORD PTR [r13+20], ebp
    mov     DWORD PTR [r13+24], esi
    mov     DWORD PTR [r13+28], edi

    ; Save guest FPU/SSE state, restore host FPU/SSE state
    lea     rax, [r13 + CTX_GUEST_FPU]
    fxsave  [rax]
    lea     rax, [r13 + CTX_HOST_FPU]
    fxrstor [rax]

    ; Restore callee-saved registers
    pop     rsi
    pop     rdi
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

dispatch_trace ENDP

END
