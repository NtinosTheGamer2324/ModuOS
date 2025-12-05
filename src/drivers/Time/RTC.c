#include <stddef.h>
#include "moduos/drivers/Time/RTC.h"
#include "moduos/kernel/io/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t rtc_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static void rtc_wait_update_end(void)
{
    while (rtc_read(0x0A) & 0x80) {
        // Wait while update in progress flag is set
    }
}

static uint8_t bcd_to_bin(uint8_t val)
{
    return (val & 0x0F) + ((val >> 4) * 10);
}

void rtc_get_time(uint8_t* hour, uint8_t* min, uint8_t* sec)
{
    rtc_wait_update_end();

    uint8_t raw_sec = rtc_read(0x00);
    uint8_t raw_min = rtc_read(0x02);
    uint8_t raw_hour = rtc_read(0x04);

    uint8_t regB = rtc_read(0x0B);

    if (!(regB & 0x04)) {
        // Convert from BCD to binary if needed
        raw_sec = bcd_to_bin(raw_sec);
        raw_min = bcd_to_bin(raw_min);
        raw_hour = bcd_to_bin(raw_hour);
    }

    if (sec)  *sec  = raw_sec;
    if (min)  *min  = raw_min;
    if (hour) *hour = raw_hour;
}

void rtc_wait_seconds(uint8_t seconds)
{
    if (seconds == 0) return;

    uint8_t start_sec, current_sec;
    rtc_get_time(NULL, NULL, &start_sec);

    do {
        rtc_get_time(NULL, NULL, &current_sec);
        // Handle wrap-around from 59 to 0
        if (current_sec < start_sec) {
            current_sec += 60;
        }
    } while ((current_sec - start_sec) < seconds);
}
void rtc_get_datetime(rtc_datetime_t* dt)
{
    if (!dt) return;

    rtc_wait_update_end();

    uint8_t sec   = rtc_read(0x00);
    uint8_t min   = rtc_read(0x02);
    uint8_t hour  = rtc_read(0x04);
    uint8_t day   = rtc_read(0x07);
    uint8_t month = rtc_read(0x08);
    uint8_t year  = rtc_read(0x09);
    uint8_t regB  = rtc_read(0x0B);

    // Convert from BCD to binary if needed
    if (!(regB & 0x04)) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin(hour);
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    }

    dt->second = sec;
    dt->minute = min;
    dt->hour   = hour;
    dt->day    = day;
    dt->month  = month;
    dt->year   = 2000 + year;  // Assumes CMOS holds year since 2000
}
