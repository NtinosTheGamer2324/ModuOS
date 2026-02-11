#pragma once

#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Result of loading an ELF64 ET_DYN SQRM module image from an in-memory buffer.
typedef struct {
    uint8_t *image;
    uint64_t image_size;  // includes GOT region

    uint64_t min_vaddr;
    uint64_t max_vaddr;

    uint64_t entry_offset; // image + entry_offset = sqrm_module_init

    sqrm_module_desc_t desc;              // v1 prefix (copied)
    const sqrm_module_desc_v2_t *desc_v2; // pointer within image (NULL if not v2)
} sqrm_elf_loaded_t;

// Loads, relocates, and prepares a module image from an already-read file buffer.
// On success, caller owns out->image and must kfree() it.
int sqrm_elf_load_from_buffer(const uint8_t *buf, size_t rd, sqrm_elf_loaded_t *out);

#ifdef __cplusplus
}
#endif
