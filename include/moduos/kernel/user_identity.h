#ifndef USER_IDENTITY_H
#define USER_IDENTITY_H

#include <stdint.h>

/* Kernel-only identity (never user-login). */
#define KERNEL_UID 0xFFFFFFFFu
#define KERNEL_USERNAME "KERNEL64"

static inline int uid_is_kernel(uint32_t uid) {
    return uid == KERNEL_UID;
}

struct process;
const char *user_identity_get(const struct process *proc);

#endif /* USER_IDENTITY_H */
