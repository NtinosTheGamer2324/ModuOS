#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/process/process.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static volatile uint64_t system_ticks = 0;
static volatile int in_timer_handler = 0;  // Re-entrancy guard

void pit_init(uint32_t frequency)
{
    uint32_t divisor = 1193182 / frequency;

    // Command byte: channel 0, lo/hi byte, mode 3, binary
    outb(PIT_COMMAND, 0x36);

    // Send frequency divisor
    outb(PIT_CHANNEL0, divisor & 0xFF);        // low byte
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF); // high byte

    // Install timer handler
    irq_install_handler(0, timer_irq_handler);
}

void timer_irq_handler(void) {
    // CRITICAL: Prevent re-entrancy
    // If we're already in the timer handler (shouldn't happen, but be safe),
    // just send EOI and return immediately
    if (in_timer_handler) {
        pic_send_eoi(0);
        return;
    }
    
    in_timer_handler = 1;
    
    // Increment tick counter
    system_ticks++;
    
    // IMPORTANT: Only call scheduler_tick if interrupts are enabled
    // This prevents calling scheduler during syscalls or other critical sections
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    
    if (rflags & (1 << 9)) {  // Check IF (interrupt flag) bit
        scheduler_tick();
    }
    
    // Send EOI
    pic_send_eoi(0);
    
    in_timer_handler = 0;
}

// Utility function to get system ticks
uint64_t get_system_ticks(void) {
    return system_ticks;
}