; enter_user_trampoline.asm - enter ring3 from scheduler context
bits 64

global amd64_enter_user_trampoline
global amd64_enter_user_now

; Expected register state when entered (from context_switch):
;   r14 = user RIP (entry point)
;   r15 = user RSP (top of user stack)
;   r12 = argc (saved in cpu_state)
;   r13 = argv (saved in cpu_state)
;   rbx = envp (saved in cpu_state)
;
; This routine loads SysV registers for user entry:
;   rdi=argc, rsi=argv, rdx=envp

%define USER_CS_SEL 0x2B
%define USER_DS_SEL 0x23

section .text

; Noreturn helper for execve: immediately enter userland at a fresh RIP/RSP.
; SysV C ABI:
;   rdi = user_rip
;   rsi = user_rsp
;   rdx = argc
;   rcx = argv
;   r8  = envp
amd64_enter_user_now:
    mov r14, rdi
    mov r15, rsi
    mov r12, rdx
    mov r13, rcx
    mov rbx, r8
    jmp amd64_enter_user_trampoline

amd64_enter_user_trampoline:
    ; Pass argc/argv into userland following SysV: rdi=argc, rsi=argv
    ; We stored them in callee-saved regs r12/r13 in the process cpu_state.
    mov rdi, r12
    mov rsi, r13
    mov rdx, rbx

    ; Set user rbp to the user stack top to avoid stale kernel frame pointers.
    mov rbp, r15

    ; Build iret frame: SS, RSP, RFLAGS, CS, RIP
    push qword USER_DS_SEL
    push r15
    push qword 0x202
    push qword USER_CS_SEL
    push r14
    iretq
