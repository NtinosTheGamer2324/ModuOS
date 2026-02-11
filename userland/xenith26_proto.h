#pragma once

#include <stdint.h>

/* Xenith26 protocol over $/dev/gui0.
 *
 * Transport framing is mdx_msg_hdr_t compatible (magic/type/size/src/dst).
 * This header defines payloads and message types.
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;      /* 'GUI0' */
    uint16_t type;
    uint16_t flags;
    uint32_t size;       /* total size including header */
    uint32_t src_pid;
    uint32_t dst_pid;
} x26_hdr_t;

#define X26_MAGIC 0x30495547u /* 'GUI0' */

#define X26_BROADCAST_PID 0xFFFFFFFFu

enum {
    X26_MSG_CLAIM_SERVER = 0xFFFE,

    /* Client -> server */
    X26_MSG_HELLO = 1,

    /* WM control */
    X26_MSG_REGISTER_WM = 2,
    X26_MSG_RAISE_WINDOW = 3,
    X26_MSG_MOVE_WINDOW = 4,

    /* Windowing */
    X26_MSG_CREATE_WINDOW = 10,
    X26_MSG_MAP_WINDOW = 11,
    X26_MSG_DESTROY_WINDOW = 12,
    X26_MSG_ATTACH_BUFFER = 13,
    X26_MSG_PRESENT = 14,
    X26_MSG_SET_TITLE = 15,

    /* Server -> client */
    X26_MSG_HELLO_ACK = 100,
    X26_MSG_WINDOW_CREATED = 110,
    X26_MSG_INPUT_EVENT = 120,
    X26_MSG_EXPOSE = 121,
};

typedef uint32_t x26_win_id_t;

typedef struct __attribute__((packed)) {
    uint32_t version; /* 1 */
} x26_hello_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t caps;
} x26_hello_ack_t;

typedef struct __attribute__((packed)) {
    uint16_t w;
    uint16_t h;
    uint16_t fmt; /* same as MD64API_GRP_FMT_* */
    uint16_t flags;
} x26_create_window_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
    int16_t x;
    int16_t y;
} x26_map_window_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
} x26_window_id_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
    uint16_t w;
    uint16_t h;
} x26_window_created_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
    uint32_t buf_id;
} x26_attach_buffer_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} x26_present_t;

typedef struct __attribute__((packed)) {
    x26_win_id_t id;
    int16_t x;
    int16_t y;
} x26_move_window_t;

/* Minimal input event (server->client) */
typedef struct __attribute__((packed)) {
    uint16_t type; /* EVENT_* */
    uint16_t _pad;
    int16_t dx;
    int16_t dy;
    int16_t x;
    int16_t y;
    uint16_t buttons;
    uint16_t keycode;
} x26_input_event_t;
