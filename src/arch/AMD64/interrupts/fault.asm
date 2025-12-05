; fault.asm - CPU Exception Handler Stubs
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

; Macro for faults WITHOUT error code
%macro FAULT_STUB_NO_ERROR 2
global fault_stub_%1
fault_stub_%1:
    ; CPU has already pushed: SS, RSP, RFLAGS, CS, RIP
    ; Pass pointer to interrupt frame as argument (RDI)
    mov rdi, rsp
    call %2
    iretq
%endmacro

; Macro for faults WITH error code
%macro FAULT_STUB_WITH_ERROR 2
global fault_stub_%1
fault_stub_%1:
    ; CPU has pushed: error_code, then SS, RSP, RFLAGS, CS, RIP
    ; Pop error code into RDI (first argument)
    pop rdi
    ; Pass pointer to interrupt frame as second argument (RSI)
    mov rsi, rsp
    call %2
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