#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD64API_PID_NAME_MAX 64
#define MD64API_CWD_MAX 256
#define MD64API_MAX_PROCESSES 256

typedef struct md64api_pid_info_u {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint32_t state;        // process_state_t
    uint32_t priority;
    uint64_t total_time;   // scheduler ticks
    uint64_t user_rip;
    uint64_t user_rsp;
    uint8_t  is_user;

    // Approx memory usage (bytes), derived from tracked ranges.
    uint64_t mem_image_bytes;
    uint64_t mem_heap_bytes;
    uint64_t mem_mmap_bytes;
    uint64_t mem_stack_bytes;
    uint64_t mem_total_bytes;

    char name[MD64API_PID_NAME_MAX];
    char cwd[MD64API_CWD_MAX];
} md64api_pid_info_u;

#ifdef __cplusplus
}
#endif
