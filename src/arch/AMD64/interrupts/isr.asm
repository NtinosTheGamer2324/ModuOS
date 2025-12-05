; isr.asm

global irq_stubs
extern irq_dispatch

%macro IRQ_STUB 1
global irq_stub%1
irq_stub%1:
    ; Save ALL registers to preserve state
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
    
    ; IRQ number -> first argument (rdi)
    mov rdi, %1
    call irq_dispatch
    
    ; Restore ALL registers
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
    
    iretq
%endmacro

section .text

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

section .data
irq_stubs:
    dq irq_stub0
    dq irq_stub1
    dq irq_stub2
    dq irq_stub3
    dq irq_stub4
    dq irq_stub5
    dq irq_stub6
    dq irq_stub7
    dq irq_stub8
    dq irq_stub9
    dq irq_stub10
    dq irq_stub11
    dq irq_stub12
    dq irq_stub13
    dq irq_stub14
    dq irq_stub15