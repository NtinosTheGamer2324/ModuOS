/*
 * arp.c  –  ARP (RFC 826) for ModuOS netctl
 *
 * Handles:
 *   - ARP Request reception → send ARP Reply (answer for our IP)
 *   - ARP Reply reception   → populate the ARP cache
 *   - ARP Request TX        → resolve an IP to MAC
 *
 * The ARP cache is maintained in netctl.c (g_arp_cache[]).
 * arp_resolve() is a blocking poll-loop that sends a request and waits
 * up to ~500 ms for a reply.
 */

#define LIBC_NO_START
#include "libc.h"
#include "netctl.h"
#include "protocol/arp.h"

/* ── Build and send an ARP packet ────────────────────────────────────── */
static int arp_send(uint16_t opcode,
                    const uint8_t *dst_mac,
                    ipv4_addr_t    dst_ip)
{
    arp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    /* Ethernet header fields inside arp_packet_t */
    memcpy(pkt.dst_mac, dst_mac,      ETH_ALEN);
    memcpy(pkt.src_mac, g_netcfg.mac, ETH_ALEN);
    pkt.ethertype = htons(ETHERTYPE_ARP);

    /* ARP body */
    pkt.arp.hw_type   = htons(ARP_ETHERNET);
    pkt.arp.prot_type = htons(ARP_IPV4);
    pkt.arp.hw_len    = ETH_ALEN;
    pkt.arp.prot_len  = 4;
    pkt.arp.opcode    = htons(opcode);

    memcpy(pkt.arp.src_mac, g_netcfg.mac, ETH_ALEN);
    pkt.arp.src_ip = g_netcfg.ip;

    if (opcode == ARP_OP_REPLY) {
        memcpy(pkt.arp.dst_mac, dst_mac, ETH_ALEN);
    }
    /* For requests the dst_mac in the ARP body is all-zero (unknown). */
    pkt.arp.dst_ip = dst_ip;

    /* arp_packet_t already contains the Ethernet header, so send the ARP
     * body (sizeof(arp_header_t)) via netctl_eth_send which prepends its
     * own Ethernet header.  To keep things simple we send just the body
     * and let netctl_eth_send build the outer header. */
    return netctl_eth_send(dst_mac,
                           ETHERTYPE_ARP,
                           &pkt.arp,
                           sizeof(arp_header_t));
}

/* ── RX handler ───────────────────────────────────────────────────────── */
void arp_rx(const uint8_t *frame, size_t len) {
    if (len < ETH_HDR_LEN + sizeof(arp_header_t)) return;

    const arp_header_t *arp =
        (const arp_header_t *)(frame + ETH_HDR_LEN);

    /* Validate: must be Ethernet/IPv4 ARP */
    if (ntohs(arp->hw_type)   != ARP_ETHERNET) return;
    if (ntohs(arp->prot_type) != ARP_IPV4)     return;
    if (arp->hw_len   != ETH_ALEN) return;
    if (arp->prot_len != 4)        return;

    uint16_t op     = ntohs(arp->opcode);
    ipv4_addr_t src = arp->src_ip;

    /* Always update the cache with the sender's mapping */
    if (src != 0)
        arp_cache_update(src, arp->src_mac);

    switch (op) {
        case ARP_OP_REQUEST:
            /* If the request is for our IP, send a reply */
            if (arp->dst_ip == g_netcfg.ip) {
                arp_send(ARP_OP_REPLY, arp->src_mac, src);
            }
            break;

        case ARP_OP_REPLY:
            /* Cache already updated above; nothing more to do */
            break;

        default:
            break;
    }
}

/* ── Gratuitous ARP (announce our own IP/MAC) ────────────────────────── */
void arp_announce(void) {
    arp_send(ARP_OP_REQUEST, ETH_BROADCAST, g_netcfg.ip);
}

/* ── Blocking ARP resolve ─────────────────────────────────────────────── */
/*
 * Returns a pointer to a static 6-byte buffer containing the resolved MAC,
 * or NULL on timeout.  Polls for up to ~500 ms (50 * 10 ms yield loops).
 */
const uint8_t *arp_resolve(ipv4_addr_t ip) {
    /* Already cached? */
    const uint8_t *cached = arp_cache_lookup(ip);
    if (cached) return cached;

    /* Send ARP request (broadcast) */
    arp_send(ARP_OP_REQUEST, ETH_BROADCAST, ip);

    /* Poll for a reply (up to 50 iterations ≈ 500 ms) */
    for (int tries = 0; tries < 50; tries++) {
        netctl_rx_poll_once();

        /* Check cache again */
        cached = arp_cache_lookup(ip);
        if (cached) return cached;

        yield();
    }

    printf("[arp] resolve timeout for ");
    netctl_print_ip(ip);
    putc('\n');
    return NULL;
}

/* ── Protocol registration ────────────────────────────────────────────── */
static const proto_handler_t g_arp_handler = {
    .ethertype = ETHERTYPE_ARP,
    .name      = "ARP",
    .rx        = arp_rx,
};

void arp_register(void) {
    netctl_proto_register(&g_arp_handler);
    /* Announce ourselves on startup */
    arp_announce();
}