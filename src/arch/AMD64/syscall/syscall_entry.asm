; syscall_entry.asm
bits 64

extern syscall_handler
extern g_syscall_entry_rbp
extern g_syscall_entry_rsp
extern current

global syscall_entry
global syscall_entry_return

section .text
syscall_entry:
    ; Syscalls currently execute in CPL0 in this kernel.
    ; Avoid being preempted by timer IRQ in the middle of the syscall prologue/epilogue,
    ; otherwise the interrupt handler may schedule/context-switch and corrupt our iret frame.
    ; iretq will restore IF from the saved RFLAGS automatically.
    cli

    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rbp, rsp
    mov [rel g_syscall_entry_rbp], rbp

    ; Align stack (match SysV ABI for C call)
    test rsp, 0xF
    jz .aligned
    sub rsp, 8
.aligned:

    ; Setup args for C calling convention
    ; Userspace passes: rax=syscall_num, rdi=arg1, rsi=arg2, rdx=arg3, r10=arg4, r8=arg5
    ; C function expects: rdi=syscall_num, rsi=arg1, rdx=arg2, rcx=arg3, r8=arg4, r9=arg5
    mov r9, r8      ; arg5 = r8
    mov r8, r10     ; arg4 = r10
    mov rcx, rdx    ; arg3 = rdx
    mov rdx, rsi    ; arg2 = rsi
    mov rsi, rdi    ; arg1 = rdi
    mov rdi, rax    ; syscall_num = rax

    cld
    call syscall_handler

    ; Restore stack
    mov rsp, rbp

    ; Store return value at [rbp+0] (the saved rax slot).
    mov [rsp], rax

    ; Signal delivery happens at the start of the next syscall (see syscall_handler).

    ; Restore all
syscall_entry_return:
    pop rax    ; Gets the return value we just stored
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Interrupts will be restored from RFLAGS in iret frame
    ; Do NOT sti here - it only affects ring 0, and can cause issues
    
    iretq