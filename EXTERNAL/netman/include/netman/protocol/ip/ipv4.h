#pragma once
#include "libc.h"

/* ── EtherType ───────────────────────────────────────────────────────────── */
#define ETHERTYPE_IPV4      0x0800
#define ETHERTYPE_ARP       0x0806

/* ── IP protocol numbers ─────────────────────────────────────────────────── */
#define IP_PROTO_ICMP       1
#define IP_PROTO_TCP        6
#define IP_PROTO_UDP        17

/* ── IPv4 header flags / offsets ─────────────────────────────────────────── */
#define IP_FLAG_DF          0x4000   /* Don't Fragment */
#define IP_FLAG_MF          0x2000   /* More Fragments */
#define IP_FRAG_MASK        0x1FFF

/* ── Wire-format IPv4 header (no options) ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       /* version (4) | IHL (5 for no-options) */
    uint8_t  dscp_ecn;
    uint16_t total_len;     /* header + payload, big-endian */
    uint16_t id;
    uint16_t flags_frag;    /* flags[3] | fragment_offset[13], big-endian */
    uint8_t  ttl;
    uint8_t  proto;         /* IP_PROTO_* */
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} ipv4_hdr_t;

#define IPV4_HDR_LEN  20   /* bytes, options not supported */

/* ── Ethernet frame header ───────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;     /* big-endian */
} eth_hdr_t;

#define ETH_HDR_LEN  14

/* ── Byte-order helpers (host = little-endian AMD64) ─────────────────────── */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x000000FFu) << 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── IPv4 pseudo-header (for UDP/TCP checksum) ───────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  src[4];
    uint8_t  dst[4];
    uint8_t  zero;
    uint8_t  proto;
    uint16_t len;           /* big-endian: UDP/TCP segment length */
} ipv4_pseudo_hdr_t;

/* ── IPv4 context ────────────────────────────────────────────────────────── */
typedef struct {
    int     net_fd;
    uint8_t mac[6];
    uint8_t ip[4];
    uint16_t id_counter;    /* incremented per packet sent */
} ipv4_ctx_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

void ipv4_init(ipv4_ctx_t *ctx, int net_fd,
               const uint8_t mac[6], const uint8_t ip[4]);

/* Compute standard Internet checksum over `len` bytes at `data`. */
uint16_t ipv4_checksum(const void *data, uint32_t len);

/*
 * Build and transmit an IPv4 packet.
 *   dst_mac   – resolved Ethernet destination (from ARP cache)
 *   dst_ip    – destination IPv4 address
 *   proto     – IP_PROTO_UDP / IP_PROTO_TCP / IP_PROTO_ICMP
 *   payload   – upper-layer bytes
 *   payload_len
 * Returns 0 on success, <0 on error.
 */
int ipv4_send(ipv4_ctx_t *ctx,
              const uint8_t dst_mac[6],
              const uint8_t dst_ip[4],
              uint8_t proto,
              const void *payload, uint16_t payload_len);

/*
 * Parse and dispatch an inbound Ethernet frame.
 * Returns the IPv4 payload pointer and fills *proto, *src_ip, *payload_len.
 * Returns NULL if the frame is not a valid IPv4 unicast/broadcast for us.
 */
const uint8_t *ipv4_parse(ipv4_ctx_t *ctx,
                           const uint8_t *frame, uint32_t frame_len,
                           uint8_t *proto,
                           uint8_t src_ip[4],
                           uint16_t *payload_len);

/* Print a dotted-decimal IPv4 address (no newline). */
void ipv4_print_addr(const uint8_t ip[4]);