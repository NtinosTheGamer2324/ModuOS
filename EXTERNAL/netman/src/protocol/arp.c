#define LIBC_NO_START
#include "libc.h"
#include "string.h"
#include "netman/protocol/arp.h"
#include "netman/protocol/ip/ipv4.h"

/* ── Internal: net_cmd helpers (mirrors netman.c's driver ABI) ───────────── */

#define NET_CMD_TX_FRAME    5
#define NET_CMD_RX_POLL     6

typedef struct {
    uint32_t cmd;
    uint32_t len;
    uint8_t  data[1500];
} _arp_net_cmd_t;

typedef struct {
    uint32_t cmd;
    int32_t  status;
    uint32_t len;
    uint8_t  data[1500];
} _arp_net_reply_t;

static int _arp_tx(int fd, const void *frame, uint32_t len) {
    _arp_net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cmd = NET_CMD_TX_FRAME;
    c.len = len;
    if (len > sizeof(c.data)) return -1;
    memcpy(c.data, frame, len);
    if (write(fd, &c, sizeof(c)) < 0) return -1;

    _arp_net_reply_t r;
    if (read(fd, &r, sizeof(r)) < 0) return -1;
    return r.status;
}

/* ── Broadcast MAC ───────────────────────────────────────────────────────── */
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── arp_init ─────────────────────────────────────────────────────────────── */
void arp_init(arp_ctx_t *ctx, int net_fd,
              const uint8_t mac[6], const uint8_t ip[4]) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->net_fd = net_fd;
    memcpy(ctx->mac, mac, 6);
    memcpy(ctx->ip, ip, 4);
}

/* ── Build a minimal Ethernet frame with ARP payload and transmit ─────────── */
static int _arp_send(arp_ctx_t *ctx,
                     const uint8_t dst_mac[6],
                     uint16_t oper,
                     const uint8_t tha[6], const uint8_t tpa[4]) {
    uint8_t frame[ETH_HDR_LEN + sizeof(arp_packet_t)];
    memset(frame, 0, sizeof(frame));

    /* Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, ctx->mac, 6);
    eth->ethertype = htons(ETHERTYPE_ARP);

    /* ARP payload */
    arp_packet_t *arp = (arp_packet_t *)(frame + ETH_HDR_LEN);
    arp->htype = htons(ARP_HTYPE_ETHERNET);
    arp->ptype = htons(ARP_PTYPE_IPV4);
    arp->hlen  = ARP_HLEN_ETHERNET;
    arp->plen  = ARP_PLEN_IPV4;
    arp->oper  = htons(oper);
    memcpy(arp->sha, ctx->mac, 6);
    memcpy(arp->spa, ctx->ip,  4);
    memcpy(arp->tha, tha, 6);
    memcpy(arp->tpa, tpa, 4);

    return _arp_tx(ctx->net_fd, frame, sizeof(frame));
}

/* ── arp_send_request ─────────────────────────────────────────────────────── */
int arp_send_request(arp_ctx_t *ctx, const uint8_t target_ip[4]) {
    uint8_t zero_mac[6] = {0};
    return _arp_send(ctx, BROADCAST_MAC,
                     ARP_OP_REQUEST, zero_mac, target_ip);
}

/* ── arp_send_reply ───────────────────────────────────────────────────────── */
int arp_send_reply(arp_ctx_t *ctx, const arp_packet_t *req) {
    return _arp_send(ctx, req->sha,
                     ARP_OP_REPLY, req->sha, req->spa);
}

/* ── arp_cache_update ─────────────────────────────────────────────────────── */
void arp_cache_update(arp_ctx_t *ctx,
                      const uint8_t ip[4], const uint8_t mac[6]) {
    /* Refresh existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (ctx->cache[i].valid &&
            memcmp(ctx->cache[i].ip, ip, 4) == 0) {
            memcpy(ctx->cache[i].mac, mac, 6);
            ctx->cache[i].last_seen_ms = time_ms();
            return;
        }
    }
    /* Insert into first free slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!ctx->cache[i].valid) {
            memcpy(ctx->cache[i].ip, ip, 4);
            memcpy(ctx->cache[i].mac, mac, 6);
            ctx->cache[i].last_seen_ms = time_ms();
            ctx->cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: evict oldest entry */
    int oldest = 0;
    for (int i = 1; i < ARP_CACHE_SIZE; i++) {
        if (ctx->cache[i].last_seen_ms < ctx->cache[oldest].last_seen_ms)
            oldest = i;
    }
    memcpy(ctx->cache[oldest].ip, ip, 4);
    memcpy(ctx->cache[oldest].mac, mac, 6);
    ctx->cache[oldest].last_seen_ms = time_ms();
    ctx->cache[oldest].valid = 1;
}

/* ── arp_lookup ───────────────────────────────────────────────────────────── */
const uint8_t *arp_lookup(arp_ctx_t *ctx, const uint8_t ip[4]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (ctx->cache[i].valid &&
            memcmp(ctx->cache[i].ip, ip, 4) == 0) {
            return ctx->cache[i].mac;
        }
    }
    return NULL;
}

/* ── arp_cache_tick ───────────────────────────────────────────────────────── */
void arp_cache_tick(arp_ctx_t *ctx, uint64_t now_ms) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (ctx->cache[i].valid &&
            (now_ms - ctx->cache[i].last_seen_ms) > ARP_ENTRY_TIMEOUT_MS) {
            ctx->cache[i].valid = 0;
        }
    }
}

/* ── arp_handle ───────────────────────────────────────────────────────────── */
void arp_handle(arp_ctx_t *ctx, const uint8_t *frame, uint32_t len) {
    if (len < ETH_HDR_LEN + (uint32_t)sizeof(arp_packet_t)) return;

    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->ethertype) != ETHERTYPE_ARP) return;

    const arp_packet_t *arp =
        (const arp_packet_t *)(frame + ETH_HDR_LEN);

    if (ntohs(arp->htype) != ARP_HTYPE_ETHERNET) return;
    if (ntohs(arp->ptype) != ARP_PTYPE_IPV4)     return;
    if (arp->hlen != ARP_HLEN_ETHERNET)           return;
    if (arp->plen != ARP_PLEN_IPV4)               return;

    /* Always learn sender mapping */
    arp_cache_update(ctx, arp->spa, arp->sha);

    uint16_t op = ntohs(arp->oper);

    if (op == ARP_OP_REQUEST) {
        /* Is it for our IP? */
        if (memcmp(arp->tpa, ctx->ip, 4) == 0) {
            arp_send_reply(ctx, arp);
        }
    }
    /* ARP_OP_REPLY: cache already updated above */
}

/* ── arp_cache_print ──────────────────────────────────────────────────────── */
void arp_cache_print(const arp_ctx_t *ctx) {
    printf("ARP Cache:\n");
    printf("  %-16s  %-17s\n", "IP", "MAC");
    int any = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        const arp_entry_t *e = &ctx->cache[i];
        if (!e->valid) continue;
        any = 1;
        printf("  %u.%u.%u.%u       ",
               e->ip[0], e->ip[1], e->ip[2], e->ip[3]);
        for (int j = 0; j < 6; j++) {
            const char hex[] = "0123456789abcdef";
            char buf[3];
            buf[0] = hex[(e->mac[j] >> 4) & 0xF];
            buf[1] = hex[e->mac[j] & 0xF];
            buf[2] = 0;
            printf("%s", buf);
            if (j < 5) printf(":");
        }
        printf("\n");
    }
    if (!any) printf("  (empty)\n");
}