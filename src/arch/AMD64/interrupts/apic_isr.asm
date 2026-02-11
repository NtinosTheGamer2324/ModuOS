; apic_isr.asm

extern apic_timer_irq_handler_c
extern apic_spurious_irq_handler_c

global apic_timer_stub
global apic_spurious_stub

section .text

%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_REGS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; Ensure System V ABI stack alignment for C calls.
; On interrupt entry, RSP alignment is not guaranteed. We align by subtracting 8
; (same trick as many kernels) before the call.

apic_timer_stub:
    SAVE_REGS
    ; Align stack for SysV ABI call. After an interrupt, RSP alignment is not guaranteed.
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    call apic_timer_irq_handler_c
    mov rsp, rax
    RESTORE_REGS
    iretq

apic_spurious_stub:
    SAVE_REGS
    mov rax, rsp
    and rsp, -16
    sub rsp, 8
    call apic_spurious_irq_handler_c
    mov rsp, rax
    RESTORE_REGS
    iretq
