#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/io/io.h"
#include <stddef.h>

/* Timeout counter for blocking operations */
#define COM_TIMEOUT 100000

/* Global initialization state for COM ports */
uint8_t com_global_initialized = 0;

/* Track which ports are initialized */
static uint8_t com_port_initialized[4] = {0, 0, 0, 0};

/* Helper: Get port index from port address */
static int get_port_index(uint16_t port) {
    switch (port) {
        case COM1_PORT: return 0;
        case COM2_PORT: return 1;
        case COM3_PORT: return 2;
        case COM4_PORT: return 3;
        default: return -1;
    }
}

/* Helper: Wait for transmitter to be ready */
static int wait_tx_ready(uint16_t port) {
    uint32_t timeout = COM_TIMEOUT;
    while (timeout-- > 0) {
        if (inb(port + COM_LINE_STATUS_REG) & COM_LSR_TX_HOLDING_EMPTY) {
            return 0;
        }
    }
    return -1; /* Timeout */
}

/* Helper: Check if data is available */
static int data_ready(uint16_t port) {
    return (inb(port + COM_LINE_STATUS_REG) & COM_LSR_DATA_READY) ? 1 : 0;
}

int com_early_init(uint16_t port) {
    int port_idx = get_port_index(port);
    if (port_idx < 0) return -1;
    
    /* Minimal initialization without testing - safe for early boot */
    
    /* Disable interrupts */
    outb(port + COM_INT_ENABLE_REG, 0x00);
    
    /* Enable DLAB (set bit 7 of Line Control Register) */
    outb(port + COM_LINE_CTRL_REG, 0x80);
    
    /* Set divisor for 115200 baud */
    outb(port + COM_DIVISOR_LOW_REG, COM_BAUD_115200 & 0xFF);
    outb(port + COM_DIVISOR_HIGH_REG, (COM_BAUD_115200 >> 8) & 0xFF);
    
    /* Set line control: 8N1 (disable DLAB) */
    outb(port + COM_LINE_CTRL_REG, COM_DATA_8_BITS | COM_STOP_1_BIT | COM_PARITY_NONE);
    
    /* Enable FIFO, clear them, with 14-byte threshold */
    outb(port + COM_FIFO_CTRL_REG, 0xC7);
    
    /* Enable IRQs, set RTS/DSR (normal operation mode) */
    outb(port + COM_MODEM_CTRL_REG, 0x0F);
    
    /* Mark as initialized */
    com_port_initialized[port_idx] = 1;
    com_global_initialized = 1;
    
    return 0;
}

int com_init(uint16_t port) {
    /* Default: 115200 baud, 8 data bits, 1 stop bit, no parity */
    return com_init_ex(port, COM_BAUD_115200, 
                       COM_DATA_8_BITS | COM_STOP_1_BIT | COM_PARITY_NONE);
}

int com_init_ex(uint16_t port, uint16_t divisor, uint8_t line_config) {
    int port_idx = get_port_index(port);
    if (port_idx < 0) return -1;
    
    /* If already initialized by early_init, just do the test and return */
    if (com_port_initialized[port_idx]) {
        /* Port already initialized, just test it */
        /* Test the serial port with loopback mode */
        outb(port + COM_MODEM_CTRL_REG, 0x1E); /* Enable loopback */
        outb(port + COM_DATA_REG, 0xAE);       /* Send test byte */
        
        /* Small delay for loopback */
        for (volatile int i = 0; i < 100; i++);
        
        /* Check if serial is working */
        int test_result = 0;
        if (inb(port + COM_DATA_REG) != 0xAE) {
            test_result = -1; /* Test failed, but port still usable */
        }
        
        /* CRITICAL: Always restore normal operation mode after test */
        outb(port + COM_MODEM_CTRL_REG, 0x0F);
        
        return test_result;
    }
    
    /* Full initialization from scratch */
    
    /* Disable interrupts */
    outb(port + COM_INT_ENABLE_REG, 0x00);
    
    /* Enable DLAB (set bit 7 of Line Control Register) */
    outb(port + COM_LINE_CTRL_REG, 0x80);
    
    /* Set divisor (baud rate) */
    outb(port + COM_DIVISOR_LOW_REG, divisor & 0xFF);
    outb(port + COM_DIVISOR_HIGH_REG, (divisor >> 8) & 0xFF);
    
    /* Set line control (disable DLAB, set data/stop/parity) */
    outb(port + COM_LINE_CTRL_REG, line_config);
    
    /* Enable FIFO, clear them, with 14-byte threshold */
    outb(port + COM_FIFO_CTRL_REG, 0xC7);
    
    /* Enable IRQs, set RTS/DSR */
    outb(port + COM_MODEM_CTRL_REG, 0x0B);
    
    /* Test the serial port with loopback mode */
    outb(port + COM_MODEM_CTRL_REG, 0x1E); /* Enable loopback */
    outb(port + COM_DATA_REG, 0xAE);       /* Send test byte */
    
    /* Small delay for loopback */
    for (volatile int i = 0; i < 100; i++);
    
    /* Check if serial is faulty */
    int test_result = 0;
    if (inb(port + COM_DATA_REG) != 0xAE) {
        test_result = -1; /* Serial port test failed */
    }
    
    /* CRITICAL: Always set to normal operation mode, even if test fails */
    outb(port + COM_MODEM_CTRL_REG, 0x0F);
    
    /* Mark as initialized regardless of test result - port is usable */
    com_port_initialized[port_idx] = 1;
    com_global_initialized = 1;
    
    return test_result;
}

int com_is_initialized(uint16_t port) {
    int port_idx = get_port_index(port);
    if (port_idx < 0) return 0;
    return com_port_initialized[port_idx];
}

int com_write_byte(uint16_t port, uint8_t data) {
    int port_idx = get_port_index(port);
    
    /* If port not initialized, try early init */
    if (port_idx >= 0 && !com_port_initialized[port_idx]) {
        com_early_init(port);
    }
    
    if (wait_tx_ready(port) != 0) {
        return -1; /* Timeout */
    }
    outb(port + COM_DATA_REG, data);
    return 0;
}

int com_write_string(uint16_t port, const char* str) {
    if (str == NULL) return -1;
    
    int count = 0;
    while (*str) {
        if (com_write_byte(port, *str) != 0) {
            return -1;
        }
        str++;
        count++;
    }
    return count;
}

int com_write(uint16_t port, const uint8_t* data, uint32_t len) {
    if (data == NULL) return -1;
    
    for (uint32_t i = 0; i < len; i++) {
        if (com_write_byte(port, data[i]) != 0) {
            return i; /* Return number of bytes written before failure */
        }
    }
    return len;
}


int com_read_byte(uint16_t port, uint8_t* data) {
    if (data == NULL) return -1;
    
    if (!data_ready(port)) {
        return 0; /* No data available */
    }
    
    *data = inb(port + COM_DATA_REG);
    return 1;
}

uint8_t com_read_byte_blocking(uint16_t port) {
    while (!data_ready(port)) {
        /* Wait for data */
    }
    return inb(port + COM_DATA_REG);
}

int com_read_string(uint16_t port, char* buffer, uint32_t max_len) {
    if (buffer == NULL || max_len == 0) return -1;
    
    uint32_t i = 0;
    while (i < max_len - 1) {
        uint8_t ch = com_read_byte_blocking(port);
        
        /* Handle newline/carriage return */
        if (ch == '\n' || ch == '\r') {
            buffer[i] = '\0';
            return i;
        }
        
        /* Handle backspace */
        if (ch == '\b' || ch == 0x7F) {
            if (i > 0) {
                i--;
                /* Echo backspace for terminal feedback */
                com_write_byte(port, '\b');
                com_write_byte(port, ' ');
                com_write_byte(port, '\b');
            }
            continue;
        }
        
        /* Store character */
        buffer[i++] = ch;
        
        /* Echo character back */
        com_write_byte(port, ch);
    }
    
    buffer[i] = '\0';
    return i;
}

int com_read_string_nonblocking(uint16_t port, char* buffer, uint32_t max_len) {
    if (buffer == NULL || max_len == 0) return -1;
    
    uint32_t i = 0;
    uint8_t ch;
    
    while (i < max_len - 1) {
        /* Try to read a byte */
        int result = com_read_byte(port, &ch);
        
        if (result == 0) {
            /* No data available */
            if (i == 0) {
                return 0; /* No data read at all */
            }
            /* Partial data read but no newline yet */
            buffer[i] = '\0';
            return 0;
        }
        
        if (result < 0) {
            return -1; /* Error */
        }
        
        /* Handle newline/carriage return */
        if (ch == '\n' || ch == '\r') {
            buffer[i] = '\0';
            return i;
        }
        
        /* Handle backspace */
        if (ch == '\b' || ch == 0x7F) {
            if (i > 0) {
                i--;
                /* Echo backspace for terminal feedback */
                com_write_byte(port, '\b');
                com_write_byte(port, ' ');
                com_write_byte(port, '\b');
            }
            continue;
        }
        
        /* Store character */
        buffer[i++] = ch;
        
        /* Echo character back */
        com_write_byte(port, ch);
    }
    
    buffer[i] = '\0';
    return i;
}

int com_data_available(uint16_t port) {
    return data_ready(port);
}

int com_tx_ready(uint16_t port) {
    return (inb(port + COM_LINE_STATUS_REG) & COM_LSR_TX_HOLDING_EMPTY) ? 1 : 0;
}

uint8_t com_get_line_status(uint16_t port) {
    return inb(port + COM_LINE_STATUS_REG);
}

int com_test(uint16_t port) {
    /* Enable loopback mode */
    outb(port + COM_MODEM_CTRL_REG, 0x1E);
    
    /* Test with various patterns */
    uint8_t test_patterns[] = {0xAE, 0x55, 0xAA, 0xFF, 0x00};
    
    for (int i = 0; i < 5; i++) {
        outb(port + COM_DATA_REG, test_patterns[i]);
        
        /* Small delay */
        for (volatile int j = 0; j < 1000; j++);
        
        if (inb(port + COM_DATA_REG) != test_patterns[i]) {
            /* Restore normal mode before returning */
            outb(port + COM_MODEM_CTRL_REG, 0x0F);
            return -1; /* Test failed */
        }
    }
    
    /* Restore normal mode */
    outb(port + COM_MODEM_CTRL_REG, 0x0F);
    return 0; /* Test passed */
}

int com_write_hex(uint16_t port, uint8_t value) {
    const char hex_chars[] = "0123456789ABCDEF";
    char hex_str[2];

    hex_str[0] = hex_chars[(value >> 4) & 0x0F];  // High nibble
    hex_str[1] = hex_chars[value & 0x0F];         // Low nibble

    for (int i = 0; i < 2; i++) {
        if (com_write_byte(port, hex_str[i]) != 0) {
            return -1;  // Failed to write
        }
    }

    return 2;  // Two characters written
}

int com_write_hex64(uint16_t port, uint64_t value) {
    // Print as 16 hex chars, high nibble first.
    const char hex_chars[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (uint8_t)((value >> (i * 4)) & 0x0Fu);
        if (com_write_byte(port, hex_chars[nib]) != 0) return -1;
    }
    return 16;
}

/* Helper function to convert integer to string */
static int int_to_string(int value, char *buf, int base) {
    if (base < 2 || base > 16) return 0;
    
    int i = 0;
    int is_negative = 0;
    
    if (value < 0 && base == 10) {
        is_negative = 1;
        value = -value;
    }
    
    if (value == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return i;
    }
    
    char temp[32];
    int temp_i = 0;
    
    while (value != 0) {
        int rem = value % base;
        temp[temp_i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        value = value / base;
    }
    
    if (is_negative) {
        buf[i++] = '-';
    }
    
    while (temp_i > 0) {
        buf[i++] = temp[--temp_i];
    }
    
    buf[i] = '\0';
    return i;
}

/* Helper function to convert unsigned integer to string */
static int uint_to_string(unsigned int value, char *buf, int base) {
    if (base < 2 || base > 16) return 0;
    
    int i = 0;
    
    if (value == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return i;
    }
    
    char temp[32];
    int temp_i = 0;
    
    while (value != 0) {
        int rem = value % base;
        temp[temp_i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        value = value / base;
    }
    
    while (temp_i > 0) {
        buf[i++] = temp[--temp_i];
    }
    
    buf[i] = '\0';
    return i;
}

int com_printf(uint16_t port, const char* format, ...) {
    if (format == NULL) return -1;
    
    __builtin_va_list args;
    __builtin_va_start(args, format);
    
    int count = 0;
    char buffer[32];
    
    while (*format) {
        if (*format == '%' && *(format + 1)) {
            format++;
            switch (*format) {
                case 'd':
                case 'i': {
                    int val = __builtin_va_arg(args, int);
                    int_to_string(val, buffer, 10);
                    com_write_string(port, buffer);
                    count += int_to_string(val, buffer, 10);
                    break;
                }
                case 'u': {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    uint_to_string(val, buffer, 10);
                    com_write_string(port, buffer);
                    count += uint_to_string(val, buffer, 10);
                    break;
                }
                case 'x': {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    uint_to_string(val, buffer, 16);
                    com_write_string(port, buffer);
                    count += uint_to_string(val, buffer, 16);
                    break;
                }
                case 'X': {
                    unsigned int val = __builtin_va_arg(args, unsigned int);
                    int len = uint_to_string(val, buffer, 16);
                    for (int i = 0; i < len; i++) {
                        if (buffer[i] >= 'a' && buffer[i] <= 'f') {
                            buffer[i] = buffer[i] - 'a' + 'A';
                        }
                    }
                    com_write_string(port, buffer);
                    count += len;
                    break;
                }
                case 's': {
                    char *str = __builtin_va_arg(args, char*);
                    if (str) {
                        int len = com_write_string(port, str);
                        count += (len > 0) ? len : 0;
                    } else {
                        com_write_string(port, "(null)");
                        count += 6;
                    }
                    break;
                }
                case 'c': {
                    char ch = (char)__builtin_va_arg(args, int);
                    com_write_byte(port, ch);
                    count++;
                    break;
                }
                case '%': {
                    com_write_byte(port, '%');
                    count++;
                    break;
                }
                case '0': {
                    // Handle format like %02x, %04x
                    int width = 0;
                    format++;
                    while (*format >= '0' && *format <= '9') {
                        width = width * 10 + (*format - '0');
                        format++;
                    }
                    if (*format == 'x' || *format == 'X') {
                        unsigned int val = __builtin_va_arg(args, unsigned int);
                        int len = uint_to_string(val, buffer, 16);
                        // Pad with zeros
                        for (int i = len; i < width; i++) {
                            com_write_byte(port, '0');
                            count++;
                        }
                        if (*format == 'X') {
                            for (int i = 0; i < len; i++) {
                                if (buffer[i] >= 'a' && buffer[i] <= 'f') {
                                    buffer[i] = buffer[i] - 'a' + 'A';
                                }
                            }
                        }
                        com_write_string(port, buffer);
                        count += len;
                    }
                    break;
                }
                default:
                    com_write_byte(port, '%');
                    com_write_byte(port, *format);
                    count += 2;
                    break;
            }
        } else {
            com_write_byte(port, *format);
            count++;
        }
        format++;
    }
    
    __builtin_va_end(args);
    return count;
}
