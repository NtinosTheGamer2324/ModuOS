; syscall64_entry.asm - SYSCALL/SYSRET entry point
;
; Frame layout mirrors syscall_entry.asm exactly so that sys_fork_impl,
; syscall_entry_return, and signal delivery work without modification.
;
; On SYSCALL entry:
;   RCX = user RIP (hardware-saved)
;   R11 = user RFLAGS (hardware-saved)
;   RSP = user RSP
;   RAX = syscall number
;   RDI, RSI, RDX = args 1-3
;   R10 = arg 4
;   R8  = arg 5

bits 64

extern syscall_handler
extern g_syscall_entry_rbp
extern g_kernel_cr3

global syscall64_entry
global syscall64_entry_return

section .text

syscall64_entry:
    ; FMASK cleared IF on entry; we are non-interruptible.
    swapgs

    ; Preserve user RSP and switch to the per-CPU kernel stack.
    mov [gs:48], rsp
    mov rsp, [gs:24]

    ; Build a synthetic iretq-compatible frame so syscall64_entry_return
    ; can use iretq unconditionally, matching the syscall_entry.asm contract.
    push qword 0x23     ; user SS  (USER_DS)
    push qword [gs:48]  ; user RSP
    push r11            ; user RFLAGS
    push qword 0x2B     ; user CS  (USER_CS)
    push rcx            ; user RIP

    ; GPR save — identical order to syscall_entry.asm.
    ; RAX still holds the syscall number at this point.
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

    ; Record frame base for sys_fork_impl.
    mov rbp, rsp
    mov [rel g_syscall_entry_rbp], rbp

    ; SysV ABI alignment.
    test rsp, 0xF
    jz .aligned
    sub rsp, 8
.aligned:
    ; Reconstruct C arguments from the saved frame.
    ; Saved layout (rbp offsets): [0]=rax [8]=rbx [16]=rcx [24]=rdx
    ;                             [32]=rsi [40]=rdi [48]=rbp [56]=r8
    ;                             [64]=r9  [72]=r10 [80]=r11 [88]=r12
    ;                             [96]=r13 [104]=r14 [112]=r15
    ; iretq frame above that: RIP CS RFLAGS RSP SS
    mov rdi, [rbp]      ; syscall_num  ← saved rax
    mov rsi, [rbp + 40] ; arg1         ← saved rdi
    mov rdx, [rbp + 32] ; arg2         ← saved rsi
    mov rcx, [rbp + 24] ; arg3         ← saved rdx
    mov r8,  [rbp + 72] ; arg4         ← saved r10
    mov r9,  [rbp + 56] ; arg5         ← saved r8

    cld
    call syscall_handler

    mov rsp, rbp
    mov [rsp], rax      ; write return value into saved rax slot

syscall64_entry_return:
    pop rax
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

    ; iretq frame: RIP CS RFLAGS RSP SS
    swapgs
    iretq