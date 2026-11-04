#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

typedef long         off_t;
typedef int          pid_t;
typedef int          uid_t;
typedef int          gid_t;
typedef unsigned int mode_t;

#endif /* _SYS_TYPES_H */
