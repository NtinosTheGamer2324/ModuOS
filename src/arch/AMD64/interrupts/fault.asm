; fault.asm - CPU Exception Handler Stubs
;
; IMPORTANT:
;  - Exception/IRQ stubs must preserve the interrupted context, not the SysV ABI.
;  - Caller-saved registers (like RDX) WILL be clobbered by the C handler call.
;  - If we restore an incorrect RSP and execute IRETQ, we can trigger a nested #GP
;    with RIP pointing inside the stub (exactly like RIP=0x141a92).
;
section .text

; External C handlers
extern fault_handler_divide_error
extern fault_handler_debug
extern fault_handler_nmi
extern fault_handler_breakpoint
extern fault_handler_overflow
extern fault_handler_bound_range
extern fault_handler_invalid_opcode
extern fault_handler_device_not_available
extern fault_handler_double_fault
extern fault_handler_invalid_tss
extern fault_handler_segment_not_present
extern fault_handler_stack_fault
extern fault_handler_general_protection
extern fault_handler_page_fault
extern fault_handler_x87_fpu
extern fault_handler_alignment_check
extern fault_handler_machine_check
extern fault_handler_simd_exception

; Save/restore all general purpose registers.
; (We intentionally do not touch segment registers here.)
%macro PUSH_GPRS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; Macro for faults WITHOUT error code
%macro FAULT_STUB_NO_ERROR 2
global fault_stub_%1
fault_stub_%1:
    ; Stack on entry (CPL0->CPL0):
    ;   [rsp+0]  RIP
    ;   [rsp+8]  CS
    ;   [rsp+16] RFLAGS

    PUSH_GPRS

    ; After PUSH_GPRS, the CPU frame is above our saved registers.
    ; 15 pushes => 120 bytes.
    mov rax, [rsp + 120 + 0]   ; rip
    mov rbx, [rsp + 120 + 8]   ; cs
    mov rcx, [rsp + 120 + 16]  ; rflags

    ; Build a stable snapshot (32 bytes) for the C handler.
    sub rsp, 32
    mov [rsp + 0], rax
    mov [rsp + 8], rbx
    mov [rsp + 16], rcx
    mov qword [rsp + 24], 0

    mov rdi, rsp
    call %2

    add rsp, 32
    POP_GPRS
    iretq
%endmacro

; Macro for faults WITH error code
%macro FAULT_STUB_WITH_ERROR 2
global fault_stub_%1
fault_stub_%1:
    ; Stack on entry (CPL0->CPL0):
    ;   [rsp+0]  ERROR_CODE
    ;   [rsp+8]  RIP
    ;   [rsp+16] CS
    ;   [rsp+24] RFLAGS

    PUSH_GPRS

    ; After PUSH_GPRS, error code + CPU frame are above saved registers.
    ; 15 pushes => 120 bytes.
    mov rdi, [rsp + 120 + 0]    ; error code (arg0)

    mov rax, [rsp + 120 + 8]    ; rip
    mov rbx, [rsp + 120 + 16]   ; cs
    mov rcx, [rsp + 120 + 24]   ; rflags

    sub rsp, 32
    mov [rsp + 0], rax
    mov [rsp + 8], rbx
    mov [rsp + 16], rcx
    mov qword [rsp + 24], 0

    mov rsi, rsp                ; frame* (arg1)
    call %2

    add rsp, 32
    POP_GPRS

    ; Drop the CPU-pushed error code before returning.
    add rsp, 8
    iretq
%endmacro

; Exception handlers
FAULT_STUB_NO_ERROR    0,  fault_handler_divide_error
FAULT_STUB_NO_ERROR    1,  fault_handler_debug
FAULT_STUB_NO_ERROR    2,  fault_handler_nmi
FAULT_STUB_NO_ERROR    3,  fault_handler_breakpoint
FAULT_STUB_NO_ERROR    4,  fault_handler_overflow
FAULT_STUB_NO_ERROR    5,  fault_handler_bound_range
FAULT_STUB_NO_ERROR    6,  fault_handler_invalid_opcode
FAULT_STUB_NO_ERROR    7,  fault_handler_device_not_available
FAULT_STUB_WITH_ERROR  8,  fault_handler_double_fault
FAULT_STUB_WITH_ERROR  10, fault_handler_invalid_tss
FAULT_STUB_WITH_ERROR  11, fault_handler_segment_not_present
FAULT_STUB_WITH_ERROR  12, fault_handler_stack_fault
FAULT_STUB_WITH_ERROR  13, fault_handler_general_protection
FAULT_STUB_WITH_ERROR  14, fault_handler_page_fault
FAULT_STUB_NO_ERROR    16, fault_handler_x87_fpu
FAULT_STUB_WITH_ERROR  17, fault_handler_alignment_check
FAULT_STUB_NO_ERROR    18, fault_handler_machine_check
FAULT_STUB_NO_ERROR    19, fault_handler_simd_exception