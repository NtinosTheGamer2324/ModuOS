#include "moduos/kernel/user_identity.h"

#include "moduos/kernel/process/process.h"

const char *user_identity_get(const struct process *proc) {
    if (!proc) return NULL;
    if (proc->uid == 0) return "root";
    return NULL;
}
