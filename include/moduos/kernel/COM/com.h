#ifndef COM_H
#define COM_H

#include <stdint.h>

/* COM port base addresses */
#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

/* COM port register offsets */
#define COM_DATA_REG        0  /* Data register (R/W) */
#define COM_INT_ENABLE_REG  1  /* Interrupt Enable Register */
#define COM_FIFO_CTRL_REG   2  /* FIFO Control Register (W) */
#define COM_LINE_CTRL_REG   3  /* Line Control Register */
#define COM_MODEM_CTRL_REG  4  /* Modem Control Register */
#define COM_LINE_STATUS_REG 5  /* Line Status Register (R) */
#define COM_MODEM_STATUS_REG 6 /* Modem Status Register (R) */
#define COM_SCRATCH_REG     7  /* Scratch Register */

/* When DLAB=1 (Divisor Latch Access Bit in Line Control Register) */
#define COM_DIVISOR_LOW_REG  0  /* Divisor Latch Low Byte */
#define COM_DIVISOR_HIGH_REG 1  /* Divisor Latch High Byte */

/* Line Status Register bits */
#define COM_LSR_DATA_READY          (1 << 0)
#define COM_LSR_OVERRUN_ERROR       (1 << 1)
#define COM_LSR_PARITY_ERROR        (1 << 2)
#define COM_LSR_FRAMING_ERROR       (1 << 3)
#define COM_LSR_BREAK_INDICATOR     (1 << 4)
#define COM_LSR_TX_HOLDING_EMPTY    (1 << 5)
#define COM_LSR_TX_EMPTY            (1 << 6)
#define COM_LSR_IMPENDING_ERROR     (1 << 7)

/* Baud rates (divisor values for 115200 base) */
#define COM_BAUD_115200  1
#define COM_BAUD_57600   2
#define COM_BAUD_38400   3
#define COM_BAUD_19200   6
#define COM_BAUD_9600    12
#define COM_BAUD_4800    24
#define COM_BAUD_2400    48
#define COM_BAUD_1200    96

/* Data bits */
#define COM_DATA_5_BITS  0x00
#define COM_DATA_6_BITS  0x01
#define COM_DATA_7_BITS  0x02
#define COM_DATA_8_BITS  0x03

/* Stop bits */
#define COM_STOP_1_BIT   0x00
#define COM_STOP_2_BITS  0x04

/* Parity */
#define COM_PARITY_NONE  0x00
#define COM_PARITY_ODD   0x08
#define COM_PARITY_EVEN  0x18
#define COM_PARITY_MARK  0x28
#define COM_PARITY_SPACE 0x38

/* COM port structure */
typedef struct {
    uint16_t port;
    uint8_t initialized;
} com_port_t;

/* Function declarations */

/**
 * Initialize a COM port with default settings (115200 baud, 8N1)
 * @param port COM port base address (COM1_PORT, COM2_PORT, etc.)
 * @return 0 on success, -1 on failure
 */
int com_init(uint16_t port);

/**
 * Initialize a COM port with custom settings
 * @param port COM port base address
 * @param divisor Baud rate divisor (use COM_BAUD_* constants)
 * @param line_config Line configuration (data bits | stop bits | parity)
 * @return 0 on success, -1 on failure
 */
int com_init_ex(uint16_t port, uint16_t divisor, uint8_t line_config);

/**
 * Write a single byte to a COM port
 * @param port COM port base address
 * @param data Byte to send
 * @return 0 on success, -1 on failure
 */
int com_write_byte(uint16_t port, uint8_t data);

/**
 * Write a string to a COM port
 * @param port COM port base address
 * @param str Null-terminated string to send
 * @return Number of bytes written, -1 on failure
 */
int com_write_string(uint16_t port, const char* str);

/**
 * Write a buffer to a COM port
 * @param port COM port base address
 * @param data Buffer to send
 * @param len Length of buffer
 * @return Number of bytes written, -1 on failure
 */
int com_write(uint16_t port, const uint8_t* data, uint32_t len);

/**
 * Read a single byte from a COM port (non-blocking)
 * @param port COM port base address
 * @param data Pointer to store received byte
 * @return 1 if data received, 0 if no data, -1 on failure
 */
int com_read_byte(uint16_t port, uint8_t* data);

/**
 * Read a byte from a COM port (blocking)
 * @param port COM port base address
 * @return Received byte
 */
uint8_t com_read_byte_blocking(uint16_t port);

/**
 * Read a string from a COM port until newline or max length (blocking)
 * @param port COM port base address
 * @param buffer Buffer to store string
 * @param max_len Maximum length to read (including null terminator)
 * @return Number of bytes read (excluding null terminator), -1 on failure
 */
int com_read_string(uint16_t port, char* buffer, uint32_t max_len);

/**
 * Read a string from a COM port until newline or max length (non-blocking)
 * @param port COM port base address
 * @param buffer Buffer to store string
 * @param max_len Maximum length to read (including null terminator)
 * @return Number of bytes read (excluding null terminator), 0 if no complete line, -1 on failure
 */
int com_read_string_nonblocking(uint16_t port, char* buffer, uint32_t max_len);

/**
 * Check if data is available to read
 * @param port COM port base address
 * @return 1 if data available, 0 otherwise
 */
int com_data_available(uint16_t port);

/**
 * Check if transmitter is ready
 * @param port COM port base address
 * @return 1 if ready, 0 otherwise
 */
int com_tx_ready(uint16_t port);

/**
 * Get line status
 * @param port COM port base address
 * @return Line status register value
 */
uint8_t com_get_line_status(uint16_t port);

/**
 * Test if COM port is working (loopback test)
 * @param port COM port base address
 * @return 0 if working, -1 on failure
 */
int com_test(uint16_t port);

/**
 * Write a byte or value as hexadecimal string to COM port
 * @param port COM port base address
 * @param value Value to write
 * @return Number of characters written, -1 on failure
 */
int com_write_hex(uint16_t port, uint8_t value);


#endif /* COM_H */