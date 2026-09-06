// netd.c — minimal network daemon: owns net0 exclusively, exposes
// ARP-cache + ping over a SHM mailbox (see netd_ipc.h). This is netd v1:
// no TCP/UDP/DNS yet, just enough to prove the client/server split works.
//
// Client transport used to be UserFS's $/user/netd invoke() node, but
// UserFS only round-trips a single fixed in/out buffer through the kernel
// and doesn't scale past that trivial case, so it's been swapped for a
// SHM mailbox that netd owns and clients attach to.
//


#include "libc.h"
#include "netd_ipc.h"

// ── Ethernet / ARP / IPv4 / ICMP wire structs ──────────────────────────

#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV4 0x0800u
#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_OP_REQUEST     1u
#define ARP_OP_REPLY       2u
#define IP_PROTO_ICMP 1u
#define ICMP_TYPE_ECHO_REQUEST 8u
#define ICMP_TYPE_ECHO_REPLY   0u
#define ETH_FRAME_MIN 60

typedef struct __attribute__((packed)) {
    uint8_t dst_mac[6], src_mac[6];
    uint16_t ethertype;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t oper;
    uint8_t sha[6], spa[4], tha[6], tpa[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl, dscp_ecn;
    uint16_t total_len, id, flags_frag;
    uint8_t ttl, proto;
    uint16_t checksum;
    uint8_t src_ip[4], dst_ip[4];
} ipv4_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t type, code;
    uint16_t checksum, id, seq;
} icmp_hdr_t;

#define ICMP_PAYLOAD_LEN 32
#define IP_PACKET_LEN (sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + ICMP_PAYLOAD_LEN)
#define FRAME_LEN_RAW (sizeof(eth_hdr_t) + IP_PACKET_LEN)
#define FRAME_LEN (FRAME_LEN_RAW < ETH_FRAME_MIN ? ETH_FRAME_MIN : FRAME_LEN_RAW)

// ── ARP cache (tiny, linear) ────────────────────────────────────────────

#define ARP_CACHE_SIZE 16
typedef struct {
    int used;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_entry_t;

static arp_entry_t g_arp_cache[ARP_CACHE_SIZE];
static int g_net_fd = -1;
static uint8_t g_our_mac[6];
static uint8_t g_our_ip[4]     = {10, 0, 2, 15};
static uint8_t g_gateway_ip[4] = {10, 0, 2, 2};

#define NET_CMD_GET_MAC 4u

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint16_t checksum16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += get_be16(p); p += 2; len -= 2; }
    if (len == 1) sum += ((uint16_t)p[0]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static arp_entry_t *arp_cache_find(const uint8_t ip[4]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_arp_cache[i].used && ip_eq(g_arp_cache[i].ip, ip)) return &g_arp_cache[i];
    return NULL;
}

static void arp_cache_put(const uint8_t ip[4], const uint8_t mac[6]) {
    arp_entry_t *e = arp_cache_find(ip);
    if (!e) {
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!g_arp_cache[i].used) { e = &g_arp_cache[i]; break; }
        }
        if (!e) e = &g_arp_cache[0]; // crude eviction if full
    }
    e->used = 1;
    memcpy(e->ip, ip, 4);
    memcpy(e->mac, mac, 6);
}

// Drains net0's RX ring, feeding ARP replies into the cache and handing
// back any IPv4/ICMP frame found (so callers can also look for ping replies
// in the same pass). Returns 1 if an IPv4 frame was copied into out_buf.
static int net_poll_once(uint8_t *out_buf, size_t out_cap, ssize_t *out_len) {
    uint8_t buf[1600];
    ssize_t r = read(g_net_fd, buf, sizeof(buf));
    if (r <= 0) return 0;

    if ((size_t)r < sizeof(eth_hdr_t)) return 0;
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    uint16_t et = get_be16((uint8_t *)&eth->ethertype);

    if (et == ETHERTYPE_ARP && (size_t)r >= sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        arp_pkt_t *arp = (arp_pkt_t *)(buf + sizeof(eth_hdr_t));
        if (get_be16((uint8_t *)&arp->oper) == ARP_OP_REPLY) {
            arp_cache_put(arp->spa, arp->sha);
        }
        return 0;
    }

    if (et == ETHERTYPE_IPV4 && out_buf) {
        size_t n = (size_t)r < out_cap ? (size_t)r : out_cap;
        memcpy(out_buf, buf, n);
        if (out_len) *out_len = (ssize_t)r;
        return 1;
    }

    return 0;
}

static int arp_resolve(const uint8_t target_ip[4], uint32_t timeout_ms, uint8_t out_mac[6]) {
    arp_entry_t *cached = arp_cache_find(target_ip);
    if (cached) { memcpy(out_mac, cached->mac, 6); return 0; }

    uint8_t frame[ETH_FRAME_MIN];
    memset(frame, 0, sizeof(frame));
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_ARP);

    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    put_be16((uint8_t *)&arp->htype, ARP_HTYPE_ETHERNET);
    put_be16((uint8_t *)&arp->ptype, ARP_PTYPE_IPV4);
    arp->hlen = 6; arp->plen = 4;
    put_be16((uint8_t *)&arp->oper, ARP_OP_REQUEST);
    memcpy(arp->sha, g_our_mac, 6);
    memcpy(arp->spa, g_our_ip, 4);
    memset(arp->tha, 0, 6);
    memcpy(arp->tpa, target_ip, 4);

    if (write(g_net_fd, frame, sizeof(frame)) < 0) return -1;

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL); // feeds ARP cache as a side effect
        arp_entry_t *e = arp_cache_find(target_ip);
        if (e) { memcpy(out_mac, e->mac, 6); return 0; }
        yield();
    }
    return -1;
}

static int do_ping(const uint8_t target_ip[4], uint32_t timeout_ms, uint32_t *out_rtt_ms) {
    uint8_t gw_mac[6];
    if (arp_resolve(g_gateway_ip, timeout_ms, gw_mac) != 0) return -1;

    uint8_t frame[FRAME_LEN];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst_mac, gw_mac, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)IP_PACKET_LEN);
    put_be16((uint8_t *)&ip->id, (uint16_t)(time_ms() & 0xFFFF));
    ip->ttl = 64;
    ip->proto = IP_PROTO_ICMP;
    memcpy(ip->src_ip, g_our_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    icmp_hdr_t *icmp = (icmp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    put_be16((uint8_t *)&icmp->id, 4242);
    put_be16((uint8_t *)&icmp->seq, 1);
    uint8_t *payload = (uint8_t *)icmp + sizeof(icmp_hdr_t);
    for (int i = 0; i < ICMP_PAYLOAD_LEN; i++) payload[i] = (uint8_t)('a' + (i % 23));
    put_be16((uint8_t *)&icmp->checksum, checksum16(icmp, sizeof(icmp_hdr_t) + ICMP_PAYLOAD_LEN));

    uint64_t t_start = time_ms();
    if (write(g_net_fd, frame, sizeof(frame)) < 0) return -1;

    uint64_t deadline = t_start + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t rxbuf[1600];
        ssize_t rxlen = 0;
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen)) {
            size_t min_len = sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t);
            if ((size_t)rxlen < min_len) continue;
            ipv4_hdr_t *rip = (ipv4_hdr_t *)(rxbuf + sizeof(eth_hdr_t));
            if (rip->proto != IP_PROTO_ICMP || !ip_eq(rip->src_ip, target_ip)) continue;
            uint8_t ihl = (uint8_t)((rip->ver_ihl & 0x0F) * 4);
            icmp_hdr_t *ricmp = (icmp_hdr_t *)((uint8_t *)rip + ihl);
            if (ricmp->type != ICMP_TYPE_ECHO_REPLY) continue;
            if (get_be16((uint8_t *)&ricmp->id) != 4242) continue;
            *out_rtt_ms = (uint32_t)(time_ms() - t_start);
            return 0;
        }
        yield();
    }
    return -1;
}

// ── Mailbox request handler ─────────────────────────────────────────────
//
// Same logic that used to live inside the UserFS invoke() callback; it
// just no longer needs to validate in_size/out_size since the SHM mailbox
// carries typed structs directly instead of raw buffers.
static void netd_process_request(const netd_request_t *req, netd_response_t *resp) {
    memset(resp, 0, sizeof(*resp));

    switch (req->cmd) {
        case NETD_CMD_PING: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 3000;
            uint32_t rtt = 0;
            if (do_ping(req->target_ip, timeout, &rtt) == 0) {
                resp->success = 1;
                resp->rtt_ms = rtt;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "ping timed out");
            }
            break;
        }

        case NETD_CMD_RESOLVE_ARP: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 3000;
            uint8_t mac[6];
            if (arp_resolve(req->target_ip, timeout, mac) == 0) {
                resp->success = 1;
                memcpy(resp->mac, mac, 6);
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "arp resolve failed");
            }
            break;
        }

        case NETD_CMD_GET_STATUS: {
            resp->success = 1;
            memcpy(resp->mac, g_our_mac, 6);
            memcpy(resp->our_ip, g_our_ip, 4);
            memcpy(resp->gateway_ip, g_gateway_ip, 4);
            resp->link_up = 1;
            break;
        }

        default:
            resp->success = 0;
            snprintf(resp->message, sizeof(resp->message), "unknown cmd %d", req->cmd);
            break;
    }
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("netd: starting...\n");

    g_net_fd = open("$/dev/net/net0", O_RDWR | O_NONBLOCK, 0);
    if (g_net_fd < 0) {
        printf("netd: could not open $/dev/net/net0 (errno=%d)\n", errno);
        return 1;
    }

    {
        uint32_t cmd = NET_CMD_GET_MAC;
        if (invoke(g_net_fd, &cmd, sizeof(cmd), g_our_mac, sizeof(g_our_mac)) < 0) {
            printf("netd: GET_MAC failed\n");
            close(g_net_fd);
            return 1;
        }
    }

    memset(g_arp_cache, 0, sizeof(g_arp_cache));

    // Create (or re-attach to) the client mailbox. SHM_O_CREAT without
    // SHM_O_EXCL is fine even if a previous netd left the segment behind:
    // same size in both cases, so shm_open() won't complain.
    int shm_handle = shm_open(NETD_IPC_SHM_NAME, O_RDWR | SHM_O_CREAT, 0644, sizeof(netd_shm_t));
    if (shm_handle < 0) {
        printf("netd: shm_open(%s) failed (errno=%d)\n", NETD_IPC_SHM_NAME, -shm_handle);
        close(g_net_fd);
        return 1;
    }

    netd_shm_t *shm = (netd_shm_t *)mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W,
                                          MAP_SHARED, shm_handle);
    if (shm == MAP_FAILED) {
        printf("netd: mmap of shm mailbox failed\n");
        close(g_net_fd);
        return 1;
    }

    // We own this mailbox exclusively (one netd instance), so it's safe
    // to reset it to a known-idle state on startup.
    memset(shm, 0, sizeof(*shm));

    printf("netd: mailbox '%s' ready, owns net0 exclusively.\n", NETD_IPC_SHM_NAME);
    printf("netd: mac=");
    for (int i = 0; i < 6; i++) printf("%02x%s", g_our_mac[i], i < 5 ? ":" : "\n");

    // Single loop does double duty:
    //  - drains net0 opportunistically so ARP replies get cached and the
    //    RX ring never backs up, even with no client waiting on anything;
    //  - services at most one pending mailbox request per pass. Request
    //    handling (do_ping/arp_resolve) is itself a blocking spin loop
    //    that keeps calling net_poll_once(), so this stays effectively
    //    the same single-threaded behavior netd v1 had over UserFS
    //    invoke() -- one client's request runs to completion before the
    //    next one (serialized by netd_ipc's mailbox lock) is picked up.
    while (1) {
        net_poll_once(NULL, 0, NULL);

        if (netd_ipc_get_state(shm) == NETD_SLOT_REQUEST_PENDING) {
            netd_process_request(&shm->req, &shm->resp);
            netd_ipc_set_state(shm, NETD_SLOT_RESPONSE_READY);
        }

        yield();
    }

    return 0;
}