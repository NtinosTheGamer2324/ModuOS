#define LIBC_NO_START
#include "libc.h"
#include "string.h"
#include "netman/protocol/ip/ipv4.h"

/* ── Internal driver ABI (same as netman.c) ──────────────────────────────── */
#define NET_CMD_TX_FRAME    5

typedef struct {
    uint32_t cmd;
    uint32_t len;
    uint8_t  data[1500];
} _ipv4_net_cmd_t;

typedef struct {
    uint32_t cmd;
    int32_t  status;
    uint32_t len;
    uint8_t  data[1500];
} _ipv4_net_reply_t;

static int _ipv4_tx(int fd, const void *frame, uint32_t len) {
    _ipv4_net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cmd = NET_CMD_TX_FRAME;
    c.len = len;
    if (len > sizeof(c.data)) return -1;
    memcpy(c.data, frame, len);
    if (write(fd, &c, sizeof(c)) < 0) return -1;

    _ipv4_net_reply_t r;
    if (read(fd, &r, sizeof(r)) < 0) return -1;
    return r.status;
}

/* ── Broadcast addresses ─────────────────────────────────────────────────── */
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── ipv4_init ────────────────────────────────────────────────────────────── */
void ipv4_init(ipv4_ctx_t *ctx, int net_fd,
               const uint8_t mac[6], const uint8_t ip[4]) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->net_fd     = net_fd;
    ctx->id_counter = 1;
    memcpy(ctx->mac, mac, 6);
    memcpy(ctx->ip,  ip,  4);
}

/* ── ipv4_checksum ────────────────────────────────────────────────────────── */
uint16_t ipv4_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p   += 2;
        len -= 2;
    }
    if (len == 1)
        sum += ((uint32_t)p[0] << 8);

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* ── ipv4_send ────────────────────────────────────────────────────────────── */
int ipv4_send(ipv4_ctx_t *ctx,
              const uint8_t dst_mac[6],
              const uint8_t dst_ip[4],
              uint8_t proto,
              const void *payload, uint16_t payload_len) {

    uint32_t total = ETH_HDR_LEN + IPV4_HDR_LEN + payload_len;
    if (total > 1514) return -1;   /* exceeds standard MTU frame */

    uint8_t frame[1514];
    memset(frame, 0, total);

    /* ── Ethernet header ────────────────────────────────────────────────── */
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst, dst_mac ? dst_mac : BROADCAST_MAC, 6);
    memcpy(eth->src, ctx->mac, 6);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    /* ── IPv4 header ────────────────────────────────────────────────────── */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + ETH_HDR_LEN);
    ip->ver_ihl   = (4 << 4) | 5;            /* IPv4, 20-byte header */
    ip->dscp_ecn  = 0;
    ip->total_len = htons((uint16_t)(IPV4_HDR_LEN + payload_len));
    ip->id        = htons(ctx->id_counter++);
    ip->flags_frag = htons(IP_FLAG_DF);
    ip->ttl       = 64;
    ip->proto     = proto;
    ip->checksum  = 0;
    memcpy(ip->src, ctx->ip,  4);
    memcpy(ip->dst, dst_ip,   4);
    ip->checksum  = ipv4_checksum(ip, IPV4_HDR_LEN);

    /* ── Payload ─────────────────────────────────────────────────────────── */
    memcpy(frame + ETH_HDR_LEN + IPV4_HDR_LEN, payload, payload_len);

    return _ipv4_tx(ctx->net_fd, frame, total);
}

/* ── ipv4_parse ───────────────────────────────────────────────────────────── */
const uint8_t *ipv4_parse(ipv4_ctx_t *ctx,
                           const uint8_t *frame, uint32_t frame_len,
                           uint8_t *proto,
                           uint8_t src_ip[4],
                           uint16_t *payload_len) {
    if (frame_len < (uint32_t)(ETH_HDR_LEN + IPV4_HDR_LEN)) return NULL;

    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) return NULL;

    /* Must be for us or broadcast */
    if (memcmp(eth->dst, ctx->mac, 6) != 0 &&
        memcmp(eth->dst, BROADCAST_MAC, 6) != 0) return NULL;

    const ipv4_hdr_t *ip =
        (const ipv4_hdr_t *)(frame + ETH_HDR_LEN);

    /* Version check */
    if ((ip->ver_ihl >> 4) != 4) return NULL;

    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < IPV4_HDR_LEN) return NULL;
    if (frame_len < (uint32_t)(ETH_HDR_LEN + ihl)) return NULL;

    /* Destination: us or broadcast */
    static const uint8_t bcast_ip[4] = {255,255,255,255};
    if (memcmp(ip->dst, ctx->ip, 4) != 0 &&
        memcmp(ip->dst, bcast_ip, 4) != 0) return NULL;

    /* Verify header checksum */
    if (ipv4_checksum(ip, ihl) != 0) return NULL;

    uint16_t total = ntohs(ip->total_len);
    if (total < ihl) return NULL;

    *proto       = ip->proto;
    *payload_len = (uint16_t)(total - ihl);
    memcpy(src_ip, ip->src, 4);

    return frame + ETH_HDR_LEN + ihl;
}

/* ── ipv4_print_addr ──────────────────────────────────────────────────────── */
void ipv4_print_addr(const uint8_t ip[4]) {
    printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}