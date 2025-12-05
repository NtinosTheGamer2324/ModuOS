#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;  // Full 4-digit year
} rtc_datetime_t;

// Read current time from RTC
void rtc_get_time(uint8_t* hour, uint8_t* min, uint8_t* sec);

// Read full date and time from RTC
void rtc_get_datetime(rtc_datetime_t* dt);

// Wait approximately N seconds using RTC polling
void rtc_wait_seconds(uint8_t seconds);

#endif // RTC_H
