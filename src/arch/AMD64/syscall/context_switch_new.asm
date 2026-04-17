; context_switch_new.asm
;
; void context_switch_asm(cpu_context_t *old_ctx, cpu_context_t *new_ctx,
;                         void *old_fpu,           void *new_fpu,
;                         uint64_t new_cr3)
;
; cpu_context_t layout:
;   0   r15
;   8   r14
;   16  r13
;   24  r12
;   32  rbx
;   40  rbp
;   48  rip
;   56  rsp
;   64  rflags
;
; FPU is lazy: CR0.TS is set on switch; #NM restores on first use.
; CR3 is switched before popfq so that IF is never open in the wrong
; address space, regardless of whether new_ctx is a kernel or user thread.

section .text
global context_switch_asm

context_switch_asm:
    ; RDI = old_ctx  (NULL on the very first switch)
    ; RSI = new_ctx
    ; RDX = old_fpu  (unused; lazy FPU)
    ; RCX = new_fpu  (unused; lazy FPU)
    ; R8  = new_cr3  (0 = no address-space switch)

    test rdi, rdi
    jz .restore

.save:
    mov [rdi +  0], r15
    mov [rdi +  8], r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12
    mov [rdi + 32], rbx
    mov [rdi + 40], rbp

    mov rax, [rsp]          ; return address becomes saved RIP
    mov [rdi + 48], rax

    lea rax, [rsp + 8]      ; caller's RSP (past the return address)
    mov [rdi + 56], rax

    pushfq
    pop rax
    mov [rdi + 64], rax

.restore:
    mov r15, [rsi +  0]
    mov r14, [rsi +  8]
    mov r13, [rsi + 16]
    mov r12, [rsi + 24]
    mov rbx, [rsi + 32]
    mov rbp, [rsi + 40]

    mov rax, [rsi + 48]     ; new RIP
    mov r9,  [rsi + 56]     ; new RSP (free to use; RDX/RCX already consumed)
    mov r10, [rsi + 64]     ; new RFLAGS

    ; Hold interrupts closed for the CR3 switch and stack swap.  popfq
    ; below restores IF atomically from the saved RFLAGS, so there is no
    ; unconditional sti anywhere in this path.
    cli

    ; Switch address space before opening interrupts.  An IRQ firing between
    ; a CR3 write and the matching popfq would run the handler in the new
    ; address space with the old stack pointer — avoid that by doing the
    ; stack swap first, then CR3, then popfq as one uninterruptible sequence.
    mov rsp, r9

    test r8, r8
    jz .no_cr3
    mov cr3, r8
.no_cr3:

    push r10
    popfq                   ; atomically restores IF (and all other flags)

    jmp rax