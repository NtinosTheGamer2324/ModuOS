#pragma once
#include "libc.h"
#include "netman/protocol/ip/ipv4.h"

/* ── DHCP UDP ports ──────────────────────────────────────────────────────── */
#define DHCP_CLIENT_PORT    68
#define DHCP_SERVER_PORT    67

/* ── BOOTP / DHCP op codes ───────────────────────────────────────────────── */
#define DHCP_OP_REQUEST     1
#define DHCP_OP_REPLY       2

/* ── DHCP message types (option 53) ──────────────────────────────────────── */
#define DHCP_MSG_DISCOVER   1
#define DHCP_MSG_OFFER      2
#define DHCP_MSG_REQUEST    3
#define DHCP_MSG_DECLINE    4
#define DHCP_MSG_ACK        5
#define DHCP_MSG_NAK        6
#define DHCP_MSG_RELEASE    7
#define DHCP_MSG_INFORM     8

/* ── DHCP option codes ───────────────────────────────────────────────────── */
#define DHCP_OPT_SUBNET         1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_REQUEST  55
#define DHCP_OPT_END            255

/* ── BOOTP magic cookie ───────────────────────────────────────────────────── */
#define DHCP_MAGIC_COOKIE   0x63825363u

/* ── Wire-format DHCP packet ─────────────────────────────────────────────── */
#define DHCP_OPTIONS_LEN    308
typedef struct __attribute__((packed)) {
    uint8_t  op;            /* DHCP_OP_* */
    uint8_t  htype;         /* 1 = Ethernet */
    uint8_t  hlen;          /* 6 */
    uint8_t  hops;
    uint32_t xid;           /* transaction ID */
    uint16_t secs;
    uint16_t flags;         /* 0x8000 = broadcast */
    uint8_t  ciaddr[4];     /* client IP (if known) */
    uint8_t  yiaddr[4];     /* your (offered) IP */
    uint8_t  siaddr[4];     /* server IP */
    uint8_t  giaddr[4];     /* relay agent IP */
    uint8_t  chaddr[16];    /* client hardware address */
    uint8_t  sname[64];     /* server name (unused) */
    uint8_t  file[128];     /* boot file name (unused) */
    uint32_t magic;         /* DHCP_MAGIC_COOKIE */
    uint8_t  options[DHCP_OPTIONS_LEN];
} dhcp_pkt_t;

/* ── UDP header (minimal, for DHCP encapsulation) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

/* ── Lease information filled after successful DHCP exchange ─────────────── */
typedef struct {
    uint8_t  ip[4];
    uint8_t  subnet[4];
    uint8_t  router[4];
    uint8_t  dns[4];
    uint32_t lease_time;     /* seconds */
    uint8_t  server_id[4];
    int      valid;          /* 1 = lease is active */
} dhcp_lease_t;

/* ── DHCP client state machine ───────────────────────────────────────────── */
typedef enum {
    DHCP_STATE_IDLE,
    DHCP_STATE_SELECTING,    /* sent DISCOVER, waiting for OFFER */
    DHCP_STATE_REQUESTING,   /* sent REQUEST, waiting for ACK */
    DHCP_STATE_BOUND,        /* lease active */
    DHCP_STATE_RENEWING,
    DHCP_STATE_FAILED,
} dhcp_state_t;

/* ── DHCP context ────────────────────────────────────────────────────────── */
typedef struct {
    int          net_fd;
    uint8_t      mac[6];
    ipv4_ctx_t  *ipv4;
    uint32_t     xid;           /* current transaction ID */
    dhcp_state_t state;
    dhcp_lease_t lease;
    uint64_t     timeout_ms;    /* deadline for current state */
} dhcp_ctx_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialise DHCP context. ipv4 may be NULL before lease is obtained. */
void dhcp_init(dhcp_ctx_t *ctx, int net_fd,
               const uint8_t mac[6], ipv4_ctx_t *ipv4);

/*
 * Run a blocking DORA exchange (Discover → Offer → Request → Ack).
 * Fills ctx->lease on success and configures *ipv4 with the leased address.
 * Returns 0 on success, <0 on failure/timeout.
 * timeout_ms: maximum time to wait (0 → use a sensible default of 10 s).
 */
int dhcp_request(dhcp_ctx_t *ctx, uint64_t timeout_ms);

/* Release the current lease (sends DHCP RELEASE). */
int dhcp_release(dhcp_ctx_t *ctx);

/* Print lease info to stdout. */
void dhcp_lease_print(const dhcp_lease_t *lease);