#pragma once

/*
 * Minimal freestanding time.h for ModuOS userland (-nostdlib).
 *
 * Enough to compile thirdparty code (e.g., miniz) which may include <time.h>
 * but doesn't necessarily call time-related functions.
 */

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* Function declarations intentionally omitted: ModuOS userland does not currently provide them. */

