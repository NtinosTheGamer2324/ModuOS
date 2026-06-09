#ifndef NETCTL_API_H
#define NETCTL_API_H

/*
 * netctl_api.h  –  Public invoke protocol for the netctl userland service
 *
 * Apps talk to netctl by:
 *   int fd = open("$/user/net", O_RDWR, 0);
 *   invoke(fd, &req, sizeof(req), &resp, sizeof(resp));
 *
 * Include this header in both the service (main.c) and any client app.
 * It has no dependency on libc.h or internal headers.
 */

#include <stdint.h>

/* ── Command codes ────────────────────────────────────────────────────── */
#define NETCTL_CMD_GET_INFO      1u  /* query MAC, IP, MTU, link state     */
#define NETCTL_CMD_SET_IP        2u  /* set static IP / mask / gateway     */
#define NETCTL_CMD_PING          3u  /* send ICMP echo request, wait reply */
#define NETCTL_CMD_UDP_SEND      4u  /* send a UDP datagram                */
#define NETCTL_CMD_ARP_LOOKUP    5u  /* resolve IP → MAC (blocking)        */
#define NETCTL_CMD_ARP_FLUSH     6u  /* clear ARP cache                    */

/* ── Status codes ─────────────────────────────────────────────────────── */
#define NETCTL_OK                0
#define NETCTL_ERR_UNKNOWN_CMD  -1
#define NETCTL_ERR_BAD_SIZE     -2
#define NETCTL_ERR_NO_LINK      -3
#define NETCTL_ERR_ARP_TIMEOUT  -4
#define NETCTL_ERR_TX_FAIL      -5
#define NETCTL_ERR_TIMEOUT      -6

/* ── Request / response structs ─────────────────────────────────────────
 * Each command has a paired _req_t and _resp_t.
 * The top-level request always starts with a uint32_t cmd field.
 * ──────────────────────────────────────────────────────────────────────*/

/* NETCTL_CMD_GET_INFO -------------------------------------------------- */
typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_GET_INFO */
} netctl_get_info_req_t;

typedef struct {
    int32_t  status;        /* NETCTL_OK or error */
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip;            /* network byte order */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t mtu;
    int32_t  link_up;       /* 1 = up, 0 = down */
} netctl_get_info_resp_t;

/* NETCTL_CMD_SET_IP ---------------------------------------------------- */
typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_SET_IP */
    uint32_t ip;            /* network byte order */
    uint32_t netmask;
    uint32_t gateway;
} netctl_set_ip_req_t;

typedef struct {
    int32_t status;
} netctl_set_ip_resp_t;

/* NETCTL_CMD_PING ------------------------------------------------------ */
typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_PING */
    uint32_t dst_ip;        /* target, network byte order */
    uint16_t id;
    uint16_t seq;
    uint32_t timeout_ms;    /* how long to wait for a reply (0 = fire & forget) */
} netctl_ping_req_t;

typedef struct {
    int32_t  status;        /* NETCTL_OK = got reply, NETCTL_ERR_TIMEOUT = no reply */
    uint32_t rtt_ms;        /* round-trip time (0 if fire & forget) */
} netctl_ping_resp_t;

/* NETCTL_CMD_UDP_SEND -------------------------------------------------- */
#define NETCTL_UDP_MAX_PAYLOAD  1024u

typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_UDP_SEND */
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t len;
    uint8_t  data[NETCTL_UDP_MAX_PAYLOAD];
} netctl_udp_send_req_t;

typedef struct {
    int32_t status;
} netctl_udp_send_resp_t;

/* NETCTL_CMD_ARP_LOOKUP ------------------------------------------------ */
typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_ARP_LOOKUP */
    uint32_t ip;
} netctl_arp_lookup_req_t;

typedef struct {
    int32_t status;
    uint8_t mac[6];
    uint8_t _pad[2];
} netctl_arp_lookup_resp_t;

/* NETCTL_CMD_ARP_FLUSH ------------------------------------------------- */
typedef struct {
    uint32_t cmd;           /* NETCTL_CMD_ARP_FLUSH */
} netctl_arp_flush_req_t;

typedef struct {
    int32_t status;
} netctl_arp_flush_resp_t;

#endif /* NETCTL_API_H */