// shm_demo_writer.c — Shared Memory demo (writer side)
//
// Creates the shared segment "/shm_demo", writes an incrementing counter and
// its own PID into it once a second, then exits WITHOUT unlinking the
// segment. That's on purpose: it demonstrates that a shm segment survives
// its creator exiting (it's the exact refcounting path fork()/munmap()/exit
// already all shared) — run shm_demo_reader afterwards and it can still
// open the segment and see the last value written.
//
// Run this FIRST, in its own terminal/session:
//   shm_demo_writer
// Then, while it's still running (or after), run shm_demo_reader elsewhere
// to watch the same memory update live, or see the last value survive.
//
// Only libc.h is used.

#include "libc.h"

#define SHM_NAME "/shm_demo"
#define TICKS    15

typedef struct {
    volatile uint32_t counter;
    volatile int      writer_pid;
    volatile char     message[64];
} demo_shm_t;

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("[writer] shm_open(\"%s\", O_RDWR|SHM_O_CREAT, size=%d)\n",
           SHM_NAME, (int)sizeof(demo_shm_t));

    int h = shm_open(SHM_NAME, O_RDWR | SHM_O_CREAT, 0666, sizeof(demo_shm_t));
    if (h < 0) {
        printf("[writer] shm_open failed: %d\n", h);
        return 1;
    }

    demo_shm_t *shm = (demo_shm_t*)mmap(NULL, sizeof(demo_shm_t),
                                         PROT_R | PROT_W, MAP_SHARED, h);
    if (shm == MAP_FAILED) {
        printf("[writer] mmap failed\n");
        return 1;
    }
    printf("[writer] mapped ok, pid=%d\n", getpid());

    shm->counter    = 0;
    shm->writer_pid = getpid();
    sprintf((char*)shm->message, "hello from writer pid %d", getpid());

    for (int i = 0; i < TICKS; i++) {
        shm->counter = (uint32_t)(i + 1);
        printf("[writer] tick %2d -> counter=%d\n", i, shm->counter);
        sleep(1);
    }

    printf("[writer] done. NOT calling shm_unlink() -- the segment stays\n");
    printf("[writer] alive with counter=%d for shm_demo_reader to find,\n", shm->counter);
    printf("[writer] even after this process exits.\n");

    munmap(shm, sizeof(demo_shm_t));
    return 0;
}