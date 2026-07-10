; syscall64_entry.asm - SYSCALL/SYSRET entry point
;
; Frame layout mirrors syscall_entry.asm exactly so that sys_fork_impl,
; syscall64_entry_return, and signal delivery work without modification.
;
; On SYSCALL entry (hardware contract):
;   RCX = user RIP
;   R11 = user RFLAGS
;   RSP = user RSP  (NOT switched; we must do it ourselves)
;   CS  = STAR.kernel_cs, SS = STAR.kernel_cs + 8
;   IF  = 0 (cleared by FMASK)
;   RAX = syscall number
;   RDI, RSI, RDX = args 1–3
;   R10 = arg 4   (RCX is clobbered by hardware)
;   R8  = arg 5

bits 64

extern syscall_handler
extern g_syscall_entry_rbp
extern g_kernel_cr3
extern g_syscall_rsp0               ; written by amd64_syscall_set_kernel_stack()

global syscall64_entry
global syscall64_entry_return

section .text

syscall64_entry:
    ; IF is already 0 (FMASK).  We are non-preemptible until we re-enable
    ; interrupts at the bottom of syscall_handler (or never, for short paths).

    swapgs

    ; Save user RSP, then load the kernel stack from g_syscall_rsp0.
    ; This is the same value amd64_tss_set_rsp0() writes to TSS.rsp0, kept in
    ; a plain exported global so we can reach it with a RIP-relative load
    ; without needing g_tss exported from gdt.c.
    mov [gs:48], rsp                        ; stash user RSP in scratch slot
    mov rsp, [rel g_syscall_rsp0]           ; kernel RSP0

    ; Build an iretq-compatible frame so syscall64_entry_return can use iretq
    ; unconditionally, keeping the same contract as syscall_entry.asm.
    push qword 0x23                         ; user SS  (USER_DS  | 3)
    push qword [gs:48]                      ; user RSP
    push r11                                ; user RFLAGS
    push qword 0x2B                         ; user CS  (USER_CS  | 3)
    push rcx                                ; user RIP

    ; Save all GPRs in the same order as syscall_entry.asm so that
    ; sys_fork_impl can walk the frame without caring which entry path was used.
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
    push rax                                ; syscall number at [rsp+0]

    mov rbp, rsp
    mov [rel g_syscall_entry_rbp], rbp

    ; SysV ABI 16-byte stack alignment before the C call.
    test rsp, 0xF
    jz .aligned
    sub rsp, 8
.aligned:

    ; Map saved registers to C arguments.
    ; Frame at rbp: [0]=rax(num) [8]=rbx [16]=rcx [24]=rdx
    ;               [32]=rsi     [40]=rdi [48]=rbp [56]=r8
    ;               [64]=r9      [72]=r10 [80]=r11 [88]=r12
    ;               [96]=r13     [104]=r14 [112]=r15
    ; iretq frame:  RIP CS RFLAGS RSP SS
    mov rdi, [rbp]          ; syscall_num  ← saved rax
    mov rsi, [rbp + 40]     ; arg1         ← saved rdi
    mov rdx, [rbp + 32]     ; arg2         ← saved rsi
    mov rcx, [rbp + 24]     ; arg3         ← saved rdx
    mov r8,  [rbp + 72]     ; arg4         ← saved r10
    mov r9,  [rbp + 56]     ; arg5         ← saved r8

    cld
    call syscall_handler

    mov rsp, rbp
    mov [rsp], rax          ; return value into saved-rax slot

    ; Check need_resched before returning to userspace.  If the timer IRQ
    ; fired during this syscall and marked the current process for preemption,
    ; we honour it here so the process doesn't get an extra unearned time slice
    ; just because it happened to be in a syscall.
    ; Interrupts are still off (FMASK cleared IF on entry); schedule() is safe
    ; to call here on the kernel stack — it will re-enable interrupts via
    ; popfq inside context_switch_asm when it switches to the next process.
extern should_reschedule
extern schedule
    call should_reschedule
    test eax, eax
    jz syscall64_entry_return
    call schedule
    ; After schedule() returns we are the current process again; fall through
    ; to restore registers and iretq back to userspace.

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

    ; iretq pops: RIP, CS, RFLAGS (restoring IF), RSP, SS
    swapgs
    iretq