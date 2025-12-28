; context_switch.asm - Context switching with argument passing
; context_switch.asm - Context switching with argument passing + FPU save/restore
; void context_switch(cpu_state_t *old_state, cpu_state_t *new_state,
;                    void *old_fpu_state, void *new_fpu_state)

section .text
global context_switch

context_switch:
    ; RDI = old_state (or NULL for first switch)
    ; RSI = new_state
    ; RDX = old_fpu_state (or NULL)
    ; RCX = new_fpu_state
    
    ; Save current context if old_state is not NULL
    test rdi, rdi
    jz .restore

    ; Lazy FPU switching: do NOT save here (too expensive).
.save_gprs:
    
    ; Save callee-saved registers to old_state
    mov [rdi + 0], r15
    mov [rdi + 8], r14
    mov [rdi + 16], r13
    mov [rdi + 24], r12
    mov [rdi + 32], rbx
    mov [rdi + 40], rbp
    
    ; Save return address as RIP
    mov rax, [rsp]
    mov [rdi + 48], rax
    
    ; Save RSP (after return address)
    lea rax, [rsp + 8]
    mov [rdi + 56], rax
    
    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + 64], rax
    
.restore:
    ; Lazy FPU switching: FPU is restored on first use via #NM handler.

    ; Restore context from new_state (RSI)
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
    
    ; Restore RFLAGS
    mov rdx, [rsi + 64]
    ; Always ensure IF=1 while running scheduled processes.
    ; This prevents "keyboard dead" scenarios if a saved context had IF=0.
    or  rdx, 0x200
    push rdx
    popfq
    
    ; Before switching stacks, check if we have arguments
    ; Arguments are in r12 (argc) and r13 (argv)
    ; If r12 != 0, move them to RDI and RSI
    test r12, r12
    jz .no_args
    
    ; We have arguments - move them to proper registers
    mov rdi, r12  ; argc -> RDI
    mov rsi, r13  ; argv -> RSI
    xor r12, r12  ; Clear r12 so we don't pass args again
    xor r13, r13  ; Clear r13
    jmp .switch_stack
    
.no_args:
    ; No arguments - clear RDI and RSI
    xor rdi, rdi
    xor rsi, rsi
    
.switch_stack:
    ; Switch to new stack
    mov rsp, rcx
    
    ; Jump to new RIP
    jmp rax