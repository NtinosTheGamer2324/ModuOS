#ifndef MODUOS_KERNEL_MEMORY_USERCOPY_H
#define MODUOS_KERNEL_MEMORY_USERCOPY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Validate that a user pointer range is mapped with the USER bit, and copy safely.
 * Returns 0 on success, non-zero on failure.
 */
int usercopy_to_user(void *user_dst, const void *kernel_src, size_t n);
int usercopy_from_user(void *kernel_dst, const void *user_src, size_t n);

/* String helper: copy NUL-terminated string into kernel buf (always NUL-terminates). */
int usercopy_string_from_user(char *kernel_dst, const char *user_src, size_t max_len);

#endif
