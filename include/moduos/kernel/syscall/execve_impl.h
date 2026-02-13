#ifndef MODUOS_KERNEL_SYSCALL_EXECVE_IMPL_H
#define MODUOS_KERNEL_SYSCALL_EXECVE_IMPL_H

#include <stdint.h>

int sys_execve_impl(const char *path_user, char *const *argv_user, char *const *envp_user);

#endif
