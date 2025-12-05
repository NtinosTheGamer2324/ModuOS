// fault.c - CPU Exception Handlers
#include "moduos/kernel/interrupts/fault.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/process/process.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/panic.h"

// Assembly stubs (defined in fault.asm)
extern void fault_stub_0(void);
extern void fault_stub_1(void);
extern void fault_stub_2(void);
extern void fault_stub_3(void);
extern void fault_stub_4(void);
extern void fault_stub_5(void);
extern void fault_stub_6(void);
extern void fault_stub_7(void);
extern void fault_stub_8(void);
extern void fault_stub_10(void);
extern void fault_stub_11(void);
extern void fault_stub_12(void);
extern void fault_stub_13(void);
extern void fault_stub_14(void);
extern void fault_stub_16(void);
extern void fault_stub_17(void);
extern void fault_stub_18(void);
extern void fault_stub_19(void);

// Helper: Format hex value to string
static void format_hex64(uint64_t value, char* buffer) {
    const char hex[] = "0123456789ABCDEF";
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buffer[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buffer[18] = '\0';
}

// Helper: Log to COM port
static void log_fault(const char *name, interrupt_frame_t *frame) {
    com_write_string(COM1_PORT, "\n[FAULT] ");
    com_write_string(COM1_PORT, name);
    com_write_string(COM1_PORT, "\n");
    
    com_write_string(COM1_PORT, "[FAULT] RIP: 0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (frame->rip >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");
}

// Helper: Build register info string
static void build_register_info(interrupt_frame_t *frame, char *buffer, int max_len) {
    char rip_str[19], rsp_str[19], rflags_str[19];
    format_hex64(frame->rip, rip_str);
    format_hex64(frame->rsp, rsp_str);
    format_hex64(frame->rflags, rflags_str);
    
    int pos = 0;
    const char *labels[] = {
        "\nRegister State:\n",
        "  RIP:    ", rip_str, "\n",
        "  RSP:    ", rsp_str, "\n",
        "  RFLAGS: ", rflags_str, NULL
    };
    
    for (int i = 0; labels[i] != NULL && pos < max_len - 1; i++) {
        const char *str = labels[i];
        while (*str && pos < max_len - 1) {
            buffer[pos++] = *str++;
        }
    }
    buffer[pos] = '\0';
}

// Helper: Trigger panic with fault info
static void fault_panic(const char *title, const char *description, interrupt_frame_t *frame, const char *error_code) {
    log_fault(title, frame);
    
    // Build full message with description + register info
    char message[512];
    int pos = 0;
    
    // Add description
    while (*description && pos < 300) {
        message[pos++] = *description++;
    }
    
    // Add register info
    char reg_info[256];
    build_register_info(frame, reg_info, sizeof(reg_info));
    const char *reg_ptr = reg_info;
    while (*reg_ptr && pos < 510) {
        message[pos++] = *reg_ptr++;
    }
    message[pos] = '\0';
    
    // Build tips with process info if available
    char tips[256];
    process_t *proc = process_get_current();
    if (proc) {
        int tip_pos = 0;
        const char *proc_label = "Faulting Process: ";
        while (*proc_label && tip_pos < 100) {
            tips[tip_pos++] = *proc_label++;
        }
        for (int i = 0; proc->name[i] && tip_pos < 200; i++) {
            tips[tip_pos++] = proc->name[i];
        }
        const char *pid_label = " (PID: ";
        while (*pid_label && tip_pos < 220) {
            tips[tip_pos++] = *pid_label++;
        }
        // Simple PID to string conversion
        char pid_str[16];
        int pid_len = 0;
        int pid = proc->pid;
        do {
            pid_str[pid_len++] = '0' + (pid % 10);
            pid /= 10;
        } while (pid > 0 && pid_len < 15);
        for (int i = pid_len - 1; i >= 0; i--) {
            tips[tip_pos++] = pid_str[i];
        }
        tips[tip_pos++] = ')';
        tips[tip_pos] = '\0';
    } else {
        tips[0] = '\0';  // No process info available
    }
    
    // Call your panic system
    panic(title, message, tips[0] ? tips : NULL, "CPU", error_code, 6);
}

// Initialize fault handlers
void fault_init(void) {
    COM_LOG_INFO(COM1_PORT, "Initializing CPU exception handlers");
    
    // Register all fault handlers (interrupt gates, ring 0)
    idt_set_entry(0, fault_stub_0, 0x8E);
    idt_set_entry(1, fault_stub_1, 0x8E);
    idt_set_entry(2, fault_stub_2, 0x8E);
    idt_set_entry(3, fault_stub_3, 0x8E);
    idt_set_entry(4, fault_stub_4, 0x8E);
    idt_set_entry(5, fault_stub_5, 0x8E);
    idt_set_entry(6, fault_stub_6, 0x8E);
    idt_set_entry(7, fault_stub_7, 0x8E);
    idt_set_entry(8, fault_stub_8, 0x8E);
    idt_set_entry(10, fault_stub_10, 0x8E);
    idt_set_entry(11, fault_stub_11, 0x8E);
    idt_set_entry(12, fault_stub_12, 0x8E);
    idt_set_entry(13, fault_stub_13, 0x8E);
    idt_set_entry(14, fault_stub_14, 0x8E);
    idt_set_entry(16, fault_stub_16, 0x8E);
    idt_set_entry(17, fault_stub_17, 0x8E);
    idt_set_entry(18, fault_stub_18, 0x8E);
    idt_set_entry(19, fault_stub_19, 0x8E);
    
    COM_LOG_OK(COM1_PORT, "CPU exception handlers initialized");
}

// === EXCEPTION HANDLERS ===

void fault_handler_divide_error(interrupt_frame_t *frame) {
    fault_panic("Divide by Zero Exception", 
                "Attempted division by zero operation.", 
                frame, "DIV_BY_ZERO");
}

void fault_handler_debug(interrupt_frame_t *frame) {
    COM_LOG_INFO(COM1_PORT, "Debug exception (ignored)");
}

void fault_handler_nmi(interrupt_frame_t *frame) {
    fault_panic("Non-Maskable Interrupt", 
                "Hardware NMI occurred - possible hardware failure.", 
                frame, "NMI");
}

void fault_handler_breakpoint(interrupt_frame_t *frame) {
    COM_LOG_INFO(COM1_PORT, "Breakpoint hit");
    VGA_Write("\\cy[DEBUG] Breakpoint at 0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (frame->rip >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        VGA_WriteChar(hex);
    }
    VGA_Write("\\rr\n");
}

void fault_handler_overflow(interrupt_frame_t *frame) {
    fault_panic("Overflow Exception", 
                "INTO instruction detected overflow condition.", 
                frame, "OVERFLOW");
}

void fault_handler_bound_range(interrupt_frame_t *frame) {
    fault_panic("BOUND Range Exceeded", 
                "Array index out of bounds (BOUND instruction).", 
                frame, "BOUND_RANGE");
}

void fault_handler_invalid_opcode(interrupt_frame_t *frame) {
    fault_panic("Invalid Opcode", 
                "CPU encountered an invalid or unsupported instruction.", 
                frame, "INVALID_OPCODE");
}

void fault_handler_device_not_available(interrupt_frame_t *frame) {
    fault_panic("Device Not Available", 
                "FPU/SSE instruction executed without proper initialization.", 
                frame, "NO_FPU");
}

void fault_handler_double_fault(uint64_t error_code, interrupt_frame_t *frame) {
    log_fault("DOUBLE FAULT", frame);
    
    char message[256];
    char err_str[19];
    format_hex64(error_code, err_str);
    
    int pos = 0;
    const char *msg = "Exception occurred while handling another exception.\nError Code: ";
    while (*msg && pos < 200) {
        message[pos++] = *msg++;
    }
    for (int i = 0; err_str[i] && pos < 250; i++) {
        message[pos++] = err_str[i];
    }
    message[pos] = '\0';
    
    panic("Double Fault Exception", 
          message,
          "This usually indicates stack overflow or corrupted exception handlers.",
          "CPU", "DOUBLE_FAULT", 6);
}

void fault_handler_invalid_tss(uint64_t error_code, interrupt_frame_t *frame) {
    fault_panic("Invalid TSS", 
                "Task State Segment reference is invalid.", 
                frame, "INVALID_TSS");
}

void fault_handler_segment_not_present(uint64_t error_code, interrupt_frame_t *frame) {
    char message[256];
    char selector[19];
    format_hex64(error_code & 0xFFFF, selector);
    
    int pos = 0;
    const char *msg = "Referenced segment is not present in memory.\nSegment Selector: ";
    while (*msg && pos < 200) {
        message[pos++] = *msg++;
    }
    for (int i = 0; selector[i] && pos < 250; i++) {
        message[pos++] = selector[i];
    }
    message[pos] = '\0';
    
    fault_panic("Segment Not Present", message, frame, "SEG_NOT_PRESENT");
}

void fault_handler_stack_fault(uint64_t error_code, interrupt_frame_t *frame) {
    fault_panic("Stack Segment Fault", 
                "Stack segment limit exceeded or stack segment not present.", 
                frame, "STACK_FAULT");
}

void fault_handler_general_protection(uint64_t error_code, interrupt_frame_t *frame) {
    char message[512];
    int pos = 0;
    
    const char *base_msg = "Memory protection violation or privilege level error.";
    while (*base_msg && pos < 100) {
        message[pos++] = *base_msg++;
    }
    
    if (error_code != 0) {
        char selector[19];
        format_hex64(error_code & 0xFFFF, selector);
        
        const char *seg_msg = "\n\nSegment Selector: ";
        while (*seg_msg && pos < 200) {
            message[pos++] = *seg_msg++;
        }
        for (int i = 0; selector[i] && pos < 250; i++) {
            message[pos++] = selector[i];
        }
        
        if (error_code & 1) {
            const char *ext = "\nCaused by external event";
            while (*ext && pos < 300) {
                message[pos++] = *ext++;
            }
        }
        if (error_code & 2) {
            const char *idt = "\nIDT table reference";
            while (*idt && pos < 350) {
                message[pos++] = *idt++;
            }
        } else {
            const char *gdt = "\nGDT/LDT table reference";
            while (*gdt && pos < 350) {
                message[pos++] = *gdt++;
            }
        }
    }
    message[pos] = '\0';
    
    fault_panic("General Protection Fault", message, frame, "GPF");
}

void fault_handler_page_fault(uint64_t error_code, interrupt_frame_t *frame) {
    // Get faulting address from CR2
    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));
    
    char message[512];
    char addr_str[19];
    format_hex64(faulting_address, addr_str);
    
    int pos = 0;
    const char *msg = "Invalid memory access detected.\n\nFaulting Address: ";
    while (*msg && pos < 100) {
        message[pos++] = *msg++;
    }
    for (int i = 0; addr_str[i] && pos < 150; i++) {
        message[pos++] = addr_str[i];
    }
    
    const char *flags = "\n\nAccess Type: ";
    while (*flags && pos < 200) {
        message[pos++] = *flags++;
    }
    
    if (error_code & PF_PRESENT) {
        const char *p = "Protection violation";
        while (*p && pos < 250) {
            message[pos++] = *p++;
        }
    } else {
        const char *p = "Page not present";
        while (*p && pos < 250) {
            message[pos++] = *p++;
        }
    }
    
    if (error_code & PF_WRITE) {
        const char *w = " (Write)";
        while (*w && pos < 280) {
            message[pos++] = *w++;
        }
    } else {
        const char *r = " (Read)";
        while (*r && pos < 280) {
            message[pos++] = *r++;
        }
    }
    
    const char *mode = "\nPrivilege Level: ";
    while (*mode && pos < 320) {
        message[pos++] = *mode++;
    }
    
    if (error_code & PF_USER) {
        const char *u = "User mode";
        while (*u && pos < 350) {
            message[pos++] = *u++;
        }
    } else {
        const char *k = "Kernel mode";
        while (*k && pos < 350) {
            message[pos++] = *k++;
        }
    }
    
    if (error_code & PF_RESERVED) {
        const char *res = "\nReserved bit violation detected";
        while (*res && pos < 400) {
            message[pos++] = *res++;
        }
    }
    
    if (error_code & PF_INSTRUCTION) {
        const char *inst = "\nCaused by instruction fetch";
        while (*inst && pos < 450) {
            message[pos++] = *inst++;
        }
    }
    
    message[pos] = '\0';
    
    com_write_string(COM1_PORT, "\n[FAULT] PAGE FAULT at 0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (faulting_address >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");
    
    fault_panic("Page Fault Exception", message, frame, "PAGE_FAULT");
}

void fault_handler_x87_fpu(interrupt_frame_t *frame) {
    fault_panic("x87 FPU Exception", 
                "Floating point unit encountered an error.", 
                frame, "FPU_ERROR");
}

void fault_handler_alignment_check(uint64_t error_code, interrupt_frame_t *frame) {
    fault_panic("Alignment Check Exception", 
                "Unaligned memory access detected with AC flag set.", 
                frame, "ALIGNMENT");
}

void fault_handler_machine_check(interrupt_frame_t *frame) {
    fault_panic("Machine Check Exception", 
                "Hardware error detected by CPU - possible hardware failure.", 
                frame, "MACHINE_CHECK");
}

void fault_handler_simd_exception(interrupt_frame_t *frame) {
    fault_panic("SIMD Floating Point Exception", 
                "SSE/AVX instruction caused a floating point exception.", 
                frame, "SIMD_FP");
}