#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_PROCLIST_NAME_MAX 64

typedef struct md_proclist_entry_u {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint64_t total_time;     // scheduler ticks (see process_t.total_time)
    char name[MD_PROCLIST_NAME_MAX];
} md_proclist_entry_u;

#ifdef __cplusplus
}
#endif
