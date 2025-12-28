#include "moduos/arch/AMD64/interrupts/timer.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/process/process.h"
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static volatile uint64_t system_ticks = 0;
static volatile int in_timer_handler = 0;

void pit_init(uint32_t frequency)
{
    uint32_t divisor = 1193182 / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    irq_install_handler(0, timer_irq_handler);
}

void timer_irq_handler(void) {
    if (in_timer_handler) {
        pic_send_eoi(0);
        return;
    }
    
    in_timer_handler = 1;
    system_ticks++;

    /* IMPORTANT:
     * scheduler_tick() may context-switch away from the current process.
     * If we don't send EOI first, the PIC will keep IRQ0 in-service and we will
     * effectively deadlock further interrupts (keyboard appears "dead").
     */
    pic_send_eoi(0);
    in_timer_handler = 0;

    /* Do not context-switch inside the timer IRQ.
     * Our IRQ stubs are not designed for switching stacks mid-stub.
     * Instead, just request a reschedule and let safe points (yield/syscalls) run it.
     */
    scheduler_request_reschedule();

    usb_tick();  // Process async USB operations
}

uint64_t get_system_ticks(void) {
    return system_ticks;
}