#ifndef NETCTL_PROTOCOL_IP_H
#define NETCTL_PROTOCOL_IP_H

/*
 * ip.h  –  IPv4 header and helpers
 *
 * All multi-byte fields are in NETWORK (big-endian) byte order.
 * Use htons/htonl from netctl.h when building packets.
 */

#include <stdint.h>
#include <stddef.h>

/* IP protocol numbers */
#define IP_PROTO_ICMP   1u
#define IP_PROTO_TCP    6u
#define IP_PROTO_UDP   17u

/* ── IPv4 Header ──────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       /* version (4) | IHL in 32-bit words */
    uint8_t  dscp_ecn;
    uint16_t total_len;     /* network byte order */
    uint16_t id;
    uint16_t frag_off;      /* flags | fragment offset, network byte order */
    uint8_t  ttl;
    uint8_t  protocol;      /* IP_PROTO_* */
    uint16_t checksum;
    uint32_t src_ip;        /* network byte order */
    uint32_t dst_ip;        /* network byte order */
    /* options follow if IHL > 5, but we don't generate them */
} ip_hdr_t;

#define IP_HDR_LEN  20   /* bytes, no options */
#define IP_VERSION  4
#define IP_TTL_DEF  64

/* Extract IHL in bytes */
#define IP_IHL_BYTES(hdr)  (((hdr)->ver_ihl & 0x0Fu) * 4u)

/* ── Internet Checksum ────────────────────────────────────────────────── */
/*
 * Computes the standard 16-bit ones-complement checksum over `len` bytes.
 * Pass checksum field zeroed.  Result is already in network byte order.
 */
static inline uint16_t inet_checksum(const void *data, size_t len) {
    const uint8_t *p   = (const uint8_t *)data;
    uint32_t        sum = 0;

    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

/* ── IP RX / TX (implemented in src/protocol/ip.c) ───────────────────── */

/* Called by proto_handler_t.rx for ETHERTYPE_IP frames. */
void ip_rx(const uint8_t *frame, size_t len);

/*
 * Send an IPv4 packet.
 * dst_ip      – destination (network byte order); used for ARP + routing.
 * protocol    – IP_PROTO_*
 * payload     – layer-4 data (already assembled by caller)
 * payload_len – byte count
 * Returns 0 on success, negative on failure.
 */
int ip_send(uint32_t    dst_ip,
            uint8_t     protocol,
            const void *payload,
            size_t      payload_len);

/* Register the IP handler with netctl (call once at startup). */
void ip_register(void);

/* ── IP-layer protocol demux (sub-handlers) ──────────────────────────── */
/*
 * Just like netctl's ethertype dispatch, IP has its own sub-handler table
 * keyed on the "protocol" field.  Adding TCP, UDP, ICMP, etc. all go here.
 */

typedef struct {
    uint8_t     protocol;
    const char *name;
    void (*rx)(const ip_hdr_t *iph, const uint8_t *payload, size_t payload_len);
} ip_proto_handler_t;

#define IP_MAX_PROTOS  8

int ip_proto_register(const ip_proto_handler_t *h);

#endif /* NETCTL_PROTOCOL_IP_H */