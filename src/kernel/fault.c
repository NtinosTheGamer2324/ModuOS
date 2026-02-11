// fault.c - CPU Exception Handlers
#include "moduos/kernel/interrupts/fault.h"
#include "moduos/kernel/interrupts/idt.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/process/process.h"  /* process_exit, schedule */
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/macros.h"
#include "moduos/kernel/panic.h"
#include "moduos/kernel/memory/phys.h"
#include "moduos/kernel/memory/paging.h"

// Debug: last syscall info (from syscall.c)
extern volatile uint64_t g_last_syscall_num;
extern volatile uint64_t g_last_syscall_args[5];

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

static void format_hex64(uint64_t value, char* buffer);

// Store last user-mode page fault details for debugging (COM1 only)
typedef struct {
    int valid;
    uint32_t pid;
    char pname[PROCESS_NAME_MAX];
    uint64_t cr2;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t cs;
    uint64_t err;
} last_user_pf_t;

static volatile last_user_pf_t g_last_user_pf;

static void record_user_pf(uint64_t cr2, uint64_t err, interrupt_frame_t *frame, uint64_t rsp_now) {
    process_t *p = process_get_current();
    g_last_user_pf.valid = 1;
    g_last_user_pf.pid = p ? p->pid : 0;
    for (int i = 0; i < PROCESS_NAME_MAX; i++) {
        g_last_user_pf.pname[i] = p ? p->name[i] : 0;
        if (!p || p->name[i] == 0) break;
    }
    g_last_user_pf.cr2 = cr2;
    g_last_user_pf.rip = frame ? frame->rip : 0;
    g_last_user_pf.rsp = rsp_now;
    g_last_user_pf.rflags = frame ? frame->rflags : 0;
    g_last_user_pf.cs = frame ? frame->cs : 0;
    g_last_user_pf.err = err;
}

void fault_print_last_user_pf(void) {
    if (!g_last_user_pf.valid) {
        com_write_string(COM1_PORT, "[LASTFAULT] none\n");
        return;
    }

    char h[32];
    com_write_string(COM1_PORT, "[LASTFAULT] pid=");
    char b[16]; itoa((int)g_last_user_pf.pid, b, 10); com_write_string(COM1_PORT, b);
    com_write_string(COM1_PORT, " name=");
    com_write_string(COM1_PORT, g_last_user_pf.pname);

    com_write_string(COM1_PORT, " cr2="); format_hex64(g_last_user_pf.cr2, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, " rip="); format_hex64(g_last_user_pf.rip, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, " rsp="); format_hex64(g_last_user_pf.rsp, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, " err="); format_hex64(g_last_user_pf.err, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, " cs="); format_hex64(g_last_user_pf.cs, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, " rflags="); format_hex64(g_last_user_pf.rflags, h); com_write_string(COM1_PORT, h);
    com_write_string(COM1_PORT, "\n");
}

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

    /* Dump raw qwords at the provided frame pointer to avoid struct/layout mismatches. */
    const uint64_t *q = (const uint64_t*)frame;
    char h0[19], h1[19], h2[19], h3[19], h4[19];
    format_hex64(q[0], h0);
    format_hex64(q[1], h1);
    format_hex64(q[2], h2);
    format_hex64(q[3], h3);
    format_hex64(q[4], h4);
    com_write_string(COM1_PORT, "[FAULT] raw[0]="); com_write_string(COM1_PORT, h0);
    com_write_string(COM1_PORT, " raw[1]="); com_write_string(COM1_PORT, h1);
    com_write_string(COM1_PORT, " raw[2]="); com_write_string(COM1_PORT, h2);
    com_write_string(COM1_PORT, "\n");
    com_write_string(COM1_PORT, "[FAULT] raw[3]="); com_write_string(COM1_PORT, h3);
    com_write_string(COM1_PORT, " raw[4]="); com_write_string(COM1_PORT, h4);
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
    char rip_str[19], cs_str[19], rflags_str[19];
    format_hex64(frame->rip, rip_str);
    format_hex64(frame->cs, cs_str);
    format_hex64(frame->rflags, rflags_str);
    
    int pos = 0;
    const char *labels[] = {
        "\nRegister State:\n",
        "  RIP:    ", rip_str, "\n",
        "  CS:     ", cs_str, "\n",
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
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #DE (divide by zero) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 8);
        }
        process_exit(128 + 8);
        for (;;) { __asm__ volatile("hlt"); }
    }
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
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #OF (overflow) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 4);
        }
        process_exit(128 + 4);
        for (;;) { __asm__ volatile("hlt"); }
    }
    fault_panic("Overflow Exception", 
                "INTO instruction detected overflow condition.", 
                frame, "OVERFLOW");
}

void fault_handler_bound_range(interrupt_frame_t *frame) {
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #BR (bound range) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 5);
        }
        process_exit(128 + 5);
        for (;;) { __asm__ volatile("hlt"); }
    }
    fault_panic("BOUND Range Exceeded", 
                "Array index out of bounds (BOUND instruction).", 
                frame, "BOUND_RANGE");
}

/* Dump instruction bytes at RIP to COM1 (best-effort).
 * NOTE: This does not handle the case where RIP itself is unmapped; in that case
 * you may get a page fault while handling #UD.
 */
static void fault_dump_rip_bytes(uint64_t rip) {
    com_write_string(COM1_PORT, "[FAULT] RIP bytes: ");
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rip;
    for (int i = 0; i < 16; i++) {
        uint8_t b = p[i];
        uint8_t hi = (b >> 4) & 0xF;
        uint8_t lo = b & 0xF;
        char h = (hi < 10) ? ('0' + hi) : ('a' + (hi - 10));
        char l = (lo < 10) ? ('0' + lo) : ('a' + (lo - 10));
        com_write_byte(COM1_PORT, h);
        com_write_byte(COM1_PORT, l);
        com_write_byte(COM1_PORT, ' ');
    }
    com_write_string(COM1_PORT, "\n");
}

void fault_handler_invalid_opcode(interrupt_frame_t *frame) {
    fault_dump_rip_bytes(frame->rip);

    /* If CPL=3, treat as user process crash and terminate instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #UD in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 6);
        }
        process_exit(128 + 6);
        for (;;) { __asm__ volatile("hlt"); }
    }

    fault_panic("Invalid Opcode", 
                "CPU encountered an invalid or unsupported instruction.", 
                frame, "INVALID_OPCODE");
}

void fault_handler_device_not_available(interrupt_frame_t *frame) {
    (void)frame;
    /* Lazy FPU switching: this exception (#NM) is expected when CR0.TS=1.
     * Enable FPU for this process and restore its state.
     */
    fpu_lazy_handle_nm();
}

void fault_handler_double_fault(uint64_t error_code, interrupt_frame_t *frame) {
    /* Debug-safe double fault handler: do not call VGA/panic UI (can triple-fault).
     * Just log minimal info to COM1 and halt.
     */
    log_fault("DOUBLE FAULT", frame);
    com_write_string(COM1_PORT, "[FAULT] Double fault error_code=0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (error_code >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, "\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
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
    /* If CPL=3, treat as user process crash and terminate instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #GP in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");

            /* More diagnostics for ring3 debugging */
            char hexbuf[19];
            com_write_string(COM1_PORT, "[FAULT]  error_code=0x");
            format_hex64(error_code, hexbuf);
            com_write_string(COM1_PORT, hexbuf);
            com_write_string(COM1_PORT, "\n");

            com_write_string(COM1_PORT, "[FAULT]  RIP=0x");
            format_hex64(frame->rip, hexbuf);
            com_write_string(COM1_PORT, hexbuf);
            com_write_string(COM1_PORT, " CS=0x");
            format_hex64(frame->cs, hexbuf);
            com_write_string(COM1_PORT, hexbuf);
            com_write_string(COM1_PORT, " RFLAGS=0x");
            format_hex64(frame->rflags, hexbuf);
            com_write_string(COM1_PORT, hexbuf);
            com_write_string(COM1_PORT, "\n");

            /* Best-effort instruction bytes */
            uint8_t bytes[16];
            memset(bytes, 0, sizeof(bytes));
            memcpy(bytes, (const void*)(uintptr_t)frame->rip, sizeof(bytes));
            com_write_string(COM1_PORT, "[FAULT]  bytes:");
            for (int i = 0; i < 16; i++) {
                static const char *hx = "0123456789abcdef";
                char b[4];
                b[0] = ' ';
                b[1] = hx[(bytes[i] >> 4) & 0xF];
                b[2] = hx[bytes[i] & 0xF];
                b[3] = 0;
                com_write_string(COM1_PORT, b);
            }
            com_write_string(COM1_PORT, "\n");

            process_exit(128 + 13);
        }
        process_exit(128 + 13);
        for (;;) { __asm__ volatile("hlt"); }
    }

    char message[512];
    // Debug: print last syscall info to COM1
    com_write_string(COM1_PORT, "[GPF] last_syscall=");
    char sbuf[32];
    itoa((int)g_last_syscall_num, sbuf, 10);
    com_write_string(COM1_PORT, sbuf);
    com_write_string(COM1_PORT, " args=");
    for (int ai = 0; ai < 5; ai++) {
        com_write_string(COM1_PORT, "0x");
        format_hex64((uint64_t)g_last_syscall_args[ai], sbuf);
        com_write_string(COM1_PORT, sbuf);
        if (ai != 4) com_write_string(COM1_PORT, " ");
    }
    com_write_string(COM1_PORT, "\n");
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
    /* Reentrancy guard: if we fault while handling a page fault, stop immediately.
     * For user-mode faults we can still recover by killing the process.
     */
    static volatile int in_pf = 0;
    if (in_pf) {
        uint64_t fa;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fa));

        if ((frame->cs & 3) == 3) {
            process_t *p = process_get_current();
            if (p) {
                com_write_string(COM1_PORT, "\n[FAULT] Nested user #PF in PID ");
                char buf[12];
                itoa((int)p->pid, buf, 10);
                com_write_string(COM1_PORT, buf);

                com_write_string(COM1_PORT, " RIP=0x");
                for (int i = 15; i >= 0; i--) {
                    uint8_t nibble = (frame->rip >> (i * 4)) & 0xF;
                    char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                    com_write_byte(COM1_PORT, hex);
                }

                com_write_string(COM1_PORT, " CR2=0x");
                for (int i = 15; i >= 0; i--) {
                    uint8_t nibble = (fa >> (i * 4)) & 0xF;
                    char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
                    com_write_byte(COM1_PORT, hex);
                }
                com_write_string(COM1_PORT, " (killing process)\n");
            }
            in_pf = 0;
            process_exit(128 + 11);
            for (;;) { __asm__ volatile("hlt"); }
        }

        com_write_string(COM1_PORT, "\n[FAULT] DOUBLE PAGE FAULT while handling page fault. CR2=0x");
        for (int i = 15; i >= 0; i--) {
            uint8_t nibble = (fa >> (i * 4)) & 0xF;
            char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
            com_write_byte(COM1_PORT, hex);
        }
        com_write_string(COM1_PORT, "\n");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    in_pf = 1;

    // Get faulting address from CR2
    uint64_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faulting_address));

    /* Handle Copy-On-Write write faults for user processes.
     * Note: This handler runs in CPL0 even for user faults. We must keep it simple.
     */
    if ((frame->cs & 3) == 3) {
        if ((error_code & PF_PRESENT) && (error_code & PF_WRITE)) {
            uint64_t page = faulting_address & ~0xFFFULL;
            uint64_t pte = paging_get_pte(page);
            if ((pte & PFLAG_PRESENT) && (pte & PFLAG_USER) && (pte & PFLAG_COW) && !(pte & PFLAG_WRITABLE)) {
                uint64_t old_phys = pte & 0xFFFFFFFFFFFFF000ULL;
                uint64_t new_phys = phys_alloc_frame();
                if (new_phys) {
                    // Copy page contents using the paging scratch area.
                    uint64_t scratch = paging_get_scratch_base();
                    if (scratch) {
                        uint64_t scratch2 = scratch + 0x1000ULL;
                        if (paging_map_page(scratch, new_phys, PFLAG_PRESENT | PFLAG_WRITABLE) == 0 &&
                            paging_map_page(scratch2, old_phys, PFLAG_PRESENT | PFLAG_WRITABLE) == 0) {
                            memcpy((void*)(uintptr_t)scratch, (void*)(uintptr_t)scratch2, 4096);
                            paging_unmap_page(scratch2);
                            paging_unmap_page(scratch);

                            // Install new PTE: writable, user, present, clear COW.
                            uint64_t new_pte = (new_phys & 0xFFFFFFFFFFFFF000ULL) | (pte & 0xFFFULL);
                            new_pte |= PFLAG_WRITABLE;
                            new_pte &= ~PFLAG_COW;
                            paging_set_pte(page, new_pte);

                            phys_ref_dec(old_phys);
                            in_pf = 0;
                            return;
                        }
                        paging_unmap_page(scratch2);
                        paging_unmap_page(scratch);
                    }
                    phys_free_frame(new_phys);
                }
            }
        }

        // Not a COW fixable fault: kill.
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "\n[FAULT] User-mode #PF in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
        }

        /* Always dump fault details to COM1 for headless debugging. */
        uint64_t rsp_now;
        __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_now));
        char h[32];
        com_write_string(COM1_PORT, "[FAULT] user_pf cr2=");
        format_hex64(faulting_address, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " rip=");
        format_hex64(frame->rip, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " rsp=");
        format_hex64(rsp_now, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " err=");
        format_hex64(error_code, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, "\n");

        in_pf = 0;
        process_exit(128 + 11);
        for (;;) { __asm__ volatile("hlt"); }
    }

    /* Kernel heap demand paging (fault-handler-safe):
     * For non-present faults in the heap range, allocate a frame and install the missing PTE
     * WITHOUT calling paging_map_page() (which may allocate/zero page tables and fault again).
     */
    {
        const uint64_t KHEAP_START = 0xFFFF800000000000ULL;
        const uint64_t KHEAP_MAX   = KHEAP_START + (32ULL * 1024 * 1024);

        uint64_t page_base = faulting_address & ~0xFFFULL;
        if (!(error_code & PF_PRESENT) && page_base >= KHEAP_START && page_base < KHEAP_MAX) {
            uint64_t pa = phys_alloc_frame();
            if (!pa) {
                com_write_string(COM1_PORT, "[PF] OOM: cannot demand-map heap page\n");
                goto after_heap_demand;
            }

            /* Zero the new physical page via kernel physmap. */
            void *kva = phys_to_virt_kernel(pa);
            if (!kva) { phys_free_frame(pa); goto after_heap_demand; }
            memset(kva, 0, 4096);

            /* Walk page tables via CR3, using kernel physmap for table pages. */
            uint64_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            uint64_t pml4_phys = (cr3 & 0xFFFFFFFFFFFFF000ULL);
            uint64_t *pml4v = (uint64_t*)phys_to_virt_kernel(pml4_phys);
            if (!pml4v) { phys_free_frame(pa); goto after_heap_demand; }

            unsigned i4 = (page_base >> 39) & 0x1FF;
            unsigned i3 = (page_base >> 30) & 0x1FF;
            unsigned i2 = (page_base >> 21) & 0x1FF;
            unsigned i1 = (page_base >> 12) & 0x1FF;

            uint64_t e4 = pml4v[i4];
            if (!(e4 & 1ULL)) { phys_free_frame(pa); goto after_heap_demand; }
            uint64_t *pdpt = (uint64_t*)phys_to_virt_kernel(e4 & 0xFFFFFFFFFFFFF000ULL);
            if (!pdpt) { phys_free_frame(pa); goto after_heap_demand; }

            uint64_t e3 = pdpt[i3];
            if (!(e3 & 1ULL) || (e3 & (1ULL<<7))) { phys_free_frame(pa); goto after_heap_demand; }
            uint64_t *pd = (uint64_t*)phys_to_virt_kernel(e3 & 0xFFFFFFFFFFFFF000ULL);
            if (!pd) { phys_free_frame(pa); goto after_heap_demand; }

            uint64_t e2 = pd[i2];
            if (!(e2 & 1ULL) || (e2 & (1ULL<<7))) { phys_free_frame(pa); goto after_heap_demand; }
            uint64_t *pt = (uint64_t*)phys_to_virt_kernel(e2 & 0xFFFFFFFFFFFFF000ULL);
            if (!pt) { phys_free_frame(pa); goto after_heap_demand; }

            /* Install missing PTE */
            pt[i1] = (pa & 0xFFFFFFFFFFFFF000ULL) | PFLAG_PRESENT | PFLAG_WRITABLE;
            __asm__ volatile("invlpg (%0)" :: "r"(page_base) : "memory");

            /* Success: resume execution (no extra logging here; logging during fault handling
             * can itself cascade into further faults on some setups).
             */
            in_pf = 0;
            return;
        }
    }
after_heap_demand: ;

    /* Stack growth: allow user stack to grow on demand, even from CPL0 (e.g., during exec argv copy).
     * Torvalds-style heuristic: fault addr is in user range, within stack window, and close to user RSP.
     */
    {
        process_t *p = process_get_current();
        if (p && p->is_user) {
            const uint64_t addr = faulting_address;
            const uint64_t user_hi = 0x0000800000000000ULL;
            if (addr < user_hi && p->user_stack_top && p->user_stack_low && p->user_stack_limit) {
                uint64_t page = addr & ~0xFFFULL;

                /* Determine current user RSP (prefer live cpu_state.rsp when in user mode). */
                uint64_t ursp = p->user_rsp;
                if ((frame->cs & 3) == 3) {
                    ursp = p->cpu_state.rsp;
                }

                /* grow only downward and only within 64KiB of current user rsp */
                const uint64_t slack = 64ULL * 1024ULL;
                if (page < p->user_stack_low && page >= p->user_stack_limit) {
                    uint64_t near = (ursp > slack) ? (ursp - slack) : 0;
                    if (page >= near && page < p->user_stack_top) {
                        uint64_t pa = phys_alloc_frame();
                        if (pa) {
                            if (paging_map_page(page, pa, PFLAG_PRESENT | PFLAG_WRITABLE | PFLAG_USER) == 0) {
                                p->user_stack_low = page;
                                /* paging_map_page() already invalidated this VA via invlpg */
                                in_pf = 0;
                                return;
                            }
                            phys_free_frame(pa);
                        }
                    }
                }

                /* If it's a user-range fault and not handled as stack growth, kill the process. */
                if (addr >= p->user_stack_limit && addr < user_hi) {
                    com_write_string(COM1_PORT, "\n[FAULT] User-range #PF not stack-growth; killing PID ");
                    char buf[12]; itoa((int)p->pid, buf, 10); com_write_string(COM1_PORT, buf);
                    com_write_string(COM1_PORT, "\n");
                    in_pf = 0;
                    process_exit(128 + 11);
                    for (;;) { __asm__ volatile("hlt"); }
                }
            }
        }
    }

    /* Minimal early print of CR2 + RIP before doing any heavier formatting. */
    uint64_t rsp_now;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_now));

    /* Record for later retrieval */
    if ((frame->cs & 3) == 3) {
        record_user_pf(faulting_address, error_code, frame, rsp_now);
    }

    com_write_string(COM1_PORT, "\n[FAULT] PAGE FAULT (early) CR2=0x");
    // Debug: print last syscall info to COM1 (kernel PF diagnostics)
    com_write_string(COM1_PORT, "[PF] last_syscall=");
    char sbuf[32];
    itoa((int)g_last_syscall_num, sbuf, 10);
    com_write_string(COM1_PORT, sbuf);
    com_write_string(COM1_PORT, " args=");
    for (int ai = 0; ai < 5; ai++) {
        com_write_string(COM1_PORT, "0x");
        format_hex64((uint64_t)g_last_syscall_args[ai], sbuf);
        com_write_string(COM1_PORT, sbuf);
        if (ai != 4) com_write_string(COM1_PORT, " ");
    }
    com_write_string(COM1_PORT, "\n");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (faulting_address >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, " RIP=0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (frame->rip >> (i * 4)) & 0xF;
        char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        com_write_byte(COM1_PORT, hex);
    }
    com_write_string(COM1_PORT, " RSP=0x");
   for (int i = 15; i >= 0; i--) {
       uint8_t nibble = (rsp_now >> (i * 4)) & 0xF;
       char hex = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
       com_write_byte(COM1_PORT, hex);
   }
   com_write_string(COM1_PORT, "\n");

   /* Page-table walk for CR2 (debug): helps identify which level is missing */
   {
       uint64_t cr3;
       __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
       uint64_t pml4_phys = cr3 & 0xFFFFFFFFFFFFF000ULL;
       uint64_t *pml4v = (uint64_t*)phys_to_virt_kernel(pml4_phys);

       unsigned i4 = (faulting_address >> 39) & 0x1FF;
       unsigned i3 = (faulting_address >> 30) & 0x1FF;
       unsigned i2 = (faulting_address >> 21) & 0x1FF;
       unsigned i1 = (faulting_address >> 12) & 0x1FF;

       uint64_t e4 = pml4v ? pml4v[i4] : 0;
       com_printf(COM1_PORT, "[FAULT] PTW i4=%u e4=0x%08x%08x\n", (unsigned)i4,
                  (uint32_t)(e4 >> 32), (uint32_t)(e4 & 0xFFFFFFFFu));
       if (e4 & 1ULL) {
           uint64_t *pdpt = (uint64_t*)phys_to_virt_kernel(e4 & 0xFFFFFFFFFFFFF000ULL);
           uint64_t e3 = pdpt[i3];
           com_printf(COM1_PORT, "[FAULT] PTW i3=%u e3=0x%08x%08x\n", (unsigned)i3,
                      (uint32_t)(e3 >> 32), (uint32_t)(e3 & 0xFFFFFFFFu));
           if ((e3 & 1ULL) && !(e3 & (1ULL<<7))) {
               uint64_t *pd = (uint64_t*)phys_to_virt_kernel(e3 & 0xFFFFFFFFFFFFF000ULL);
               uint64_t e2 = pd[i2];
               com_printf(COM1_PORT, "[FAULT] PTW i2=%u e2=0x%08x%08x\n", (unsigned)i2,
                          (uint32_t)(e2 >> 32), (uint32_t)(e2 & 0xFFFFFFFFu));
               if ((e2 & 1ULL) && !(e2 & (1ULL<<7))) {
                   uint64_t *pt = (uint64_t*)phys_to_virt_kernel(e2 & 0xFFFFFFFFFFFFF000ULL);
                   uint64_t e1 = pt[i1];
                   com_printf(COM1_PORT, "[FAULT] PTW i1=%u e1=0x%08x%08x\n", (unsigned)i1,
                              (uint32_t)(e1 >> 32), (uint32_t)(e1 & 0xFFFFFFFFu));
               }
           }
       }
   }
    
    char message[512];
    char addr_str[19];
    format_hex64(faulting_address, addr_str);
    
    int pos = 0;
    const char *msg = "Invalid memory access detected.\n\nCR2 (Faulting Address): ";
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
    
    /* If CPL=3, treat as user process crash and terminate instead of panicking kernel.
     * IMPORTANT: do not call schedule() here; process_exit() will context-switch away.
     */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #PF in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
        }

        /* Always dump fault details to COM1 for headless debugging. */
        char h[32];
        com_write_string(COM1_PORT, "[FAULT] user_pf cr2=");
        format_hex64(faulting_address, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " rip=");
        format_hex64(frame->rip, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " rsp=");
        format_hex64(rsp_now, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, " err=");
        format_hex64(error_code, h);
        com_write_string(COM1_PORT, h);
        com_write_string(COM1_PORT, "\n");

        in_pf = 0;
        process_exit(128 + 11);
        for (;;) { __asm__ volatile("hlt"); }
    }
    
    /* Now behave like all other fatal faults: route through the unified panic UI. */
    in_pf = 0;
    fault_panic("Page Fault", message, frame, "PAGE_FAULT");
}

void fault_handler_x87_fpu(interrupt_frame_t *frame) {
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #MF (x87 FPU) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 16);
        }
        process_exit(128 + 16);
        for (;;) { __asm__ volatile("hlt"); }
    }
    fault_panic("x87 FPU Exception", 
                "Floating point unit encountered an error.", 
                frame, "FPU_ERROR");
}

void fault_handler_alignment_check(uint64_t error_code, interrupt_frame_t *frame) {
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #AC (alignment check) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 17);
        }
        process_exit(128 + 17);
        for (;;) { __asm__ volatile("hlt"); }
    }
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
    /* If CPL=3, kill user process instead of panicking kernel. */
    if ((frame->cs & 3) == 3) {
        process_t *p = process_get_current();
        if (p) {
            com_write_string(COM1_PORT, "[FAULT] User-mode #XF (SIMD FP) in PID ");
            char buf[12];
            itoa((int)p->pid, buf, 10);
            com_write_string(COM1_PORT, buf);
            com_write_string(COM1_PORT, " (killing process)\n");
            process_exit(128 + 19);
        }
        process_exit(128 + 19);
        for (;;) { __asm__ volatile("hlt"); }
    }
    fault_panic("SIMD Floating Point Exception", 
                "SSE/AVX instruction caused a floating point exception.", 
                frame, "SIMD_FP");
}
