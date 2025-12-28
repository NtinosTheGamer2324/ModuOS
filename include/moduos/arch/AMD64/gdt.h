#ifndef MODUOS_AMD64_GDT_H
#define MODUOS_AMD64_GDT_H

#include <stdint.h>

/* Segment selectors (must match GDT layout in gdt.c) */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x23 /* index=4, RPL=3 */
#define USER_CS   0x2B /* index=5, RPL=3 */
#define TSS_SEL   0x30 /* index=6 */

void amd64_gdt_init(void);

/* Set the kernel stack (RSP0) used when entering ring0 from ring3 (interrupts/syscalls). */
void amd64_tss_set_rsp0(uint64_t rsp0);

#endif
