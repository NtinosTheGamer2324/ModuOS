#include "moduos/kernel/events/events.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/applications/rl-clock.h"
#include <stdbool.h>

void Clock() {
    VGA_EnableScrolling(false);
    VGA_HideCursor();
    
    Event e;
    bool quit = false;
    rtc_datetime_t time, last_time;
    
    event_wait();

    // Initialize last_time to invalid to force initial redraw
    last_time.second = 255;
    
    while (!quit) {
        // Poll all events in queue
        while (event_poll(&e)) {
            if (e.type == EVENT_KEY_PRESSED && e.data.keyboard.keycode == KEY_ESCAPE) {
                quit = true;
                break;
            }
        }
        if (quit) break;
        
        // Fetch current time and date
        rtc_get_datetime(&time);
        
        // Update only on time change
        if (time.second != last_time.second || 
            time.minute != last_time.minute || 
            time.hour != last_time.hour || 
            time.day != last_time.day ||
            time.month != last_time.month ||
            time.year != last_time.year) {
            
            VGA_Clear();
            
            VGA_Write("\\clb================================================================================\\rr\n");
            VGA_Write("\\clb                                REAL-TIME CLOCK                                 \\rr\n");
            VGA_Write("\\clb================================================================================\\rr\n\n");

            // Show date in YYYY-MM-DD format
            VGA_Writef("                               Date: %04u-%02u-%02u\n", time.year, time.month, time.day);
            
            // Show 24-hour time
            VGA_Writef("                            24-Hour Time: %02u:%02u:%02u\n", time.hour, time.minute, time.second);
            
            // Show 12-hour time
            int hour_12 = time.hour % 12;
            if (hour_12 == 0) hour_12 = 12;
            const char* am_pm = (time.hour >= 12) ? " PM" : " AM";
            VGA_Writef("                          12-Hour Time: %02u:%02u:%02u %s\n", hour_12, time.minute, time.second, am_pm);

            VGA_Write("\n\\clb      Note:\\rr Time is read from the RTC and may not be perfectly accurate.\n");
            VGA_Write("\\clg                              Press ESC to quit...\\rr\n");
            
            last_time = time;
        }
        
        // Simple delay to reduce CPU usage
        for (volatile int i = 0; i < 900000; i++);
    }
    
    VGA_Clear();
    
    VGA_EnableScrolling(true);
    VGA_ShowCursor();
    event_wait();
}
