#include "moduos/kernel/shell/helpers.h"
#include "moduos/kernel/io/io.h"  
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/power/ACPI.h"
#include "moduos/kernel/kernel.h"
#include "moduos/kernel/macros.h"
static inline void cpuid(uint32_t code, uint32_t* a, uint32_t* d, uint32_t* c, uint32_t* b) {
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(code));
}
void get_cpu_vendor2(char* buf) {
    uint32_t ebx, ecx, edx, eax;
    cpuid(0, &eax, &edx, &ecx, &ebx);

    *(uint32_t*)&buf[0] = ebx;
    *(uint32_t*)&buf[4] = edx;
    *(uint32_t*)&buf[8] = ecx;
    buf[12] = '\0';
}
void poweroff2(void) {
    VGA_Clear();
    VGA_Write("\\cyShutting Down ...\\rr");
    DEBUG_PAUSE(1);
    acpi_shutdown();
    LOG_WARN("ERROR WITH ACPI , USING LEGACY MODE");
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    LOG("System halted. Press the power button to turn off.");
    while (1) asm volatile("hlt");
}
void reboot2(void) {
    VGA_Clear();
    VGA_Write("\\cyRebooting system...\\rr\n");
    DEBUG_PAUSE(1);
    acpi_reboot();
    LOG_WARN("ERROR WITH ACPI , USING LEGACY MODE");
    
    while (1) asm volatile("hlt");
}