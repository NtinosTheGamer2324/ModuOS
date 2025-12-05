#include "moduos/kernel/COM/com.h"
#include "moduos/kernel/io/io.h"
#include <stddef.h>

/* Timeout counter for blocking operations */
#define COM_TIMEOUT 100000

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

int com_init(uint16_t port) {
    /* Default: 115200 baud, 8 data bits, 1 stop bit, no parity */
    return com_init_ex(port, COM_BAUD_115200, 
                       COM_DATA_8_BITS | COM_STOP_1_BIT | COM_PARITY_NONE);
}

int com_init_ex(uint16_t port, uint16_t divisor, uint8_t line_config) {
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
    
    /* Check if serial is faulty */
    if (inb(port + COM_DATA_REG) != 0xAE) {
        return -1; /* Serial port faulty */
    }
    
    /* Set to normal operation mode */
    outb(port + COM_MODEM_CTRL_REG, 0x0F);
    
    return 0;
}

int com_write_byte(uint16_t port, uint8_t data) {
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
