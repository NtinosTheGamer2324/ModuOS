; fork_trampoline.asm - Child process return from fork()
;
; When context_switch_asm jumps here, the kernel stack looks like:
;   [rsp+0]  padding (0)       ← rsp points here on entry
;   [rsp+8]  user RIP
;   [rsp+16] user CS
;   [rsp+24] user RFLAGS (IF=1)
;   [rsp+32] user RSP
;   [rsp+40] user SS
;
; We skip the padding slot, set rax=0 (child fork return value), then iretq.

bits 64
section .text

global fork_child_trampoline

fork_child_trampoline:
    ; Skip the padding slot pushed during fork setup
    add rsp, 8

    ; Child returns 0 from fork()
    xor rax, rax

    ; Zero all registers to avoid leaking kernel data to userland
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor rbp, rbp

    ; Restore user GS base. The scheduler (timer IRQ) executed swapgs on entry
    ; from userland, switching to kernel GS. We must swap back before iretq
    ; so that userland sees the correct GS base.
    ; If this child was first scheduled from a non-IRQ context (e.g. direct
    ; schedule() call from kernel_main), the GS is already at kernel base and
    ; swapgs is still correct because iretq to ring3 requires user GS.
    swapgs

    ; Return to userland — pops RIP, CS, RFLAGS, RSP, SS from kernel stack
    iretq
