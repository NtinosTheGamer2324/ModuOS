; syscall64_entry.asm - AMD64 SYSCALL/SYSRET entry
bits 64

extern syscall_handler
extern g_syscall_rsp0

global syscall64_entry

section .text

; SYSCALL clobbers:
;   RCX = user RIP
;   R11 = user RFLAGS
;   CS/SS switched to kernel via STAR
;   RSP is NOT changed (still user RSP!)
;
; We must switch to a kernel stack before calling C.
;
; ABI we keep (from userland):
;   RAX = syscall_num
;   RDI, RSI, RDX, R10, R8 = args 1..5
;
; We call C as: syscall_handler(num, a1,a2,a3,a4,a5)
;   RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5

syscall64_entry:
    ; Save user RSP (SYSCALL does not save it; SYSRET uses current RSP as user RSP)
    mov r12, rsp

    ; Switch to kernel stack immediately (do not touch user stack)
    mov rsp, [rel g_syscall_rsp0]

    ; Save user RIP/RFLAGS (SYSCALL places them in RCX/R11)
    push rcx
    push r11

    ; Save GPRs we clobber / want preserved
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Build args for syscall_handler (keep existing userland ABI)
    ; user: rax=num, rdi,rsi,rdx,r10,r8
    ; C:    rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    cld
    call syscall_handler

    ; Return value now in RAX

    ; Restore preserved regs
    pop r15
    pop r14
    pop r13
    pop r12        ; restore saved user RSP into r12
    pop rbp
    pop rbx

    ; Restore user RFLAGS/RIP for SYSRET
    pop r11
    pop rcx

    ; Restore user stack pointer before returning to CPL=3
    mov rsp, r12

    sysretq
