; isr.asm

global irq_stubs
extern irq_dispatch
extern schedule
extern should_reschedule

%macro IRQ_STUB 1
global irq_stub%1
irq_stub%1:
    cld

    test word [rsp + 8], 3
    jz .from_kernel_%1
    swapgs

.from_kernel_%1:
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

    mov rdi, %1
    call irq_dispatch

    ; Only preempt when returning to userspace (CPL 3).
    ; The iretq frame is now at [rsp + 15*8]: RIP, CS, RFLAGS, RSP, SS.
    ; CS is at offset +8 from RIP, i.e. [rsp + 15*8 + 8].
    test word [rsp + 15*8 + 8], 3
    jz .no_preempt_%1

    ; Returning to ring-3: check need_resched and invoke schedule() if set.
    ; Interrupts are still disabled here (we're in IRQ context), which is
    ; exactly the condition schedule() needs to do a safe context switch.
    call should_reschedule
    test eax, eax
    jz .no_preempt_%1
    call schedule

.no_preempt_%1:
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

    test word [rsp + 8], 3
    jz .return_to_kernel_%1
    swapgs

.return_to_kernel_%1:
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