#include "sqrm_elf.h"

#include "moduos/kernel/errno.h"
#include "moduos/kernel/memory/memory.h"
#include "moduos/kernel/memory/string.h"

// NOTE: This file is part of an in-progress SQRM refactor.
// For now, we keep a minimal implementation that compiles.
// Proper GOT + relocation support will be implemented here as part of the refactor.

int sqrm_elf_load_from_buffer(const uint8_t *buf, size_t rd, sqrm_elf_loaded_t *out) {
    (void)buf;
    (void)rd;
    (void)out;
    return -ENOSYS;
}
