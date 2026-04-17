; enter_user_trampoline.asm - enter ring3 from scheduler context
bits 64

global amd64_enter_user_trampoline
global amd64_enter_user_now

extern process_get_current_cr3
extern com_write_string

%define USER_CS_SEL 0x2B
%define USER_DS_SEL 0x23

section .rodata
trampoline_msg: db "[TRAMPOLINE] Entering user mode", 10, 0

section .text

amd64_enter_user_now:
    mov r14, rdi
    mov r15, rsi
    mov r12, rdx
    mov r13, rcx
    mov rbx, r8
    jmp amd64_enter_user_trampoline

amd64_enter_user_trampoline:
    cli

    push r12
    push r13
    push r14
    push r15
    push rbx

    mov rdi, 0x3F8
    lea rsi, [rel trampoline_msg]
    call com_write_string

    call process_get_current_cr3

    pop rbx
    pop r15
    pop r14
    pop r13
    pop r12

    test rax, rax
    jz .no_cr3_switch
    mov cr3, rax

.no_cr3_switch:
    mov rdi, r12
    mov rsi, r13
    mov rdx, rbx
    xor rbp, rbp

    push qword USER_DS_SEL
    push r15
    push qword 0x202
    push qword USER_CS_SEL
    push r14

    iretq