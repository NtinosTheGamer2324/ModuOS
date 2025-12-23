#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

// Exception numbers
#define FAULT_DIVIDE_ERROR          0
#define FAULT_DEBUG                 1
#define FAULT_NMI                   2
#define FAULT_BREAKPOINT            3
#define FAULT_OVERFLOW              4
#define FAULT_BOUND_RANGE           5
#define FAULT_INVALID_OPCODE        6
#define FAULT_DEVICE_NOT_AVAILABLE  7
#define FAULT_DOUBLE_FAULT          8
#define FAULT_COPROCESSOR_SEGMENT   9
#define FAULT_INVALID_TSS           10
#define FAULT_SEGMENT_NOT_PRESENT   11
#define FAULT_STACK_FAULT           12
#define FAULT_GENERAL_PROTECTION    13
#define FAULT_PAGE_FAULT            14
#define FAULT_X87_FPU_ERROR         16
#define FAULT_ALIGNMENT_CHECK       17
#define FAULT_MACHINE_CHECK         18
#define FAULT_SIMD_EXCEPTION        19
#define FAULT_VIRTUALIZATION        20
#define FAULT_SECURITY_EXCEPTION    30

// Interrupt stack frame (pushed by CPU during interrupt)
/* Note: When an exception occurs in ring0 (CPL0->CPL0), the CPU pushes only:
 *   RIP, CS, RFLAGS
 * The extended SS/RSP are pushed only on privilege change.
 */
typedef struct {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} interrupt_frame_t;

// Full fault context (includes error code if applicable)
typedef struct {
    uint64_t error_code;
    interrupt_frame_t frame;
} __attribute__((packed)) fault_context_t;

// Page fault error code flags
#define PF_PRESENT      (1 << 0)  // Page not present
#define PF_WRITE        (1 << 1)  // Write access
#define PF_USER         (1 << 2)  // User mode
#define PF_RESERVED     (1 << 3)  // Reserved bit violation
#define PF_INSTRUCTION  (1 << 4)  // Instruction fetch

// Initialize fault handlers
void fault_init(void);

// Individual fault handler functions (called from assembly stubs)
void fault_handler_divide_error(interrupt_frame_t *frame);
void fault_handler_debug(interrupt_frame_t *frame);
void fault_handler_nmi(interrupt_frame_t *frame);
void fault_handler_breakpoint(interrupt_frame_t *frame);
void fault_handler_overflow(interrupt_frame_t *frame);
void fault_handler_bound_range(interrupt_frame_t *frame);
void fault_handler_invalid_opcode(interrupt_frame_t *frame);
void fault_handler_device_not_available(interrupt_frame_t *frame);
void fault_handler_double_fault(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_invalid_tss(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_segment_not_present(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_stack_fault(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_general_protection(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_page_fault(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_x87_fpu(interrupt_frame_t *frame);
void fault_handler_alignment_check(uint64_t error_code, interrupt_frame_t *frame);
void fault_handler_machine_check(interrupt_frame_t *frame);
void fault_handler_simd_exception(interrupt_frame_t *frame);

#endif // FAULT_H