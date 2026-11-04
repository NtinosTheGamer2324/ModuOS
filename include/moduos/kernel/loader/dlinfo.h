#ifndef MODUOS_LOADER_DLINFO_H
#define MODUOS_LOADER_DLINFO_H

#include <stdint.h>

/*
 * Hand-off block between the kernel ELF loader and ld-moduos.sqrl.
 *
 * The kernel maps both the executable (ET_EXEC, linked at 0x400000) and the
 * interpreter into the new address space and enters the interpreter. A block
 * is used instead of an auxv on the stack because the two exec paths
 * (exec.c and execve) build user stacks differently; SYS_DLINFO is the one
 * channel both can serve identically.
 */

/* Link address of ld-moduos.sqrl. Must stay clear of every executable. */
#define DL_INTERP_BASE 0x04000000ULL

#define DLINFO_VERSION 1

typedef struct {
    uint64_t version;
    uint64_t exe_phdr;   /* user VA of the executable's phdr table, 0 if unmapped */
    uint64_t exe_phnum;
    uint64_t exe_phent;
    uint64_t exe_entry;
    uint64_t interp_base;
} dlinfo_t;

#endif
