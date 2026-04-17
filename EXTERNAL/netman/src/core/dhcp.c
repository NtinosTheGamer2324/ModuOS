#define LIBC_NO_START
#include "libc.h"
#include "string.h"
#include "netman/core/dhcp.h"
#include "netman/protocol/ip/ipv4.h"

/* ── Internal driver ABI ─────────────────────────────────────────────────── */
#define NET_CMD_TX_FRAME    5
#define NET_CMD_RX_POLL     6

typedef struct {
    uint32_t cmd;
    uint32_t len;
    uint8_t  data[1500];
} _dhcp_net_cmd_t;

typedef struct {
    uint32_t cmd;
    int32_t  status;
    uint32_t len;
    uint8_t  data[1500];
} _dhcp_net_reply_t;

static int _dhcp_tx_raw(int fd, const void *frame, uint32_t len) {
    _dhcp_net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cmd = NET_CMD_TX_FRAME;
    c.len = len;
    if (len > sizeof(c.data)) return -1;
    memcpy(c.data, frame, len);
    if (write(fd, &c, sizeof(c)) < 0) return -1;

    _dhcp_net_reply_t r;
    if (read(fd, &r, sizeof(r)) < 0) return -1;
    return r.status;
}

static int _dhcp_rx(int fd, uint8_t *out, uint32_t *out_len) {
    _dhcp_net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cmd = NET_CMD_RX_POLL;

    _dhcp_net_reply_t r;
    if (write(fd, &c, sizeof(c)) < 0) return -1;
    if (read(fd, &r, sizeof(r)) < 0) return -1;
    if (r.status < 0 || r.len == 0) return -1;

    *out_len = r.len;
    memcpy(out, r.data, r.len);
    return 0;
}

/* ── DHCP option helpers ──────────────────────────────────────────────────── */

static uint8_t *_opt_write(uint8_t *p, uint8_t code,
                            const void *val, uint8_t len) {
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static uint8_t *_opt_write_u8(uint8_t *p, uint8_t code, uint8_t val) {
    return _opt_write(p, code, &val, 1);
}

/* Find option in received packet; returns pointer to value or NULL. */
static const uint8_t *_opt_find(const dhcp_pkt_t *pkt, uint8_t code,
                                  uint8_t *out_len) {
    const uint8_t *p   = pkt->options;
    const uint8_t *end = pkt->options + DHCP_OPTIONS_LEN;

    while (p < end && *p != DHCP_OPT_END) {
        uint8_t c = *p++;
        if (c == 0) continue;   /* pad */
        if (p >= end) break;
        uint8_t l = *p++;
        if (p + l > end) break;
        if (c == code) {
            if (out_len) *out_len = l;
            return p;
        }
        p += l;
    }
    return NULL;
}

/* ── Build and send a DHCP frame (Ethernet/IP/UDP/DHCP) ─────────────────── */

static const uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t BCAST_IP[4]  = {255,255,255,255};
static const uint8_t ZERO_IP[4]   = {0,0,0,0};

static int _dhcp_send(dhcp_ctx_t *ctx,
                       const dhcp_pkt_t *pkt,
                       const uint8_t dst_ip[4]) {
    /* Build UDP payload = dhcp_pkt_t */
    uint16_t dhcp_len = (uint16_t)sizeof(dhcp_pkt_t);
    uint16_t udp_len  = (uint16_t)(sizeof(udp_hdr_t) + dhcp_len);

    /* Frame: ETH + IP + UDP + DHCP */
    uint32_t frame_len = ETH_HDR_LEN + IPV4_HDR_LEN + udp_len;
    uint8_t frame[1514];
    memset(frame, 0, frame_len);

    /* Ethernet */
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst, BCAST_MAC, 6);
    memcpy(eth->src, ctx->mac, 6);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    /* IPv4 */
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + ETH_HDR_LEN);
    ip->ver_ihl    = (4 << 4) | 5;
    ip->dscp_ecn   = 0;
    ip->total_len  = htons((uint16_t)(IPV4_HDR_LEN + udp_len));
    ip->id         = htons(ctx->xid & 0xFFFF);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_UDP;
    ip->checksum   = 0;
    memcpy(ip->src, ZERO_IP,  4);   /* 0.0.0.0 until we have a lease */
    memcpy(ip->dst, dst_ip,   4);

    /* Use leased IP as source if we have one */
    if (ctx->lease.valid)
        memcpy(ip->src, ctx->lease.ip, 4);

    ip->checksum = ipv4_checksum(ip, IPV4_HDR_LEN);

    /* UDP */
    udp_hdr_t *udp = (udp_hdr_t *)(frame + ETH_HDR_LEN + IPV4_HDR_LEN);
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->length   = htons(udp_len);
    udp->checksum = 0;   /* optional for UDP/IPv4 */

    /* DHCP */
    memcpy(frame + ETH_HDR_LEN + IPV4_HDR_LEN + sizeof(udp_hdr_t),
           pkt, dhcp_len);

    return _dhcp_tx_raw(ctx->net_fd, frame, frame_len);
}

/* ── Build base DHCP packet ──────────────────────────────────────────────── */
static void _dhcp_build_base(dhcp_ctx_t *ctx, dhcp_pkt_t *pkt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->op    = DHCP_OP_REQUEST;
    pkt->htype = 1;
    pkt->hlen  = 6;
    pkt->xid   = htonl(ctx->xid);
    pkt->flags = htons(0x8000);   /* broadcast flag */
    memcpy(pkt->chaddr, ctx->mac, 6);
    pkt->magic = htonl(DHCP_MAGIC_COOKIE);
}

/* ── dhcp_init ────────────────────────────────────────────────────────────── */
void dhcp_init(dhcp_ctx_t *ctx, int net_fd,
               const uint8_t mac[6], ipv4_ctx_t *ipv4) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->net_fd = net_fd;
    ctx->ipv4   = ipv4;
    memcpy(ctx->mac, mac, 6);
    ctx->state  = DHCP_STATE_IDLE;
    /* Simple XID: combine low bytes of MAC with a constant */
    ctx->xid = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
             | ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
    if (ctx->xid == 0) ctx->xid = 0xDEADBEEFu;
}

/* ── Send DHCPDISCOVER ───────────────────────────────────────────────────── */
static int _dhcp_discover(dhcp_ctx_t *ctx) {
    dhcp_pkt_t pkt;
    _dhcp_build_base(ctx, &pkt);

    uint8_t *opt = pkt.options;
    opt = _opt_write_u8(opt, DHCP_OPT_MSG_TYPE, DHCP_MSG_DISCOVER);

    /* Parameter request list */
    uint8_t params[] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    opt = _opt_write(opt, DHCP_OPT_PARAM_REQUEST, params, sizeof(params));

    *opt = DHCP_OPT_END;

    ctx->state = DHCP_STATE_SELECTING;
    return _dhcp_send(ctx, &pkt, BCAST_IP);
}

/* ── Send DHCPREQUEST ────────────────────────────────────────────────────── */
static int _dhcp_request_ip(dhcp_ctx_t *ctx,
                             const uint8_t offered_ip[4],
                             const uint8_t server_id[4]) {
    dhcp_pkt_t pkt;
    _dhcp_build_base(ctx, &pkt);

    uint8_t *opt = pkt.options;
    opt = _opt_write_u8(opt, DHCP_OPT_MSG_TYPE, DHCP_MSG_REQUEST);
    opt = _opt_write(opt, DHCP_OPT_REQUESTED_IP, offered_ip, 4);
    opt = _opt_write(opt, DHCP_OPT_SERVER_ID, server_id, 4);

    uint8_t params[] = { DHCP_OPT_SUBNET, DHCP_OPT_ROUTER, DHCP_OPT_DNS };
    opt = _opt_write(opt, DHCP_OPT_PARAM_REQUEST, params, sizeof(params));

    *opt = DHCP_OPT_END;

    ctx->state = DHCP_STATE_REQUESTING;
    return _dhcp_send(ctx, &pkt, BCAST_IP);
}

/* ── Parse an inbound frame and extract DHCP payload ────────────────────── */
static const dhcp_pkt_t *_dhcp_parse_frame(const uint8_t *frame,
                                            uint32_t frame_len) {
    if (frame_len < (uint32_t)(ETH_HDR_LEN + IPV4_HDR_LEN +
                                sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t)))
        return NULL;

    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->ethertype) != ETHERTYPE_IPV4) return NULL;

    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + ETH_HDR_LEN);
    if ((ip->ver_ihl >> 4) != 4)   return NULL;
    if (ip->proto != IP_PROTO_UDP) return NULL;

    uint8_t ihl = (ip->ver_ihl & 0x0F) * 4;
    const udp_hdr_t *udp =
        (const udp_hdr_t *)(frame + ETH_HDR_LEN + ihl);

    if (ntohs(udp->dst_port) != DHCP_CLIENT_PORT) return NULL;

    return (const dhcp_pkt_t *)((const uint8_t *)udp + sizeof(udp_hdr_t));
}

/* ── dhcp_request (DORA) ─────────────────────────────────────────────────── */
int dhcp_request(dhcp_ctx_t *ctx, uint64_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = 10000;

    uint8_t offered_ip[4] = {0};
    uint8_t server_id[4]  = {0};

    if (_dhcp_discover(ctx) < 0) {
        ctx->state = DHCP_STATE_FAILED;
        return -1;
    }

    uint64_t deadline = time_ms() + timeout_ms;
    int got_offer = 0;

    /* ── Wait for OFFER then send REQUEST then wait for ACK ─────────────── */
    while (time_ms() < deadline) {
        uint8_t frame[1500];
        uint32_t flen = 0;

        if (_dhcp_rx(ctx->net_fd, frame, &flen) < 0) {
            /* No frame yet – brief yield */
            yield();
            continue;
        }

        const dhcp_pkt_t *reply = _dhcp_parse_frame(frame, flen);
        if (!reply) continue;

        /* Verify XID */
        if (ntohl(reply->xid) != ctx->xid) continue;
        if (reply->op != DHCP_OP_REPLY)    continue;
        if (ntohl(reply->magic) != DHCP_MAGIC_COOKIE) continue;

        /* Determine message type */
        uint8_t opt_len = 0;
        const uint8_t *msg_type_opt =
            _opt_find(reply, DHCP_OPT_MSG_TYPE, &opt_len);
        if (!msg_type_opt || opt_len < 1) continue;
        uint8_t msg_type = msg_type_opt[0];

        if (msg_type == DHCP_MSG_OFFER && !got_offer) {
            memcpy(offered_ip, reply->yiaddr, 4);

            const uint8_t *sid = _opt_find(reply, DHCP_OPT_SERVER_ID, &opt_len);
            if (sid && opt_len >= 4) memcpy(server_id, sid, 4);

            if (_dhcp_request_ip(ctx, offered_ip, server_id) < 0) {
                ctx->state = DHCP_STATE_FAILED;
                return -1;
            }
            got_offer = 1;
            continue;
        }

        if (msg_type == DHCP_MSG_ACK && got_offer) {
            /* Parse lease options */
            memcpy(ctx->lease.ip, reply->yiaddr, 4);

            const uint8_t *sub = _opt_find(reply, DHCP_OPT_SUBNET, &opt_len);
            if (sub && opt_len >= 4) memcpy(ctx->lease.subnet, sub, 4);

            const uint8_t *gw = _opt_find(reply, DHCP_OPT_ROUTER, &opt_len);
            if (gw && opt_len >= 4) memcpy(ctx->lease.router, gw, 4);

            const uint8_t *dns = _opt_find(reply, DHCP_OPT_DNS, &opt_len);
            if (dns && opt_len >= 4) memcpy(ctx->lease.dns, dns, 4);

            const uint8_t *lt = _opt_find(reply, DHCP_OPT_LEASE_TIME, &opt_len);
            if (lt && opt_len >= 4) {
                ctx->lease.lease_time = ((uint32_t)lt[0] << 24)
                                      | ((uint32_t)lt[1] << 16)
                                      | ((uint32_t)lt[2] <<  8)
                                      |  (uint32_t)lt[3];
            }

            memcpy(ctx->lease.server_id, server_id, 4);
            ctx->lease.valid = 1;
            ctx->state = DHCP_STATE_BOUND;

            /* Configure the IPv4 context with our new address */
            if (ctx->ipv4) {
                memcpy(ctx->ipv4->ip, ctx->lease.ip, 4);
            }

            return 0;
        }

        if (msg_type == DHCP_MSG_NAK) {
            ctx->state = DHCP_STATE_FAILED;
            return -1;
        }
    }

    ctx->state = DHCP_STATE_FAILED;
    return -1;   /* timeout */
}

/* ── dhcp_release ────────────────────────────────────────────────────────── */
int dhcp_release(dhcp_ctx_t *ctx) {
    if (!ctx->lease.valid) return 0;

    dhcp_pkt_t pkt;
    _dhcp_build_base(ctx, &pkt);
    memcpy(pkt.ciaddr, ctx->lease.ip, 4);

    uint8_t *opt = pkt.options;
    opt = _opt_write_u8(opt, DHCP_OPT_MSG_TYPE, DHCP_MSG_RELEASE);
    opt = _opt_write(opt, DHCP_OPT_SERVER_ID, ctx->lease.server_id, 4);
    *opt = DHCP_OPT_END;

    int r = _dhcp_send(ctx, &pkt, ctx->lease.server_id);
    ctx->lease.valid = 0;
    ctx->state = DHCP_STATE_IDLE;
    return r;
}

/* ── dhcp_lease_print ────────────────────────────────────────────────────── */
void dhcp_lease_print(const dhcp_lease_t *lease) {
    if (!lease->valid) {
        printf("DHCP: no active lease\n");
        return;
    }
    printf("DHCP Lease:\n");
    printf("  IP      : %u.%u.%u.%u\n",
           lease->ip[0],     lease->ip[1],
           lease->ip[2],     lease->ip[3]);
    printf("  Subnet  : %u.%u.%u.%u\n",
           lease->subnet[0], lease->subnet[1],
           lease->subnet[2], lease->subnet[3]);
    printf("  Router  : %u.%u.%u.%u\n",
           lease->router[0], lease->router[1],
           lease->router[2], lease->router[3]);
    printf("  DNS     : %u.%u.%u.%u\n",
           lease->dns[0],    lease->dns[1],
           lease->dns[2],    lease->dns[3]);
    printf("  Server  : %u.%u.%u.%u\n",
           lease->server_id[0], lease->server_id[1],
           lease->server_id[2], lease->server_id[3]);
    printf("  Lease   : %u seconds\n", lease->lease_time);
}