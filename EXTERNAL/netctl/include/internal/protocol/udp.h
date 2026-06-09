#ifndef NETCTL_PROTOCOL_UDP_H
#define NETCTL_PROTOCOL_UDP_H

/*
 * udp.h  –  UDP (RFC 768) definitions
 *
 * Supports:
 *   - Sending UDP datagrams (udp_send)
 *   - Port-based demux via udp_port_handler_t registration
 *
 * To receive on a port: fill a udp_port_handler_t and call
 * udp_port_register().  No sockets, no dynamic allocation — just a
 * flat callback table.
 */

#include <stdint.h>
#include <stddef.h>
#include "ip.h"

/* ── UDP Header ───────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;     /* header + data, network byte order */
    uint16_t checksum;   /* 0 = disabled (we skip pseudo-header for now) */
} udp_hdr_t;

#define UDP_HDR_LEN  8

/* ── Port handler ─────────────────────────────────────────────────────── */
typedef struct {
    uint16_t port;   /* host byte order */
    const char *name;
    /* src_ip / src_port are in network byte order */
    void (*rx)(uint32_t    src_ip,
               uint16_t    src_port,
               const void *payload,
               size_t      payload_len);
} udp_port_handler_t;

#define UDP_MAX_PORTS  16

/* Register a UDP port listener.  Returns 0 on success, -1 if table full. */
int udp_port_register(const udp_port_handler_t *h);

/* ── RX / TX ─────────────────────────────────────────────────────────── */
void udp_rx(const ip_hdr_t *iph, const uint8_t *payload, size_t payload_len);

/*
 * Send a UDP datagram.
 * dst_ip / dst_port – destination (host byte order for port).
 * src_port          – source port (host byte order).
 * data / len        – payload.
 */
int udp_send(uint32_t    dst_ip,
             uint16_t    dst_port,
             uint16_t    src_port,
             const void *data,
             size_t      len);

/* Register with IP sub-handler table (call once at startup). */
void udp_register(void);

#endif /* NETCTL_PROTOCOL_UDP_H */