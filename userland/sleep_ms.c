#include "libc.h"

static void sleep_ms(uint32_t ms) {
    uint64_t end = time_ms() + ms;
    while (time_ms() < end)
    {
        //we do not want to yield, it will burn CPU cycles, but anyway. we do not want to yield because Automation Manager will think Oh wait, it finished (when it didn't) 
        // fg /Apps/sleep_ms.sqr 1000
        printf("%d", time_ms());
    }
}

int md_main(long argc, char **argv) {
    if (argc != 1) {
        printf("Usage: %s <time_ms>\n", argv[0]);
        return 1;
    }

    sleep_ms((uint32_t)strtol(argv[1], NULL, 10));

    return 0;
}