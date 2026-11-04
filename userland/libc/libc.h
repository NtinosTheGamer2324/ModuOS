/* libc.h - migration shim.
 *
 * The old monolithic libc.h is gone; the library is now split into ISO
 * headers (stdio.h, string.h, ...) plus moduos.h for ModuOS-only APIs.
 * This header just pulls everything in so existing apps keep compiling
 * while you move them over to the individual headers.
 *
 * Behaviour changes to watch for when migrating:
 *   putc(c)        -> putc_raw(c)  (ISO putc takes a FILE*)
 *   setenv(k, v)   -> setenv(k, v, 1)
 *   O_CREAT        is now 0x0040 (what the kernel really checks)
 *   errno          is one shared variable in libc.sqrl, not one per TU
 */
#ifndef _LIBC_H
#define _LIBC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "errno.h"
#include "ctype.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "time.h"
#include "signal.h"
#include "setjmp.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/types.h"
#include "sys/wait.h"
#include "moduos.h"

#endif /* _LIBC_H */
