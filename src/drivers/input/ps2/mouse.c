#include "moduos/drivers/input/ps2/mouse.h"
#include "moduos/kernel/io/io.h"
#include "moduos/arch/AMD64/interrupts/irq.h"
#include "moduos/arch/AMD64/interrupts/pic.h"
#include "moduos/kernel/events/events.h"
#include "moduos/fs/devfs.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

#define PS2_STATUS_OUTPUT_BUFFER (1 << 0)
#define PS2_STATUS_MOUSE_DATA    (1 << 5)

#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_ENABLE_PORT2  0xA8
#define PS2_CMD_WRITE_PORT2   0xD4

static volatile int32_t g_mouse_x = 0;
static volatile int32_t g_mouse_y = 0;
static volatile int32_t g_mouse_wheel = 0;
static volatile uint8_t g_mouse_buttons = 0;

// 3-byte PS/2 packet
static uint8_t pkt[4];
static int pkt_i = 0;
static int has_wheel = 0;

static int ps2_wait_input(void) {
    // wait until input buffer empty
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS_PORT) & (1 << 1)) == 0) return 0;
    }
    return -1;
}

static int ps2_wait_output(uint8_t *out) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_BUFFER) {
            uint8_t d = inb(PS2_DATA_PORT);
            if (out) *out = d;
            return 0;
        }
    }
    return -1;
}

static int ps2_mouse_send(uint8_t cmd) {
    uint8_t ack;
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_PORT2);
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_DATA_PORT, cmd);
    if (ps2_wait_output(&ack) != 0) return -1;
    return (ack == 0xFA) ? 0 : -1;
}

// Try to enable IntelliMouse wheel: sequence 200,100,80 then GETID should return 3
static void ps2_mouse_try_enable_wheel(void) {
    uint8_t id = 0;
    (void)ps2_mouse_send(0xF3); (void)ps2_mouse_send(200);
    (void)ps2_mouse_send(0xF3); (void)ps2_mouse_send(100);
    (void)ps2_mouse_send(0xF3); (void)ps2_mouse_send(80);

    if (ps2_mouse_send(0xF2) == 0) {
        // after F2, device returns ID byte
        if (ps2_wait_output(&id) == 0) {
            if (id == 3) has_wheel = 1;
        }
    }
}

int ps2_mouse_init(void) {
    /*
     * IMPORTANT:
     *  - Mask IRQ12 while we program the controller/device, otherwise an IRQ can
     *    arrive before our handler is installed.
     *  - If IRQ12 fires with no handler, irq_dispatch() will EOI but will NOT
     *    drain the PS/2 output buffer, resulting in an interrupt storm.
     */
    pic_mask_irq(12);

    /* Install handler first (safe even while masked). */
    irq_install_handler(12, ps2_mouse_irq_handler);

    // Enable port2
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_CMD_PORT, PS2_CMD_ENABLE_PORT2);

    // Enable IRQ12 in controller config byte
    uint8_t config = 0;
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_CMD_PORT, PS2_CMD_READ_CONFIG);
    if (ps2_wait_output(&config) != 0) config = 0;

    config |= (1 << 1); // IRQ12 enable
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_CMD_PORT, PS2_CMD_WRITE_CONFIG);
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_DATA_PORT, config);

    // Flush any pending bytes (mouse/keyboard) before enabling data reporting
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_BUFFER) {
        (void)inb(PS2_DATA_PORT);
    }

    // Reset mouse
    (void)ps2_mouse_send(0xFF);
    uint8_t resp;
    (void)ps2_wait_output(&resp); // 0xAA self-test maybe

    // Enable wheel if possible
    ps2_mouse_try_enable_wheel();

    // Enable data reporting
    if (ps2_mouse_send(0xF4) != 0) {
        // still continue; some emulators behave oddly
    }

    // Flush again in case device produced extra bytes during init
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_BUFFER) {
        (void)inb(PS2_DATA_PORT);
    }

    pic_unmask_irq(12);
    return 0;
}

static void mouse_push_move(int8_t dx, int8_t dy) {
    if (dx == 0 && dy == 0) return;
    Event e;
    e.type = EVENT_MOUSE_MOVE;
    e.timestamp = 0;
    e.data.mouse.delta_x = dx;
    e.data.mouse.delta_y = dy;
    // maintain absolute in kernel
    e.data.mouse.x = (int16_t)g_mouse_x;
    e.data.mouse.y = (int16_t)g_mouse_y;
    e.data.mouse.buttons = g_mouse_buttons;
    event_push(&e);
    devfs_input_push_event(&e);
}

static void mouse_push_buttons(uint8_t new_buttons) {
    if (new_buttons == g_mouse_buttons) return;
    g_mouse_buttons = new_buttons;
    Event e;
    e.type = EVENT_MOUSE_BUTTON;
    e.timestamp = 0;
    e.data.mouse.delta_x = 0;
    e.data.mouse.delta_y = 0;
    e.data.mouse.x = (int16_t)g_mouse_x;
    e.data.mouse.y = (int16_t)g_mouse_y;
    e.data.mouse.buttons = g_mouse_buttons;
    event_push(&e);
    devfs_input_push_event(&e);
}

void ps2_mouse_irq_handler(void) {
    /*
     * Drain all pending mouse bytes from the controller output buffer.
     *
     * If we only read a single byte and then EOI, the PS/2 controller may keep
     * IRQ12 asserted (level-like behaviour) because bytes remain in the output
     * buffer, causing an interrupt storm.
     */
    for (int safety = 0; safety < 32; safety++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_BUFFER)) {
            break;
        }

        /* Only consume mouse bytes here; if we see keyboard data, leave it for
         * IRQ1 (but do consume a stray byte if it came through on IRQ12).
         */
        if (!(status & PS2_STATUS_MOUSE_DATA)) {
            (void)inb(PS2_DATA_PORT);
            continue;
        }

        uint8_t data = inb(PS2_DATA_PORT);

        // Packet sync: first byte must have bit3 set
        if (pkt_i == 0 && (data & 0x08) == 0) {
            continue;
        }

        pkt[pkt_i++] = data;
        int need = has_wheel ? 4 : 3;
        if (pkt_i < need) {
            continue;
        }

        pkt_i = 0;

        uint8_t b0 = pkt[0];
        int8_t dx = (int8_t)pkt[1];
        int8_t dy = (int8_t)pkt[2];

        // Y is typically negative when moving up; convert to screen coords (up => -dy)
        g_mouse_x += dx;
        g_mouse_y -= dy;

        uint8_t buttons = 0;
        if (b0 & 0x01) buttons |= 1; // L
        if (b0 & 0x02) buttons |= 2; // R
        if (b0 & 0x04) buttons |= 4; // M

        if (buttons != g_mouse_buttons) {
            mouse_push_buttons(buttons);
        }

        if (dx != 0 || dy != 0) {
            mouse_push_move(dx, (int8_t)-dy);
        }

        if (has_wheel) {
            int8_t wz = (int8_t)pkt[3];
            if (wz) {
                g_mouse_wheel += wz;
                // Emit a generic event to wake readers (optional)
                Event e;
                e.type = EVENT_MOUSE_MOVE;
                e.timestamp = 0;
                e.data.mouse.delta_x = 0;
                e.data.mouse.delta_y = 0;
                e.data.mouse.x = (int16_t)g_mouse_x;
                e.data.mouse.y = (int16_t)g_mouse_y;
                e.data.mouse.buttons = g_mouse_buttons;
                event_push(&e);
                devfs_input_push_event(&e);
            }
        }
    }

    pic_send_eoi(12);
}

// Expose state to devfs (implemented in devfs.c)
int32_t devfs_mouse_get_x(void) { return g_mouse_x; }
int32_t devfs_mouse_get_y(void) { return g_mouse_y; }
uint8_t devfs_mouse_get_buttons(void) { return g_mouse_buttons; }
int32_t devfs_mouse_take_wheel(void) { int32_t w = g_mouse_wheel; g_mouse_wheel = 0; return w; }
