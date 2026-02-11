; syscall64_entry.asm - AMD64 SYSCALL/SYSRET entry
bits 64

extern syscall_handler
global syscall64_entry

section .text

syscall64_entry:
    ; 1. Switch to Kernel GS to access per-cpu data
    swapgs 

    ; 2. Save the USER RSP into a temporary slot in your per-cpu struct
    ; Assuming your cpu_local_t has user_rsp at offset 16
    mov [gs:16], rsp 

    ; 3. Load the KERNEL STACK
    ; Assuming syscall_rsp0 is at offset 24
    mov rsp, [gs:24]

    ; 4. Manually construct what your C handler expects
    ; SYSCALL put User RIP in RCX and User RFLAGS in R11.
    push qword [gs:16]  ; Push User RSP
    push r11            ; Push User RFLAGS
    push rcx            ; Push User RIP

    ; 5. Push GPRs to match your syscall_handler call
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 6. Map registers to C Calling Convention
    ; User: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5
    ; C:    rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5
    mov r9, r8      ; a5
    mov r8, r10     ; a4
    mov rcx, rdx    ; a3
    mov rdx, rsi    ; a2
    mov rsi, rdi    ; a1
    mov rdi, rax    ; syscall_num

    cld
    call syscall_handler

    ; 7. Restore GPRs
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 8. Prepare for SYSRET
    pop rcx         ; Restore User RIP into RCX
    pop r11         ; Restore User RFLAGS into R11
    
    ; 9. Restore User RSP
    mov rsp, [gs:16]

    ; 10. Flip GS back to User mode
    swapgs

    ; 11. RETURN (Must use sysretq, NOT iretq)
    sysretq
