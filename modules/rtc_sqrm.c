#include "moduos/kernel/sqrm.h"
#include "moduos/fs/devfs.h"

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "RTC");

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} rtc_time_t;

/* Defenitions */
#define RTCCMD 0x70
#define RTCRES 0x71

    /* Requests */
#define GETSEC 0x00
#define GETMIN 0x02
#define GETHOUR 0x04
#define GETDAY 0x07
#define GETMONTH 0x08
#define GETYEAR 0x09
#define STATUSA 0x0A
#define STATUSB 0x0B
#define FORMATCONFIG 0x0B

#define UIP_FLAG 0x80

/* GLOBALS */
const sqrm_kernel_api_t *g_api;

/* Helpers */
static void kprint(const char *s) {
    g_api->com_write_string(0x3F8, s);
}

static int bcd_to_bin(uint8_t bcd) {
    return ((bcd & 0xF0) >> 4) * 10 + (bcd & 0x0F);
}

static void *rtc_memcpy(void *dest, const void *src, size_t len) {
    const unsigned char *s = src;
    unsigned char *d = dest;
    while (len--) {
        *d++ = *s++;
    }
    return dest;
}

/* Reads a CMOS register with NMI kept disabled (bit 0x80 on the select write). */
static uint8_t cmos_read(uint8_t reg) {
    g_api->outb(RTCCMD, reg | 0x80);
    return g_api->inb(RTCRES);
}

static int rtc_update_in_progress(void) {
    return cmos_read(STATUSA) & UIP_FLAG;
}

/* RTC Helpers */
static rtc_time_t rtc_get_time(void) {
    rtc_time_t t1, t2;
    uint64_t ms_start = g_api->ticks_to_ms(g_api->get_system_ticks());

    /* Wait for any in-progress update to finish before the first read */
    while (rtc_update_in_progress()) {
        if ((g_api->ticks_to_ms(g_api->get_system_ticks()) - ms_start) >= 2000) {
            kprint("[RTC] Watchdog timeout waiting for UIP clear\n");
            rtc_time_t fail_fallback = {0};
            return fail_fallback;
        }
    }

    while (1) {
        /* Watchdog timer */
        if ((g_api->ticks_to_ms(g_api->get_system_ticks()) - ms_start) >= 2000) {
            kprint("[RTC] Watchdog timeout during full snapshot\n");
            rtc_time_t fail_fallback = {0};
            return fail_fallback;
        }

        t1.second = cmos_read(GETSEC);
        t1.minute = cmos_read(GETMIN);
        t1.hour   = cmos_read(GETHOUR);
        t1.day    = cmos_read(GETDAY);
        t1.month  = cmos_read(GETMONTH);
        t1.year   = cmos_read(GETYEAR);

        /* If an update started mid-read, this snapshot is unreliable; retry */
        if (rtc_update_in_progress()) {
            continue;
        }

        t2.second = cmos_read(GETSEC);
        t2.minute = cmos_read(GETMIN);
        t2.hour   = cmos_read(GETHOUR);
        t2.day    = cmos_read(GETDAY);
        t2.month  = cmos_read(GETMONTH);
        t2.year   = cmos_read(GETYEAR);

        /* Break loop only if both full register layouts match identically */
        if (t1.second == t2.second && t1.minute == t2.minute &&
            t1.hour   == t2.hour   && t1.day    == t2.day    &&
            t1.month  == t2.month  && t1.year   == t2.year) {
            break;
        }
    }

    uint8_t status_b = cmos_read(STATUSB);
    int is_binary = status_b & 0x04; /* bit 2: 1 = binary, 0 = BCD */
    int is_24hr   = status_b & 0x02; /* bit 1: 1 = 24hr, 0 = 12hr */

    rtc_time_t final_time;

    if (is_binary) {
        final_time.second = t2.second;
        final_time.minute = t2.minute;
        final_time.hour   = t2.hour & 0x7F; /* mask off PM bit if present */
        final_time.day    = t2.day;
        final_time.month  = t2.month;
        final_time.year   = t2.year + 2000;
    } else {
        final_time.second = bcd_to_bin(t2.second);
        final_time.minute = bcd_to_bin(t2.minute);
        final_time.hour   = bcd_to_bin(t2.hour & 0x7F);
        final_time.day    = bcd_to_bin(t2.day);
        final_time.month  = bcd_to_bin(t2.month);
        final_time.year   = bcd_to_bin(t2.year) + 2000;
    }

    /* 12-hour mode: bit 7 of the raw hour byte marks PM and must be folded in
     * after BCD/binary decoding of the low bits. */
    if (!is_24hr && (t2.hour & 0x80)) {
        final_time.hour = (final_time.hour % 12) + 12;
    }

    return final_time;
}

/* DevFS stubs*/
static void *devfs_open_stub(void *ctx, int flags) {
    (void)flags;
    return ctx; /* return ctx as the handle */
}

static int devfs_close_stub(void *ctx) {
    (void)ctx;
    return 0;
}

static ssize_t devfs_write_stub(void *ctx, const void *buf, size_t count) {
    (void)ctx; (void)buf; (void)count;
    return -1; /* read-only nodes */
}

/* DevFS Function */
ssize_t devfs_read(void *ctx, void *buf, size_t n) {
    (void)ctx;

    if (n < sizeof(rtc_time_t)) {
        return -1;
    }

    rtc_time_t time = rtc_get_time();
    rtc_memcpy(buf, &time, sizeof(time));

    return (ssize_t)sizeof(time);
}

/* ops */
static devfs_device_ops_t ops_rtc = {
    .name        = "rtc",
    .open        = devfs_open_stub,
    .read        = devfs_read,
    .write       = devfs_write_stub,
    .close       = devfs_close_stub,
    .can_replace = NULL,
};


/* Entry */
int sqrm_module_init(const sqrm_kernel_api_t *api) {

    g_api = api;

    g_api->devfs_register_path("rtc", &ops_rtc, NULL);

    return 0;
}