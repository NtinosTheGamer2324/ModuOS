#include "libc.h"

/* Must byte-for-byte match the rtc_time_t layout defined in the RTC kernel
 * module (moduos/modules/rtc.c). If that struct changes, this must change
 * too — there's no shared header between kernel module and userland here,
 * so keep them in sync manually.
 */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} rtc_time_t;

#define RTC_DEV_PATH "$/dev/rtc"

static const char *month_name(uint8_t m) {
    static const char *names[] = {
        "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (m < 1 || m > 12) return names[0];
    return names[m];
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open(RTC_DEV_PATH, O_RDONLY, 0);
    if (fd < 0) {
        puts("rtc: failed to open " RTC_DEV_PATH);
        return 1;
    }

    rtc_time_t t;
    ssize_t n = read(fd, &t, sizeof(t));
    close(fd);

    if (n != (ssize_t)sizeof(t)) {
        printf("rtc: short/failed read (got %d, expected %d)\n",
               (int)n, (int)sizeof(t));
        return 1;
    }

    printf("%04u-%s-%02u %02u:%02u:%02u\n",
           (unsigned)t.year, month_name(t.month), (unsigned)t.day,
           (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);

    return 0;
}