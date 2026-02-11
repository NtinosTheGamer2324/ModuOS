#pragma once

#include <stdint.h>

/* Shared protocol for $/dev/gui0.
 *
 * This is intentionally tiny and versionable; it will evolve into an X11-like
 * display server protocol.
 */

typedef struct __attribute__((packed)) {
    uint32_t magic;      /* 'GUI0' */
    uint16_t type;
    uint16_t flags;
    uint32_t size;       /* total size including header */
    uint32_t src_pid;    /* filled by kernel for client->server, server for server->client */
    uint32_t dst_pid;    /* server->client target pid, 0 for client->server */
} mdx_msg_hdr_t;

#define MDX_GUI_MAGIC 0x30495547u /* 'GUI0' */

/* Message types (start small; expand later). */
enum {
    MDX_MSG_HELLO = 1,
    MDX_MSG_HELLO_ACK = 2,
    MDX_MSG_PING = 3,
    MDX_MSG_PONG = 4,

    /* Control: claim server role on $/dev/gui0.
     * Header-only message; payload_len=0.
     */
    MDX_MSG_CLAIM_SERVER = 0xFFFE,
};

typedef struct __attribute__((packed)) {
    uint32_t version;
} mdx_hello_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t server_caps;
} mdx_hello_ack_t;
