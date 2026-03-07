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
    ; SysV ABI: RSP must be 16-byte aligned at the point of the call instruction.
    ; 'call' will then push the 8-byte return address, leaving RSP misaligned by 8
    ; inside the callee — which is exactly what the ABI requires.
    ; Save pre-alignment RSP in a callee-saved register so it survives the call.
    mov rbx, rsp
    and rsp, -16
    call apic_timer_irq_handler_c
    mov rsp, rbx
    RESTORE_REGS
    iretq

apic_spurious_stub:
    SAVE_REGS
    mov rbx, rsp
    and rsp, -16
    call apic_spurious_irq_handler_c
    mov rsp, rbx
    RESTORE_REGS
    iretq
