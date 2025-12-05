; syscall_asm.s  (fixed)
bits 64

extern syscall_handler

global syscall_entry

section .text
syscall_entry:
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
    
    ; Align stack
    test rsp, 0xF
    jz .aligned
    sub rsp, 8
.aligned:

    ; Setup args...
    mov r8, rsi
    mov r9, rdi
    mov rdi, rax
    mov rsi, rbx
    mov r10, rdx
    mov rdx, rcx
    mov rcx, r10

    cld
    call syscall_handler

    ; Restore stack
    mov rsp, rbp

    ; Store return value (rax is at top of stack now!)
    mov [rsp], rax

    ; Restore all
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

    iretq