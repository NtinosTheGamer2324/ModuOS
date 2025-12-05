#include <stdint.h>
#include <stddef.h>
#include "moduos/drivers/graphics/VGA.h"
#include "moduos/kernel/io/io.h"
#include "moduos/kernel/memory/string.h"
#include <stdbool.h>
#include <stdarg.h>

#define WIDTH 80
#define HEIGHT 25
#define SCROLLBACK_LINES 100

#define VGA_ADDRESS 0xB8000
static volatile uint16_t* const VGA_BUFFER = (volatile uint16_t*)VGA_ADDRESS;
static bool vga_scrolling_enabled = true;

// Scrollback buffer
static uint16_t scrollback_buffer[SCROLLBACK_LINES * WIDTH];
static int scrollback_position = 0;
static int scrollback_count = 0;
static int scroll_offset = 0;

// Color state
static uint8_t vga_fg_color = 0x07;
static uint8_t vga_bg_color = 0x00;
static uint8_t vga_color = 0x07;

// Cursor position
static int cursor_row = 0;
static int cursor_col = 0;

// Helper functions
static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static inline void update_vga_color(void) {
    vga_color = (vga_fg_color & 0x0F) | ((vga_bg_color & 0x0F) << 4);
}

static void update_cursor(int row, int col) {
    uint16_t pos = row * WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void save_line_to_scrollback(int row) {
    int buf_row = scrollback_position % SCROLLBACK_LINES;
    for (int col = 0; col < WIDTH; col++) {
        scrollback_buffer[buf_row * WIDTH + col] = VGA_BUFFER[row * WIDTH + col];
    }
    scrollback_position++;
    if (scrollback_count < SCROLLBACK_LINES) {
        scrollback_count++;
    }
}

static void scroll(void) {
    if (!vga_scrolling_enabled) {
        cursor_row = HEIGHT - 1;
        cursor_col = 0;
        return;
    }

    save_line_to_scrollback(0);
    
    for (int row = 1; row < HEIGHT; row++) {
        for (int col = 0; col < WIDTH; col++) {
            VGA_BUFFER[(row - 1) * WIDTH + col] = VGA_BUFFER[row * WIDTH + col];
        }
    }

    for (int col = 0; col < WIDTH; col++) {
        VGA_BUFFER[(HEIGHT - 1) * WIDTH + col] = vga_entry(' ', vga_color);
    }

    cursor_row = HEIGHT - 1;
    scroll_offset = 0;
}

static uint8_t parse_color_code(const char* code, int len, bool* is_background) {
    *is_background = false;
    
    if (len == 2 && code[0] == 'c') {
        switch (code[1]) {
            case 'b': return 0x01; case 'p': return 0x05;
            case 'r': return 0x04; case 'g': return 0x02;
            case 'y': return 0x06; case 'c': return 0x03;
            case 'w': return 0x0F; case 'l': return 0x08;
            case 'k': return 0x00;
        }
    } else if (len == 3 && code[0] == 'c' && code[1] == 'l') {
        switch (code[2]) {
            case 'b': return 0x09; case 'p': return 0x0D;
            case 'r': return 0x0C; case 'g': return 0x0A;
            case 'y': return 0x0E; case 'c': return 0x0B;
            case 'w': return 0x0F;
        }
    } else if (len == 2 && code[0] == 'b') {
        *is_background = true;
        switch (code[1]) {
            case 'b': return 0x01; case 'p': return 0x05;
            case 'r': return 0x04; case 'g': return 0x02;
            case 'y': return 0x06; case 'c': return 0x03;
            case 'w': return 0x07; case 'l': return 0x08;
            case 'k': return 0x00;
        }
    } else if (len == 3 && code[0] == 'b' && code[1] == 'l') {
        *is_background = true;
        switch (code[2]) {
            case 'b': return 0x09; case 'p': return 0x0D;
            case 'r': return 0x0C; case 'g': return 0x0A;
            case 'y': return 0x0E; case 'c': return 0x0B;
            case 'w': return 0x0F;
        }
    }
    return 0xFF;
}

static int try_parse_color(const char* code, int len, uint8_t* out_color, bool* is_background) {
    uint8_t color = parse_color_code(code, len, is_background);
    if (color == 0xFF) return 0;
    *out_color = color;
    return 1;
}

static void print_padded_number(int num, bool zero_pad, int width, int base, bool is_unsigned) {
    char buffer[32];
    unsigned int n;
    bool negative = false;

    if (!is_unsigned && num < 0) {
        negative = true;
        n = (unsigned int)(-num);
    } else {
        n = (unsigned int)num;
    }

    int i = 0;
    do {
        unsigned int digit = n % base;
        buffer[i++] = (digit < 10) ? '0' + digit : 'a' + (digit - 10);
        n /= base;
    } while (n > 0);

    if (negative) buffer[i++] = '-';
    buffer[i] = '\0';

    // Reverse
    for (int j = 0; j < i / 2; j++) {
        char tmp = buffer[j];
        buffer[j] = buffer[i - j - 1];
        buffer[i - j - 1] = tmp;
    }

    int len = i;
    int pad = (width > len) ? width - len : 0;

    char temp[2] = {0};
    for (int j = 0; j < pad; j++) {
        temp[0] = zero_pad ? '0' : ' ';
        VGA_Write(temp);
    }

    VGA_Write(buffer);
}

// Public API
void VGA_Clear(void) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        VGA_BUFFER[i] = vga_entry(' ', vga_color);
    }
    cursor_row = 0;
    cursor_col = 0;
    scroll_offset = 0;
    update_cursor(cursor_row, cursor_col);
}

void VGA_WriteChar(char c) {
    if (scroll_offset > 0) {
        scroll_offset = 0;
    }
    
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= HEIGHT) scroll();
    } else if (c == '\r') {
        cursor_col = 0;
    } else {
        if (cursor_col >= WIDTH) {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= HEIGHT) {
                if (vga_scrolling_enabled)
                    scroll();
                else
                    cursor_row = HEIGHT - 1;
            }
        }

        VGA_BUFFER[cursor_row * WIDTH + cursor_col] = vga_entry(c, vga_color);
        cursor_col++;

        if (cursor_col >= WIDTH) {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= HEIGHT) scroll();
        }
    }

    update_cursor(cursor_row, cursor_col);
}

void VGA_Write(const char* str) {
    while (*str) {
        if (*str == '\\' && *(str + 1)) {
            // Backspace
            if (*(str + 1) == 'b' && (*(str + 2) == '\0' || 
                (*(str + 2) != 'b' && *(str + 2) != 'r' && *(str + 2) != 'g' && 
                 *(str + 2) != 'y' && *(str + 2) != 'c' && *(str + 2) != 'p' && 
                 *(str + 2) != 'w' && *(str + 2) != 'k' && *(str + 2) != 'l'))) {
                VGA_Backspace();
                str += 2;
                continue;
            }

            // Reset colors
            if (*(str + 1) == 'r' && *(str + 2) == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                str += 3;
                continue;
            }

            // Try 3-char color
            uint8_t col;
            bool is_bg;
            if (*(str + 3) && try_parse_color(str + 1, 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                str += 4;
                continue;
            }

            // Try 2-char color
            if (*(str + 2) && try_parse_color(str + 1, 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                str += 3;
                continue;
            }
        }
        
        VGA_WriteChar(*str++);
    }
}

void VGA_WriteN(const char *buf, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char ch = buf[i];

        if (ch == '\\' && i + 1 < count) {
            // Backspace handling
            if (buf[i+1] == 'b') {
                if (i + 2 >= count || (buf[i+2] != 'b' && buf[i+2] != 'r' && 
                    buf[i+2] != 'g' && buf[i+2] != 'y' && buf[i+2] != 'c' && 
                    buf[i+2] != 'p' && buf[i+2] != 'w' && buf[i+2] != 'k' && buf[i+2] != 'l')) {
                    VGA_Backspace();
                    i += 1;
                    continue;
                }
            }

            // Reset colors
            if (i + 2 < count && buf[i+1] == 'r' && buf[i+2] == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                i += 2;
                continue;
            }

            // Try color codes
            uint8_t col;
            bool is_bg;
            if (i + 3 < count && try_parse_color(&buf[i+1], 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                i += 3;
                continue;
            }

            if (i + 2 < count && try_parse_color(&buf[i+1], 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                i += 2;
                continue;
            }
        }

        VGA_WriteChar(ch);
    }
}

void VGA_Writef(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char temp_str[2] = {0};

    for (; *fmt != '\0'; fmt++) {
        if (*fmt == '\\' && *(fmt + 1)) {
            // Handle colors (same logic as VGA_Write)
            if (*(fmt + 1) == 'b' && (*(fmt + 2) == '\0' || 
                (*(fmt + 2) != 'b' && *(fmt + 2) != 'r' && *(fmt + 2) != 'g' && 
                 *(fmt + 2) != 'y' && *(fmt + 2) != 'c' && *(fmt + 2) != 'p' && 
                 *(fmt + 2) != 'w' && *(fmt + 2) != 'k' && *(fmt + 2) != 'l'))) {
                VGA_Backspace();
                fmt++;
                continue;
            }

            if (*(fmt + 1) == 'r' && *(fmt + 2) == 'r') {
                vga_fg_color = 0x07;
                vga_bg_color = 0x00;
                update_vga_color();
                fmt += 2;
                continue;
            }

            uint8_t col;
            bool is_bg;
            if (*(fmt + 3) && try_parse_color(fmt + 1, 3, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                fmt += 3;
                continue;
            }

            if (*(fmt + 2) && try_parse_color(fmt + 1, 2, &col, &is_bg)) {
                if (is_bg) vga_bg_color = col;
                else vga_fg_color = col;
                update_vga_color();
                fmt += 2;
                continue;
            }

            temp_str[0] = *fmt;
            VGA_Write(temp_str);
            continue;
        }

        if (*fmt == '%') {
            fmt++;
            if (*fmt == '\0') break;

            if (*fmt == '%') {
                fmt++;
                if (*fmt == '\0') {
                    VGA_Write("%");
                    break;
                }
                VGA_Write("%");
                temp_str[0] = *fmt;
                VGA_Write(temp_str);
                continue;
            }

            bool zero_pad = false;
            if (*fmt == '0') {
                zero_pad = true;
                fmt++;
            }

            int width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            if (*fmt == '\0') break;

            switch (*fmt) {
                case 'd': {
                    int num = va_arg(args, int);
                    print_padded_number(num, zero_pad, width, 10, false);
                    break;
                }
                case 'u': {
                    unsigned int num = va_arg(args, unsigned int);
                    print_padded_number((int)num, zero_pad, width, 10, true);
                    break;
                }
                case 'x': {
                    unsigned int num = va_arg(args, unsigned int);
                    VGA_Write("0x");
                    print_padded_number((int)num, zero_pad, width, 16, true);
                    break;
                }
                case 's': {
                    const char* str = va_arg(args, const char*);
                    VGA_Write(str ? str : "(null)");
                    break;
                }
                case 'c': {
                    char ch = (char)va_arg(args, int);
                    temp_str[0] = ch;
                    VGA_Write(temp_str);
                    break;
                }
                case '%': {
                    VGA_Write("%");
                    break;
                }
                default: {
                    VGA_Write("%");
                    temp_str[0] = *fmt;
                    VGA_Write(temp_str);
                    break;
                }
            }
        } else {
            temp_str[0] = *fmt;
            VGA_Write(temp_str);
        }
    }

    va_end(args);
}

void VGA_Backspace(void) {
    if (cursor_col == 0 && cursor_row == 0) return;

    if (cursor_col == 0) {
        cursor_row--;
        cursor_col = WIDTH - 1;
    } else {
        cursor_col--;
    }

    VGA_BUFFER[cursor_row * WIDTH + cursor_col] = vga_entry(' ', vga_color);
    update_cursor(cursor_row, cursor_col);
}

void VGA_ScrollUp(int lines) {
    if (scrollback_count == 0) return;
    
    scroll_offset += lines;
    if (scroll_offset > scrollback_count) {
        scroll_offset = scrollback_count;
    }
    
    for (int row = 0; row < HEIGHT; row++) {
        int source_index = scrollback_position - scroll_offset - (HEIGHT - row);
        
        if (source_index >= 0 && source_index < scrollback_position) {
            int buf_row = source_index % SCROLLBACK_LINES;
            for (int col = 0; col < WIDTH; col++) {
                VGA_BUFFER[row * WIDTH + col] = scrollback_buffer[buf_row * WIDTH + col];
            }
        } else if (source_index >= scrollback_position) {
            int current_row = source_index - scrollback_position;
            if (current_row >= HEIGHT) {
                for (int col = 0; col < WIDTH; col++) {
                    VGA_BUFFER[row * WIDTH + col] = vga_entry(' ', vga_color);
                }
            }
        }
    }
}

void VGA_ScrollDown(int lines) {
    if (scroll_offset == 0) return;
    
    scroll_offset -= lines;
    if (scroll_offset < 0) scroll_offset = 0;
    
    if (scroll_offset == 0) return;
    
    for (int row = 0; row < HEIGHT; row++) {
        int source_index = scrollback_position - scroll_offset - (HEIGHT - row);
        
        if (source_index >= 0 && source_index < scrollback_position) {
            int buf_row = source_index % SCROLLBACK_LINES;
            for (int col = 0; col < WIDTH; col++) {
                VGA_BUFFER[row * WIDTH + col] = scrollback_buffer[buf_row * WIDTH + col];
            }
        }
    }
}

void VGA_ScrollReset(void) {
    scroll_offset = 0;
}

int VGA_GetScrollOffset(void) {
    return scroll_offset;
}

void VGA_EnableScrolling(bool enable) {
    vga_scrolling_enabled = enable;
}

bool VGA_IsScrollingEnabled(void) {
    return vga_scrolling_enabled;
}

void VGA_HideCursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void VGA_ShowCursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    update_cursor(cursor_row, cursor_col);
}