; context_switch.asm
;
; void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
;                     void *old_fpu,           void *new_fpu)
;
; cpu_state_t layout mirrors cpu_context_t (same offsets).
;
; FPU is lazy: CR0.TS is set on switch; #NM restores on first use.
; r12/r13 are preserved across the switch unmodified; the user-mode
; trampoline (amd64_enter_user_trampoline) reads argc/argv from them.
;
; RFLAGS is restored from the saved context via popfq, not forced via sti,
; so a kernel thread that context-switched out with IF=0 will resume with
; IF=0.  The trampoline opens with an explicit cli and re-enables interrupts
; through iretq, so that path is safe either way.

section .text
global context_switch

context_switch:
    ; RDI = old_state  (NULL on the very first switch)
    ; RSI = new_state
    ; RDX = old_fpu    (unused; lazy FPU)
    ; RCX = new_fpu    (unused; lazy FPU)

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
    mov r9,  [rsi + 56]     ; new RSP
    mov r10, [rsi + 64]     ; new RFLAGS

    ; Keep interrupts closed across the stack swap and into popfq.
    ; popfq is the only place IF changes; there is no sti in this path.
    cli

    mov rsp, r9

    push r10
    popfq

    jmp rax