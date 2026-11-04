#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include "types.h"

#define WNOHANG 1

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#endif /* _SYS_WAIT_H */
