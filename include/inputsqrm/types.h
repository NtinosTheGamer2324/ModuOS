#ifndef INPUT_TYPES_H
#define INPUT_TYPES_H

/* Vectors */
typedef struct {
    int x;
    int y;
}vector2_t;

/* KEYBOARD */
typedef enum {
    KEY_UNKNOWN = 0,
    KEY_A,
    KEY_B,
    KEY_C,
    KEY_D,
    KEY_E,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_I,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_N,
    KEY_O,
    KEY_P,
    KEY_Q,
    KEY_R,
    KEY_S,
    KEY_T,
    KEY_U,
    KEY_V,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_Z,
    KEY_LBRACKET,
    KEY_RBRACKET,
    KEY_SEMICOLON,
    KEY_QUOTE,
    KEY_COMMA,
    KEY_DOT,
    KEY_GREATERTHAN,
    KEY_FORWARDSLASH,
    KEY_BACKSLASH,
    KEY_LSHIFT,
    KEY_LCTRL,
    KEY_LALT,
    KEY_SUPER,
    KEY_SPACE,
    KEY_RALT,
    KEY_RCTRL,
    KEY_RSHIFT,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_0,
    KEY_DASH_MINUS,
    KEY_EQUAL,
    KEY_BACKTICK,
    KEY_ESCAPE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_PRINT_SCREEN,
    KEY_SCROLL_LOCK,
    KEY_KEY_PAUSE_BREAK,
    KEY_INSERT,
    KEY_HOME,
    KEY_PAGEUP,
    KEY_DEL,
    KEY_END,
    KEY_PAGEDOWN,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    KEY_NUMLOCK,
    KEY_ASTERISC,
    KEY_NUM1,
    KEY_NUM2,
    KEY_NUM3,
    KEY_NUM4,
    KEY_NUM5,
    KEY_NUM6,
    KEY_NUM7,
    KEY_NUM8,
    KEY_NUM9,
    KEY_NUM0,
    KEY_PLUS,
    KEY_TAB,
    KEY_CAPSLOCK,
    KEY_APPS,
}keys_t;

/* SHARED */
typedef enum {
    STATE_PRESS,
    STATE_RELEASE
}key_states_t;

typedef struct {
    keys_t key;
    key_states_t state;
}key_event_t;

/* MOUSE */
typedef enum {
    LMB,
    RMB,
    MMB,
    MB4,
    MB5
}mouse_btn_t;

typedef struct {
    mouse_btn_t key;
    key_states_t state;
}mouse_key_event_t;

typedef enum {
    CONTROLLER_TYPE_KEYBOARD,
    CONTROLLER_TYPE_MOUSE, /* SHARED WITH TOUCHSCREEN */
    CONTROLLER_TYPE_GAMEPAD
} controller_types_t;

typedef struct {
    uint32_t id;
    const char *name;
    controller_types_t type;
}controller_t;

typedef enum {
    esuccess,
    efail,
    eperm,
    einvalinfo,
    eoutofspace,
}errc_t;



#endif