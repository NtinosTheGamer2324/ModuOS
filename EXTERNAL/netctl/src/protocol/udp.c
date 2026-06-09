/*
 * udp.c  –  UDP (RFC 768) for ModuOS netctl
 *
 * Handles:
 *   - RX: strip UDP header, dispatch to registered port handler
 *   - TX: build UDP header + optional pseudo-header checksum, hand to IP
 *
 * Checksum policy:
 *   TX checksum is computed using the IPv4 pseudo-header (src/dst IP,
 *   zero byte, protocol, UDP length).  The checksum is required for
 *   correctness with most stacks; setting it to 0 would disable it per
 *   RFC 768, but we compute it properly here.
 */

#define LIBC_NO_START
#include "libc.h"
#include "netctl.h"
#include "protocol/ip.h"
#include "protocol/udp.h"

/* ── Port handler table ───────────────────────────────────────────────── */
static udp_port_handler_t g_ports[UDP_MAX_PORTS];
static int                g_port_count = 0;

int udp_port_register(const udp_port_handler_t *h) {
    if (g_port_count >= UDP_MAX_PORTS) {
        printf("[udp] ERROR: port table full (max %d)\n", UDP_MAX_PORTS);
        return -1;
    }
    g_ports[g_port_count++] = *h;
    printf("[udp] listening on port %u (%s)\n", h->port, h->name);
    return 0;
}

/* ── Pseudo-header checksum ───────────────────────────────────────────── */
/*
 * The UDP checksum is computed over:
 *   [src_ip][dst_ip][0x00][proto=17][udp_len][udp_hdr][payload]
 */
typedef struct __attribute__((packed)) {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t udp_len;
} udp_pseudo_hdr_t;

/* Checksum helper: sum bytes as big-endian 16-bit words.
 * Byte-by-byte so it works on any pointer regardless of alignment. */
static uint32_t csum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t udp_checksum(uint32_t    src_ip,
                              uint32_t    dst_ip,
                              const void *udp_seg,
                              size_t      udp_seg_len)
{
    udp_pseudo_hdr_t ph;
    ph.src_ip   = src_ip;
    ph.dst_ip   = dst_ip;
    ph.zero     = 0;
    ph.protocol = IP_PROTO_UDP;
    ph.udp_len  = htons((uint16_t)udp_seg_len);

    uint32_t sum = 0;
    sum = csum_add(sum, &ph, sizeof(ph));
    sum = csum_add(sum, udp_seg, udp_seg_len);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── RX ───────────────────────────────────────────────────────────────── */
void udp_rx(const ip_hdr_t *iph, const uint8_t *payload, size_t payload_len) {
    if (payload_len < UDP_HDR_LEN) return;

    const udp_hdr_t *udph = (const udp_hdr_t *)payload;
    uint16_t dst_port = ntohs(udph->dst_port);
    uint16_t src_port = udph->src_port;   /* kept in net order for callback */

    size_t data_len = payload_len - UDP_HDR_LEN;
    const uint8_t *data = payload + UDP_HDR_LEN;

    for (int i = 0; i < g_port_count; i++) {
        if (g_ports[i].port == dst_port) {
            g_ports[i].rx(iph->src_ip, src_port, data, data_len);
            return;
        }
    }
    /* No listener on this port — silently drop */
}

/* ── TX ───────────────────────────────────────────────────────────────── */
int udp_send(uint32_t    dst_ip,
             uint16_t    dst_port,
             uint16_t    src_port,
             const void *data,
             size_t      len)
{
    size_t udp_len = UDP_HDR_LEN + len;
    if (udp_len > g_netcfg.mtu - IP_HDR_LEN) {
        printf("[udp] send: payload too large\n");
        return -1;
    }

    static uint8_t udp_buf[ETH_MAX_PAYLOAD];  /* not re-entrant */

    udp_hdr_t *udph = (udp_hdr_t *)udp_buf;
    udph->src_port = htons(src_port);
    udph->dst_port = htons(dst_port);
    udph->length   = htons((uint16_t)udp_len);
    udph->checksum = 0;

    if (data && len)
        memcpy(udp_buf + UDP_HDR_LEN, data, len);

    udph->checksum = udp_checksum(g_netcfg.ip, dst_ip, udp_buf, udp_len);

    return ip_send(dst_ip, IP_PROTO_UDP, udp_buf, udp_len);
}

/* ── Protocol registration ────────────────────────────────────────────── */
static const ip_proto_handler_t g_udp_handler = {
    .protocol = IP_PROTO_UDP,
    .name     = "UDP",
    .rx       = udp_rx,
};

void udp_register(void) {
    ip_proto_register(&g_udp_handler);
}