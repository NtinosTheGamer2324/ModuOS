; context_switch_new.asm - Context switching for new process system
; void context_switch_asm(cpu_context_t *old_ctx, cpu_context_t *new_ctx,
;                         void *old_fpu, void *new_fpu)

section .text
global context_switch_asm

; cpu_context_t structure offsets:
; offset 0:  r15
; offset 8:  r14
; offset 16: r13
; offset 24: r12
; offset 32: rbx
; offset 40: rbp
; offset 48: rip
; offset 56: rsp
; offset 64: rflags

context_switch_asm:
    ; RDI = old_ctx (or NULL for first switch)
    ; RSI = new_ctx
    ; RDX = old_fpu (or NULL)
    ; RCX = new_fpu
    
    ; Save current context if old_ctx is not NULL
    test rdi, rdi
    jz .restore
    
.save:
    ; Save callee-saved registers
    mov [rdi + 0], r15
    mov [rdi + 8], r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12
    mov [rdi + 32], rbx
    mov [rdi + 40], rbp
    
    ; Save return address as RIP
    mov rax, [rsp]
    mov [rdi + 48], rax
    
    ; Save RSP (pointing after return address)
    lea rax, [rsp + 8]
    mov [rdi + 56], rax
    
    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 64], rax
    
.restore:
    ; Lazy FPU: handled by CR0.TS bit, not saved/restored here
    
    ; Restore context from new_ctx (RSI)
    mov r15, [rsi + 0]
    mov r14, [rsi + 8]
    mov r13, [rsi + 16]
    mov r12, [rsi + 24]
    mov rbx, [rsi + 32]
    mov rbp, [rsi + 40]
    
    ; Get new RIP
    mov rax, [rsi + 48]
    
    ; Get new RSP
    mov rcx, [rsi + 56]
    
    ; Restore RFLAGS (ensure IF=1 for interrupts)
    mov rdx, [rsi + 64]
    or  rdx, 0x200      ; Force IF=1
    push rdx
    popfq
    
    ; Switch to new stack
    mov rsp, rcx
    
    ; Jump to new RIP
    jmp rax
