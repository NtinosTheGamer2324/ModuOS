#ifndef NETCTL_PROTOCOL_ICMP_H
#define NETCTL_PROTOCOL_ICMP_H

/*
 * icmp.h  –  ICMP (RFC 792) definitions
 *
 * Currently implements:
 *   - Echo Request (type 8) / Echo Reply (type 0)  — ping responder
 *
 * Adding more ICMP types: define the type constant and handle it in
 * icmp_rx() in src/protocol/icmp.c.
 */

#include <stdint.h>
#include <stddef.h>
#include "ip.h"

/* ICMP type codes */
#define ICMP_ECHO_REPLY    0u
#define ICMP_ECHO_REQUEST  8u

/* ── ICMP Header ──────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;         /* echo: identifier  */
    uint16_t seq;        /* echo: sequence no */
} icmp_hdr_t;

/* ── ICMP RX / TX ────────────────────────────────────────────────────── */
void icmp_rx(const ip_hdr_t *iph, const uint8_t *payload, size_t payload_len);

/*
 * Send an ICMP Echo Request to dst_ip.
 * id / seq are in host byte order (icmp_send converts them).
 * Returns 0 on success.
 */
int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq);

/* Register with the IP sub-handler table (call once at startup). */
void icmp_register(void);

#endif /* NETCTL_PROTOCOL_ICMP_H */