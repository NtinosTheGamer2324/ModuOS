#ifndef MACROS_H
#define MACROS_H

#include "moduos/drivers/graphics/VGA.h"
#include "moduos/drivers/Time/RTC.h"
#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/debug.h"

/* VGA logging macros (original, no COM) */
#define LOG_INFO(text) do { \
    VGA_Write("[ \\ccINFO \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define LOG_WARN(text) do { \
    VGA_Write("[ \\cyWARN \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define LOG_ERROR(text) do { \
    VGA_Write("[ \\clrERROR \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define LOG_PANIC(text) do { \
    VGA_Write("[ \\crPANIC \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define LOG_CRIT(text) do { \
    VGA_Write("[ \\clrCRITICAL \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define LOG_OK(fmt, ...) do { \
    VGA_Write("[ \\clgOK \\rr] "); \
    VGA_Writef(fmt, ##__VA_ARGS__); \
    VGA_Write("\n"); \
} while(0)


#define LOG(text) do { \
    VGA_Write("[ \\cwLOG \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

/* COM-only logging macros (no VGA output) */
#define COM_LOG_INFO(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[INFO] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG_WARN(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[WARN] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG_ERROR(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[ERROR] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG_PANIC(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[PANIC] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG_CRIT(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[CRITICAL] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG_OK(COM_PORT, ...) do { \
    com_write_string(COM_PORT, "[OK] "); \
    com_printf(COM_PORT, __VA_ARGS__); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

#define COM_LOG(COM_PORT, text) do { \
    com_write_string(COM_PORT, "[LOG] "); \
    com_write_string(COM_PORT, text); \
    com_write_string(COM_PORT, "\n"); \
} while(0)

/* Debug-only COM logging (runtime toggle) */
#define DBG_COM_LOG(COM_PORT, text) do { \
    if (kernel_debug_get()) { \
        COM_LOG(COM_PORT, text); \
    } \
} while(0)

#define DBG_COM_PRINTF(COM_PORT, ...) do { \
    if (kernel_debug_get()) { \
        com_printf(COM_PORT, __VA_ARGS__); \
    } \
} while(0)

/* VGA-only logging macros (no COM output) */
#define VGA_LOG_INFO(text) do { \
    VGA_Write("[ \\ccINFO \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG_WARN(text) do { \
    VGA_Write("[ \\cyWARN \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG_ERROR(text) do { \
    VGA_Write("[ \\clrERROR \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG_PANIC(text) do { \
    VGA_Write("[ \\crPANIC \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG_CRIT(text) do { \
    VGA_Write("[ \\clrCRITICAL \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG_OK(text) do { \
    VGA_Write("[ \\clgOK \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define VGA_LOG(text) do { \
    VGA_Write("[ \\cwLOG \\rr] "); \
    VGA_Write(text); \
    VGA_Write("\n"); \
} while(0)

#define DEBUG_PAUSE(sec) do { \
    rtc_wait_seconds(sec); \
} while(0)

#endif