#include "moduos/kernel/sqrm.h"
#include "inputsqrm/contract.h"
#include <stdbool.h>

SQRM_DEFINE_MODULE(SQRM_TYPE_GENERIC, "inputmanager");

/* Defines */
#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define MAX_CONTROLLERS 32

/* Globals */
sqrm_kernel_api_t *g_api = NULL;
controller_t g_controllers[MAX_CONTROLLERS];
int g_nextavail = 0;

/* Helpers */
static void kprint(const char *string) {
    g_api->com_write_string(COM1_PORT, "[INPTM] ");
    g_api->com_write_string(COM1_PORT, string);
}

static void krprint(const char *string) {
    g_api->com_write_string(COM1_PORT, string);
}

/* Raw write to COM2, no prefix. Used to dump every key event we receive
 * for debugging, separately from the normal COM1 log. */
static void com2_print(const char *string) {
    g_api->com_write_string(COM2_PORT, string);
}

/* String Helpers */
static int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static const char *itoa(int value, char *str, int base) {
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *ptr = str, *ptr1 = str, tmp_char;
    int sign = 0;

    // Handle negative sign for base 10
    if (value < 0 && base == 10) {
        sign = 1;
    }

    // Use unsigned int for safe handling of INT_MIN
    unsigned int uvalue = (sign) ? -value : value;

    do {
        int remainder = uvalue % base;
        *ptr++ = digits[remainder];
        uvalue /= base;
    } while (uvalue);

    if (sign) {
        *ptr++ = '-';
    }

    *ptr-- = '\0';

    // Reverse the string
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }

    return str;
}

static int register_controller(controller_types_t _type, const char *_name) {
    /* Check for space BEFORE writing, so we never write past the array. */
    if (g_nextavail >= MAX_CONTROLLERS) {
        return -eoutofspace;
    }

    int id = g_nextavail;

    /* Set info */
    g_controllers[id].id = id;
    g_controllers[id].name = _name;
    g_controllers[id].type = _type;

    /* Increase the next available counter */
    g_nextavail++;

    /* Return the ID of the controller we just registered. */
    return id;
}

/* Shared guard used by every push_* entry point. A controller is only
 * "live" if it's in range AND has actually been registered (name != NULL
 * survives a remove_controller(), which zeroes the slot). Type is checked
 * too so a mouse id can't inject key events and vice versa -- this can be
 * called from IRQ context so it must stay cheap and non-blocking. */
static bool controller_is_valid(int id, controller_types_t expected_type) {
    if (id < 0 || id >= MAX_CONTROLLERS) {
        return false;
    }
    if (g_controllers[id].name == NULL) {
        return false;
    }
    if (g_controllers[id].type != expected_type) {
        return false;
    }
    return true;
}

static int remove_controller (int id, const char *name) {
    controller_t nall = {0};

    /* Bounds check FIRST -- an out-of-range id must never reach the array. */
    if (id < 0 || id >= MAX_CONTROLLERS) {
        kprint("Controller with ID: ");
        char idbuf[32];
        itoa(id, idbuf, 10);
        krprint(idbuf);
        krprint(" is out of range\n");
        return -einvalinfo;
    }

    /* Compare the actual string contents, not the pointer values -- two
     * different pointers can (and usually will) hold the same name. */
    if (g_controllers[id].name == NULL || strcmp(name, g_controllers[id].name) != 0) {
        kprint("Names do not match to remove controller.\n");
        return -einvalinfo;
    }

    /* Remove that controller */
    g_controllers[id] = nall;

    return esuccess;
}

/* Event queue */
/* Single unified event queue, tagged by kind. Kept as a fixed-size ring
 * buffer with power-of-two length so producers (called from IRQ context,
 * e.g. ps2_irq -> push_key_event) never allocate or block. Overflow drops
 * the oldest unread event rather than the new one, so a slow/absent
 * consumer can't wedge a driver's interrupt handler. */
#define EVENT_QUEUE_SIZE 256 /* must be power of two */

typedef enum {
    INM_EVENT_KEY,
    INM_EVENT_MOUSE_COORD,
    INM_EVENT_MOUSE_CLICK,
} inm_event_kind_t;

typedef struct {
    inm_event_kind_t kind;
    int controllerid;
    union {
        key_event_t key;
        vector2_t coords;
        mouse_key_event_t click;
    } data;
} inm_event_t;

static inm_event_t g_event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t g_queue_head = 0; /* next slot to write */
static volatile uint32_t g_queue_tail = 0; /* next slot to read */

static void queue_push(const inm_event_t *ev) {
    uint32_t next = (g_queue_head + 1) & (EVENT_QUEUE_SIZE - 1);

    if (next == g_queue_tail) {
        /* Full: drop the oldest event to make room for this one. */
        g_queue_tail = (g_queue_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    }

    g_event_queue[g_queue_head] = *ev;
    g_queue_head = next;
}

/* Pops the oldest queued event. Returns esuccess with *out filled in, or
 * efail if the queue is empty. This is what devfs (inm/kbd0, inm/event0,
 * etc.) will call from read() once that layer exists. */
static int pop_event(inm_event_t *out) {
    if (g_queue_tail == g_queue_head) {
        return efail; /* empty */
    }
    *out = g_event_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    return esuccess;
}

/* Keyboard API */
static void push_key_event(int controllerid, key_event_t key) {
    /* Dump every key event we receive to COM2, regardless of whether it
     * ends up accepted or dropped -- this is a raw debug trace, separate
     * from the COM1 module log. */
    char idbuf[32];
    com2_print("[INPTM-KEY] key=");
    itoa((int)key.key, idbuf, 10);
    com2_print(idbuf);
    com2_print(" state=");
    com2_print(key.state == STATE_PRESS ? "PRESS" : "RELEASE");
    com2_print(" controller=");
    itoa(controllerid, idbuf, 10);
    com2_print(idbuf);

    if (!controller_is_valid(controllerid, CONTROLLER_TYPE_KEYBOARD)) {
        com2_print(" [DROPPED: invalid controller]\n");
        return;
    }
    com2_print("\n");

    inm_event_t ev;
    ev.kind = INM_EVENT_KEY;
    ev.controllerid = controllerid;
    ev.data.key = key;
    queue_push(&ev);
}

/* Mouse API */
static void push_mouse_coord_event(int controllerid, vector2_t coords) {
    if (!controller_is_valid(controllerid, CONTROLLER_TYPE_MOUSE)) {
        return;
    }

    inm_event_t ev;
    ev.kind = INM_EVENT_MOUSE_COORD;
    ev.controllerid = controllerid;
    ev.data.coords = coords;
    queue_push(&ev);
}

static void push_mouse_click_event(int controllerid, mouse_key_event_t event) {
    if (!controller_is_valid(controllerid, CONTROLLER_TYPE_MOUSE)) {
        return;
    }

    inm_event_t ev;
    ev.kind = INM_EVENT_MOUSE_CLICK;
    ev.controllerid = controllerid;
    ev.data.click = event;
    queue_push(&ev);
}

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    g_api = api;

    /* Register the API */
    kprint("Registering API.\n");
    static input_api_t input_api;
    input_api.register_controller = register_controller;
    input_api.remove_controller = remove_controller;
    input_api.push_key_event = push_key_event;
    input_api.push_mouse_coord_event = push_mouse_coord_event;
    input_api.push_mouse_click_event = push_mouse_click_event;
    api->sqrm_service_register("inputmanager", &input_api, sizeof(input_api));

    return 0;
}