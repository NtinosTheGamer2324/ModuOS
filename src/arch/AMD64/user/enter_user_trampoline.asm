; enter_user_trampoline.asm - enter ring3 from scheduler context
bits 64

global amd64_enter_user_trampoline

; Expected register state when entered (from context_switch):
;   r14 = user RIP (entry point)
;   r15 = user RSP (top of user stack)
;   rdi = argc
;   rsi = argv
;
; This routine does NOT clobber rdi/rsi so user _start/md_main see argc/argv.

%define USER_CS_SEL 0x2B
%define USER_DS_SEL 0x23

section .text
amd64_enter_user_trampoline:
    ; Pass argc/argv into userland following SysV: rdi=argc, rsi=argv
    ; We stored them in callee-saved regs r12/r13 in the process cpu_state.
    mov rdi, r12
    mov rsi, r13

    ; Build iret frame: SS, RSP, RFLAGS, CS, RIP
    push qword USER_DS_SEL
    push r15
    push qword 0x202
    push qword USER_CS_SEL
    push r14
    iretq
