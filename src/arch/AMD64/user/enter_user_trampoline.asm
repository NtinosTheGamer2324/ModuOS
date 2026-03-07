; enter_user_trampoline.asm - enter ring3 from scheduler context
bits 64

global amd64_enter_user_trampoline
global amd64_enter_user_now

extern process_get_current_cr3
extern com_write_string

; Expected register state when entered (from context_switch):
;   r14 = user RIP (entry point)
;   r15 = user RSP (top of user stack)
;   r12 = argc
;   r13 = argv
;   rbx = envp

%define USER_CS_SEL 0x2B
%define USER_DS_SEL 0x23

section .text

amd64_enter_user_now:
    mov r14, rdi
    mov r15, rsi
    mov r12, rdx
    mov r13, rcx
    mov rbx, r8
    jmp amd64_enter_user_trampoline

amd64_enter_user_trampoline:
    ; Interrupts must be off for the entire trampoline — iretq restores IF
    ; atomically from RFLAGS=0x202.  Enforce this regardless of caller state.
    cli

    ; Announce entry for diagnostics.
    mov rdi, 0x3F8
    lea rsi, [rel .msg]
    call com_write_string

    ; Retrieve the process PML4 physical address via C — NASM extern data
    ; symbol PC32 relocations are unreliable across kernel object files.
    push r12
    push r13
    push r14
    push r15
    push rbx
    call process_get_current_cr3    ; rax = CR3 (0 = no switch needed)
    pop rbx
    pop r15
    pop r14
    pop r13
    pop r12

    test rax, rax
    jz .no_cr3_switch
    mov cr3, rax

.no_cr3_switch:
    ; SysV userland entry: rdi=argc, rsi=argv, rdx=envp
    mov rdi, r12
    mov rsi, r13
    mov rdx, rbx

    xor rbp, rbp        ; ABI: rbp=0 marks outermost frame at process entry

    mov ax, USER_DS_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax

    ; iretq frame: SS, RSP, RFLAGS, CS, RIP
    push qword USER_DS_SEL
    push r15
    push qword 0x202
    push qword USER_CS_SEL
    push r14

    iretq           ; atomically restores RFLAGS (IF=1 from 0x202) on ring-3 entry

section .rodata
.msg: db "[TRAMPOLINE] Entering user mode", 10, 0
