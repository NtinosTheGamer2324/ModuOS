/*
 * ip.c  –  IPv4 layer for ModuOS netctl
 *
 * Handles:
 *   - RX: validate IP header, dispatch to ip_proto_handler_t table
 *   - TX: build IP header, run ARP to find dst MAC, call netctl_eth_send
 *
 * Does NOT handle fragmentation (frames > MTU are rejected at TX).
 * Does NOT handle IP options (IHL > 5 packets are accepted but options ignored).
 */

#define LIBC_NO_START
#include "libc.h"
#include "netctl.h"
#include "protocol/ip.h"
#include "protocol/arp.h"

/* ── Sub-protocol dispatch table ─────────────────────────────────────── */
static ip_proto_handler_t g_ip_protos[IP_MAX_PROTOS];
static int                g_ip_proto_count = 0;

int ip_proto_register(const ip_proto_handler_t *h) {
    if (g_ip_proto_count >= IP_MAX_PROTOS) {
        printf("[ip] ERROR: IP proto table full (max %d)\n", IP_MAX_PROTOS);
        return -1;
    }
    g_ip_protos[g_ip_proto_count++] = *h;
    printf("[ip] registered IP proto %u (%s)\n", h->protocol, h->name);
    return 0;
}

/* ── IP static fragment counter ──────────────────────────────────────── */
static uint16_t g_ip_id = 1;

/* ── RX ───────────────────────────────────────────────────────────────── */
void ip_rx(const uint8_t *frame, size_t len) {
    if (len < ETH_HDR_LEN + IP_HDR_LEN) return;

    const ip_hdr_t *iph = (const ip_hdr_t *)(frame + ETH_HDR_LEN);

    /* Version check */
    if ((iph->ver_ihl >> 4) != IP_VERSION) return;

    uint16_t total = ntohs(iph->total_len);
    if ((size_t)(ETH_HDR_LEN + total) > len) return;  /* truncated */

    /* Validate header checksum */
    uint16_t saved = iph->checksum;
    /* Temporarily zero checksum for calculation — we work on a local copy */
    ip_hdr_t tmp = *iph;
    tmp.checksum = 0;
    if (inet_checksum(&tmp, IP_HDR_LEN) != saved) {
        /* Corrupt header — drop silently */
        return;
    }

    /* Accept packets for our IP or broadcast */
    if (iph->dst_ip != g_netcfg.ip) {
        uint32_t bcast = g_netcfg.ip | ~g_netcfg.netmask;
        uint32_t all   = 0xFFFFFFFFu;
        if (iph->dst_ip != bcast && iph->dst_ip != all)
            return;
    }

    size_t   ihl     = IP_IHL_BYTES(iph);
    size_t   pay_len = total - ihl;
    const uint8_t *payload = (const uint8_t *)iph + ihl;

    /* Dispatch to registered sub-protocol */
    for (int i = 0; i < g_ip_proto_count; i++) {
        if (g_ip_protos[i].protocol == iph->protocol) {
            g_ip_protos[i].rx(iph, payload, pay_len);
            return;
        }
    }
    /* Unhandled IP protocol — silently drop */
}

/* ── TX ───────────────────────────────────────────────────────────────── */
int ip_send(uint32_t    dst_ip,
            uint8_t     protocol,
            const void *payload,
            size_t      payload_len)
{
    if (payload_len + IP_HDR_LEN > g_netcfg.mtu) {
        printf("[ip] send: payload too large (%d > MTU)\n", (int)payload_len);
        return -1;
    }

    /* ── Routing: use gateway if dst is outside our subnet ───────────── */
    uint32_t next_hop = dst_ip;
    if (g_netcfg.gateway && g_netcfg.netmask) {
        if ((dst_ip & g_netcfg.netmask) != (g_netcfg.ip & g_netcfg.netmask))
            next_hop = g_netcfg.gateway;
    }

    /* ── ARP resolve ─────────────────────────────────────────────────── */
    const uint8_t *dst_mac = arp_resolve(next_hop);
    if (!dst_mac) {
        printf("[ip] send: ARP failed for next-hop ");
        netctl_print_ip(next_hop);
        putc('\n');
        return -1;
    }

    /* ── Build IP header ─────────────────────────────────────────────── */
    static uint8_t ip_buf[ETH_MAX_PAYLOAD];  /* reused per call (not re-entrant) */

    ip_hdr_t *iph = (ip_hdr_t *)ip_buf;
    memset(iph, 0, IP_HDR_LEN);

    iph->ver_ihl   = (IP_VERSION << 4) | (IP_HDR_LEN / 4);
    iph->total_len = htons((uint16_t)(IP_HDR_LEN + payload_len));
    uint16_t _id = g_ip_id++; iph->id = htons(_id);
    iph->frag_off  = 0;
    iph->ttl       = IP_TTL_DEF;
    iph->protocol  = protocol;
    iph->src_ip    = g_netcfg.ip;
    iph->dst_ip    = dst_ip;
    iph->checksum  = 0;
    iph->checksum  = inet_checksum(iph, IP_HDR_LEN);

    /* Copy payload after header */
    memcpy(ip_buf + IP_HDR_LEN, payload, payload_len);

    return netctl_eth_send(dst_mac,
                           ETHERTYPE_IP,
                           ip_buf,
                           IP_HDR_LEN + payload_len);
}

/* ── Protocol registration ────────────────────────────────────────────── */
static const proto_handler_t g_ip_eth_handler = {
    .ethertype = ETHERTYPE_IP,
    .name      = "IPv4",
    .rx        = ip_rx,
};

void ip_register(void) {
    netctl_proto_register(&g_ip_eth_handler);
}