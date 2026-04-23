; dispatch_trace.asm — MASM x64 trampoline for Windows
;
; void dispatch_trace(GuestContext* ctx, void* host_code)
; Windows x64 calling convention: RCX = ctx, RDX = host_code
;
; Pinned registers inside a trace:
;   R12 = fastmem_base   (callee-saved)
;   R13 = GuestContext*   (callee-saved)
;   R14 = EA scratch      (callee-saved — we push/pop it ourselves)
;   R15 = ram_size        (callee-saved)
;
; Guest GP registers live in host EAX–EDI while a trace runs.
; ESP is NOT mapped to host RSP; it stays in ctx->gp[GP_ESP].

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
    mov     r15d, DWORD PTR [r13+56]       ; R15D = ctx->ram_size
    mov     r14, rsi                        ; Stash host_code in R14

    ; Load guest GP registers into host registers
    ; (ESP intentionally skipped — stays in ctx->gp[4])
    mov     eax, DWORD PTR [r13+0]
    mov     ecx, DWORD PTR [r13+4]
    mov     edx, DWORD PTR [r13+8]
    mov     ebx, DWORD PTR [r13+12]
    mov     ebp, DWORD PTR [r13+20]
    mov     esi, DWORD PTR [r13+24]
    mov     edi, DWORD PTR [r13+28]

    ; Dispatch into trace (ends with RET back here)
    call    r14

    ; Save guest GP registers back to context
    mov     DWORD PTR [r13+0],  eax
    mov     DWORD PTR [r13+4],  ecx
    mov     DWORD PTR [r13+8],  edx
    mov     DWORD PTR [r13+12], ebx
    mov     DWORD PTR [r13+20], ebp
    mov     DWORD PTR [r13+24], esi
    mov     DWORD PTR [r13+28], edi

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
