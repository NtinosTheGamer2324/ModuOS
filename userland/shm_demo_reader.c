// shm_demo_reader.c — Shared Memory demo (reader side)
//
// Opens the SAME segment "/shm_demo" the writer created (does not create
// it -- no SHM_O_CREAT here) and polls it once a second, printing whatever
// the writer last wrote. Run shm_demo_writer first (or at the same time in
// another session) to see it working.
//
// After the polling loop, it also proves write protection is real: it
// re-opens the segment read-only and tries to mmap() it with PROT_W, which
// must fail with -EACCES, since this handle only requested O_RDONLY.
//
// Finally it calls shm_unlink() to remove the segment from the namespace
// (cleanup for repeat runs of the demo).
//

#include "libc.h"

#define SHM_NAME "/shm_demo"
#define TICKS    10

typedef struct {
    volatile uint32_t counter;
    volatile int      writer_pid;
    volatile char     message[64];
} demo_shm_t;

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("[reader] shm_open(\"%s\", O_RDONLY)\n", SHM_NAME);
    int h = shm_open(SHM_NAME, O_RDONLY, 0, 0);
    if (h < 0) {
        printf("[reader] shm_open failed: %d (run shm_demo_writer first!)\n", h);
        return 1;
    }

    demo_shm_t *shm = (demo_shm_t*)mmap(NULL, sizeof(demo_shm_t),
                                         PROT_R, MAP_SHARED, h);
    if (shm == MAP_FAILED) {
        printf("[reader] mmap failed\n");
        return 1;
    }
    printf("[reader] mapped ok, pid=%d\n", getpid());

    uint32_t last = 0xFFFFFFFF;
    for (int i = 0; i < TICKS; i++) {
        if (shm->counter != last) {
            last = shm->counter;
            printf("[reader] counter=%d  writer_pid=%d  message=\"%s\"\n",
                   shm->counter, shm->writer_pid, (const char*)shm->message);
        } else {
            printf("[reader] (unchanged) counter=%d\n", shm->counter);
        }
        sleep(1);
    }

    munmap(shm, sizeof(demo_shm_t));

    /* Prove write protection is enforced: a fresh read-only handle must
     * refuse a PROT_W mapping with -EACCES. */
    printf("[reader] verifying read-only enforcement...\n");
    int h2 = shm_open(SHM_NAME, O_RDONLY, 0, 0);
    if (h2 < 0) {
        printf("[reader]   shm_open (2nd) failed: %d\n", h2);
    } else {
        void *p = mmap(NULL, sizeof(demo_shm_t), PROT_R | PROT_W, MAP_SHARED, h2);
        if (p == MAP_FAILED) {
            printf("[reader]   PASS: PROT_W mmap on an O_RDONLY handle was rejected, as expected\n");
        } else {
            printf("[reader]   FAIL: PROT_W mmap on an O_RDONLY handle unexpectedly SUCCEEDED\n");
            munmap(p, sizeof(demo_shm_t));
        }
    }

    printf("[reader] shm_unlink(\"%s\")\n", SHM_NAME);
    int rc = shm_unlink(SHM_NAME);
    printf("[reader] shm_unlink returned %d\n", rc);

    return 0;
}