// netd.c — network daemon: owns net0 exclusively, exposes ARP-cache,
// ping, one-shot UDP send/receive, and now TCP connections, all over a
// SHM mailbox (see netd_ipc.h).
//
// Client transport used to be UserFS's $/user/netd invoke() node, but
// UserFS only round-trips a single fixed in/out buffer through the
// kernel and doesn't scale past that trivial case, so it's been swapped
// for a SHM mailbox that netd owns and clients attach to.
//
// v4 adds a DHCP client (see the "DHCP client" section below), run once
// at startup before the mailbox even exists. Why: SLIRP always handed
// out 10.0.2.15/10.0.2.2 so hardcoding those was harmless, but passt
// mirrors the host's own addressing and expects the guest to actually
// do DHCP to find out what it is. g_our_ip/g_gateway_ip below are now
// just the *fallback* used if no DHCP server answers.
//
// v3 adds TCP. Design tradeoffs, so future-you doesn't have to
// rediscover them by reading the state machine cold:
//   - Stop-and-wait, not sliding window: at most one outstanding unacked
//     segment per connection. Simple, not fast.
//   - No simultaneous-close (CLOSING) state -- only "we close first" and
//     "peer closes first" are handled.
//   - No options, no MSS negotiation, no SACK, no window scaling.
//   - TCBs live in a flat array and are serviced every main-loop
//     iteration via tcp_tick_all(), independent of whether a client is
//     mid-call -- this is what makes retransmits and TIME_WAIT expiry
//     work for connections nobody is actively polling.
//   - Handles are just TCB array indices, no generation counter. Don't
//     hang onto a handle across CLOSE and expect stale use to reliably
//     fail if the slot gets reused quickly.
//


#include "libc.h"
#include "netd_ipc.h"

// ── Ethernet / ARP / IPv4 / ICMP / UDP / TCP wire structs ──────────────

#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV4 0x0800u
#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_OP_REQUEST     1u
#define ARP_OP_REPLY       2u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u
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

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint16_t length;    // header + payload, in bytes
    uint16_t checksum;
} udp_hdr_t;

// Biggest possible UDP frame we'll ever build/receive here, sized off
// NETD_UDP_MAX_PAYLOAD from netd_ipc.h.
#define UDP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + \
                        sizeof(udp_hdr_t) + NETD_UDP_MAX_PAYLOAD)

// TCP: no options, fixed 20-byte header (data_offset always 5 words).
typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;   // high nibble = header len in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_hdr_t;

#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u

#define TCP_HDR_WORDS 5   // 20 bytes / 4, no options
#define TCP_MSS       536 // conservative, no PMTU/MSS-option negotiation

// Biggest frame we ever build/receive for TCP, sized off
// NETD_TCP_MAX_PAYLOAD from netd_ipc.h.
#define TCP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + \
                        sizeof(tcp_hdr_t) + NETD_TCP_MAX_PAYLOAD)

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

// SLIRP-era defaults. Under passt these are almost certainly wrong --
// they're kept only as a fallback for when dhcp_client_run() can't reach
// a server (see md_main()), so netd still comes up on networks that
// don't run DHCP instead of refusing to start.
static uint8_t g_our_ip[4]     = {10, 0, 2, 15};
static uint8_t g_gateway_ip[4] = {10, 0, 2, 2};
static uint8_t g_subnet_mask[4] = {255, 255, 255, 0};
static int     g_dhcp_bound = 0; // 1 once dhcp_client_run() has succeeded at least once

#define NET_CMD_GET_MAC 4u

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// net0's write() already retries internally for a short bounded window
// when the driver reports the TX ring as transiently busy (EAGAIN), but
// this is a second, cheap line of defense at the transport layer: if that
// window was exceeded (e.g. the driver was mid-recovering a stuck
// descriptor), give the send a few more chances spread over up to
// NET_WRITE_RETRY_MS rather than failing the entire ping/DNS/TCP request
// on what may still just be a slow moment. Any non-EAGAIN error (real
// failure) is still returned immediately, unretried.
#define NET_WRITE_RETRY_MS 500

static ssize_t net_write_retry(const void *frame, size_t len) {
    uint64_t deadline = time_ms() + NET_WRITE_RETRY_MS;
    for (;;) {
        ssize_t r = write(g_net_fd, frame, len);
        if (r >= 0) return r;
        if (r != -11 /* EAGAIN */) return r;
        if (time_ms() >= deadline) return r;
        yield();
    }
}

static uint16_t checksum16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += get_be16(p); p += 2; len -= 2; }
    if (len == 1) sum += ((uint16_t)p[0]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

// Both UDP's and TCP's checksums run over a fake "pseudo-header"
// (src/dst IP, protocol, segment length) glued in front of the real
// header+payload -- it's what ties the checksum to the IP addresses
// even though they're outside the transport header itself. One struct,
// one builder, reused for both protocols (only `proto` differs).
typedef struct __attribute__((packed)) {
    uint8_t  src_ip[4], dst_ip[4];
    uint8_t  zero;
    uint8_t  proto;
    uint16_t seg_len;
} pseudo_hdr_t;

static uint16_t udp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                              const udp_hdr_t *udp, const void *payload, size_t payload_len) {
    uint8_t buf[sizeof(pseudo_hdr_t) + sizeof(udp_hdr_t) + NETD_UDP_MAX_PAYLOAD];
    pseudo_hdr_t ph;
    memcpy(ph.src_ip, src_ip, 4);
    memcpy(ph.dst_ip, dst_ip, 4);
    ph.zero = 0;
    ph.proto = IP_PROTO_UDP;
    put_be16((uint8_t *)&ph.seg_len, get_be16((const uint8_t *)&udp->length));

    size_t off = 0;
    memcpy(buf + off, &ph, sizeof(ph));  off += sizeof(ph);
    memcpy(buf + off, udp, sizeof(*udp)); off += sizeof(*udp);
    memcpy(buf + off, payload, payload_len); off += payload_len;

    uint16_t sum = checksum16(buf, off);
    // Per RFC 768, a computed checksum of exactly 0 is sent as all-ones
    // (0 in the field means "no checksum"); checksum16's ones-complement
    // math naturally can't produce 0 unless the input summed to 0xFFFF,
    // but keep the check explicit for clarity.
    return sum == 0 ? 0xFFFF : sum;
}

static uint16_t tcp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                              const tcp_hdr_t *tcp, const void *payload, size_t payload_len) {
    uint8_t buf[sizeof(pseudo_hdr_t) + sizeof(tcp_hdr_t) + NETD_TCP_MAX_PAYLOAD];
    pseudo_hdr_t ph;
    memcpy(ph.src_ip, src_ip, 4);
    memcpy(ph.dst_ip, dst_ip, 4);
    ph.zero = 0;
    ph.proto = IP_PROTO_TCP;
    put_be16((uint8_t *)&ph.seg_len, (uint16_t)(sizeof(tcp_hdr_t) + payload_len));

    size_t off = 0;
    memcpy(buf + off, &ph, sizeof(ph));   off += sizeof(ph);
    memcpy(buf + off, tcp, sizeof(*tcp)); off += sizeof(*tcp);
    memcpy(buf + off, payload, payload_len); off += payload_len;

    uint16_t sum = checksum16(buf, off);
    return sum == 0 ? 0xFFFF : sum;
}

static arp_entry_t *arp_cache_find(const uint8_t ip[4]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_arp_cache[i].used && ip_eq(g_arp_cache[i].ip, ip)) return &g_arp_cache[i];
    return NULL;
}

static int arp_resolve(const uint8_t target_ip[4], uint32_t timeout_ms, uint8_t out_mac[6]);

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

// ── TCP connection table ────────────────────────────────────────────────

#define MAX_TCP_CONNS      8
#define TCP_RECV_BUF_SIZE  4096
#define TCP_RETRANSMIT_MS  1000
#define TCP_MAX_RETRIES    5
#define TCP_TIME_WAIT_MS   4000

typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_conn_state_t;

typedef struct {
    int used;
    tcp_conn_state_t state;

    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;

    uint32_t snd_una;   // oldest unacked seq
    uint32_t snd_nxt;   // next seq we'll use
    uint32_t rcv_nxt;   // next seq we expect from peer

    // Retransmission (one outstanding segment at a time -- could be the
    // SYN, a data chunk, or the FIN; whichever we last sent and are
    // waiting to have acked).
    uint8_t  retx_frame[TCP_FRAME_MAX];
    size_t   retx_frame_len;
    int      retx_pending;
    uint64_t retx_deadline;
    int      retx_count;

    // Inbound data, FIFO. Stop-and-wait only constrains *our* sending;
    // we still accept and buffer whatever the peer sends us.
    uint8_t  recv_buf[TCP_RECV_BUF_SIZE];
    size_t   recv_len;
    int      fin_received;

    uint64_t time_wait_deadline;
} tcp_conn_t;

static tcp_conn_t g_tcp_conns[MAX_TCP_CONNS];
static uint16_t   g_next_ephemeral_udp_port = 49152;
static uint16_t   g_next_ephemeral_tcp_port = 49152; // separate counter from UDP's

static tcp_conn_t *tcp_find_by_index(int handle) {
    if (handle < 0 || handle >= MAX_TCP_CONNS) return NULL;
    if (!g_tcp_conns[handle].used) return NULL;
    return &g_tcp_conns[handle];
}

static tcp_conn_t *tcp_find_by_tuple(const uint8_t remote_ip[4], uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_tcp_conns[i];
        if (c->used && c->remote_port == remote_port && c->local_port == local_port
            && ip_eq(c->remote_ip, remote_ip)) return c;
    }
    return NULL;
}

static int tcp_alloc(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (!g_tcp_conns[i].used) {
            memset(&g_tcp_conns[i], 0, sizeof(g_tcp_conns[i]));
            g_tcp_conns[i].used = 1;
            return i;
        }
    }
    return -1;
}

static void tcp_free(tcp_conn_t *c) {
    memset(c, 0, sizeof(*c));
}

static void tcp_clear_retransmit(tcp_conn_t *c) {
    c->retx_pending = 0;
    c->retx_count = 0;
}

// Builds and sends one TCP segment. If arm_retransmit, stashes a copy of
// the frame as the connection's single outstanding retransmit candidate
// (stop-and-wait: only ever one at a time).
static int tcp_send_segment(tcp_conn_t *c, uint8_t flags, const void *payload, uint16_t payload_len,
                             int arm_retransmit) {
    uint8_t gw_mac[6];
    if (arp_resolve(g_gateway_ip, 2000, gw_mac) != 0) return -1;

    size_t tcp_len = sizeof(tcp_hdr_t) + payload_len;
    size_t ip_len = sizeof(ipv4_hdr_t) + tcp_len;
    size_t frame_len_raw = sizeof(eth_hdr_t) + ip_len;
    size_t frame_len = frame_len_raw < ETH_FRAME_MIN ? ETH_FRAME_MIN : frame_len_raw;

    uint8_t frame[TCP_FRAME_MAX];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst_mac, gw_mac, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)ip_len);
    put_be16((uint8_t *)&ip->id, (uint16_t)(time_ms() & 0xFFFF));
    ip->ttl = 64;
    ip->proto = IP_PROTO_TCP;
    memcpy(ip->src_ip, g_our_ip, 4);
    memcpy(ip->dst_ip, c->remote_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    tcp_hdr_t *tcp = (tcp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    put_be16((uint8_t *)&tcp->src_port, c->local_port);
    put_be16((uint8_t *)&tcp->dst_port, c->remote_port);
    put_be32((uint8_t *)&tcp->seq, c->snd_nxt);
    put_be32((uint8_t *)&tcp->ack, c->rcv_nxt);
    tcp->data_offset = (uint8_t)(TCP_HDR_WORDS << 4);
    tcp->flags = flags;
    put_be16((uint8_t *)&tcp->window, TCP_RECV_BUF_SIZE > 0xFFFF ? 0xFFFF : (uint16_t)TCP_RECV_BUF_SIZE);
    if (payload_len) memcpy((uint8_t *)tcp + sizeof(tcp_hdr_t), payload, payload_len);
    put_be16((uint8_t *)&tcp->checksum,
             tcp_checksum(g_our_ip, c->remote_ip, tcp, payload, payload_len));

    if (net_write_retry(frame, frame_len) < 0) return -1;

    if (arm_retransmit) {
        memcpy(c->retx_frame, frame, frame_len);
        c->retx_frame_len = frame_len;
        c->retx_pending = 1;
        c->retx_deadline = time_ms() + TCP_RETRANSMIT_MS;
        c->retx_count = 0;
    }

    return 0;
}

// Runs the state machine for one inbound TCP segment already matched to
// its TCB.
static void tcp_input(tcp_conn_t *c, const ipv4_hdr_t *ip, const tcp_hdr_t *tcp,
                       const uint8_t *payload, size_t payload_len) {
    uint32_t seq = get_be32((const uint8_t *)&tcp->seq);
    uint32_t ack = get_be32((const uint8_t *)&tcp->ack);
    uint8_t flags = tcp->flags;
    (void)ip;

    if (flags & TCP_FLAG_RST) {
        tcp_free(c);
        return;
    }

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            tcp_clear_retransmit(c);
            c->state = TCP_ESTABLISHED;
            tcp_send_segment(c, TCP_FLAG_ACK, NULL, 0, 0);
        }
        // A bare SYN (simultaneous open) isn't handled -- out of scope
        // for v2, same as the CLOSING state below.
        break;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if ((flags & TCP_FLAG_ACK) && ack == c->snd_nxt && c->retx_pending) {
            c->snd_una = ack;
            tcp_clear_retransmit(c);
            if (c->state == TCP_FIN_WAIT_1) c->state = TCP_FIN_WAIT_2;
        }

        if (payload_len > 0 && seq == c->rcv_nxt && c->state != TCP_FIN_WAIT_2) {
            size_t n = payload_len;
            if (c->recv_len + n > sizeof(c->recv_buf)) n = sizeof(c->recv_buf) - c->recv_len;
            if (n > 0) memcpy(c->recv_buf + c->recv_len, payload, n);
            c->recv_len += n;
            c->rcv_nxt += (uint32_t)payload_len; // ack the full segment even if our buffer truncated it
            tcp_send_segment(c, TCP_FLAG_ACK, NULL, 0, 0);
        } else if (payload_len > 0 && seq != c->rcv_nxt) {
            // Out of order / duplicate -- re-ACK what we actually have so
            // the peer knows where to resume. No reordering buffer.
            tcp_send_segment(c, TCP_FLAG_ACK, NULL, 0, 0);
        }

        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt += 1;
            c->fin_received = 1;
            tcp_send_segment(c, TCP_FLAG_ACK, NULL, 0, 0);
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
            } else if (c->state == TCP_FIN_WAIT_2) {
                c->state = TCP_TIME_WAIT;
                c->time_wait_deadline = time_ms() + TCP_TIME_WAIT_MS;
            }
            // FIN_WAIT_1 + FIN here = simultaneous close -- unhandled in
            // v2; the connection will likely stall and eventually get
            // reaped by the retransmit-exhaustion path in tcp_tick_all().
        }
        break;

    case TCP_LAST_ACK:
        if ((flags & TCP_FLAG_ACK) && ack == c->snd_nxt) {
            tcp_free(c); // fully closed
        }
        break;

    case TCP_CLOSE_WAIT:
        // We already ACKed the peer's FIN to get here; nothing more to
        // do until the client calls TCP_CLOSE.
        break;

    case TCP_TIME_WAIT:
        // Peer retransmitting its FIN because our ACK got lost -- re-ACK
        // it, don't restart the timer.
        if (flags & TCP_FLAG_FIN) tcp_send_segment(c, TCP_FLAG_ACK, NULL, 0, 0);
        break;

    default:
        break;
    }
}

// Runs every netd main-loop iteration, independent of whether a client
// is currently blocked on the mailbox -- this is what makes retransmits
// and TIME_WAIT expiry actually work for long-lived connections.
static void tcp_tick_all(void) {
    uint64_t now = time_ms();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_tcp_conns[i];
        if (!c->used) continue;

        if (c->state == TCP_TIME_WAIT && now >= c->time_wait_deadline) {
            tcp_free(c);
            continue;
        }

        if (c->retx_pending && now >= c->retx_deadline) {
            if (c->retx_count >= TCP_MAX_RETRIES) {
                tcp_free(c); // give up -- connection is dead
                continue;
            }
            write(g_net_fd, c->retx_frame, c->retx_frame_len);
            c->retx_count++;
            c->retx_deadline = now + TCP_RETRANSMIT_MS * (1u << c->retx_count); // simple backoff
        }
    }
}

// Drains net0's RX ring, feeding ARP replies into the cache, dispatching
// TCP segments into their TCB, and handing back any other IPv4 frame
// found (so callers can also look for ping/UDP replies in the same
// pass). Returns 1 if such a frame was copied into out_buf.
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

    if (et == ETHERTYPE_IPV4) {
        if ((size_t)r < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return 0;
        ipv4_hdr_t *rip = (ipv4_hdr_t *)(buf + sizeof(eth_hdr_t));

        if (rip->proto == IP_PROTO_TCP) {
            uint8_t ihl = (uint8_t)((rip->ver_ihl & 0x0F) * 4);
            if ((size_t)r >= sizeof(eth_hdr_t) + ihl + sizeof(tcp_hdr_t)) {
                tcp_hdr_t *rtcp = (tcp_hdr_t *)((uint8_t *)rip + ihl);
                uint8_t data_off_bytes = (uint8_t)((rtcp->data_offset >> 4) * 4);
                size_t hdrs_len = sizeof(eth_hdr_t) + ihl + data_off_bytes;
                uint8_t *payload = buf + hdrs_len;
                size_t payload_len = (size_t)r > hdrs_len ? (size_t)r - hdrs_len : 0;

                tcp_conn_t *c = tcp_find_by_tuple(rip->src_ip, get_be16((uint8_t *)&rtcp->src_port),
                                                   get_be16((uint8_t *)&rtcp->dst_port));
                if (c) tcp_input(c, rip, rtcp, payload, payload_len);
            }
            return 0; // handled as a side effect, same as ARP replies
        }

        if (out_buf) {
            size_t n = (size_t)r < out_cap ? (size_t)r : out_cap;
            memcpy(out_buf, buf, n);
            if (out_len) *out_len = (ssize_t)r;
            return 1;
        }
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

    if (net_write_retry(frame, sizeof(frame)) < 0) return -1;

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL); // feeds ARP cache as a side effect
        arp_entry_t *e = arp_cache_find(target_ip);
        if (e) { memcpy(out_mac, e->mac, 6); return 0; }
        yield();
    }
    return -1;
}

static uint16_t g_next_icmp_id = 0x1000;

// Return codes distinguish *why* a ping failed, so callers (and the
// message shown to the client) don't lump "couldn't even get the packet
// onto the wire" together with "sent fine, nobody answered" -- those
// used to be indistinguishable, which is exactly what made the stuck-TX
// bug so confusing to diagnose from the client side.
#define DO_PING_ERR_ARP     -1
#define DO_PING_ERR_TX      -2
#define DO_PING_ERR_TIMEOUT -3

static int do_ping(const uint8_t target_ip[4], uint32_t timeout_ms, uint32_t *out_rtt_ms) {
    uint8_t gw_mac[6];
    if (arp_resolve(g_gateway_ip, timeout_ms, gw_mac) != 0) return DO_PING_ERR_ARP;

    uint16_t icmp_id = g_next_icmp_id++;
    if (g_next_icmp_id == 0) g_next_icmp_id = 0x1000; // wrapped past 65535

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
    put_be16((uint8_t *)&icmp->id, icmp_id);
    put_be16((uint8_t *)&icmp->seq, 1);
    uint8_t *payload = (uint8_t *)icmp + sizeof(icmp_hdr_t);
    for (int i = 0; i < ICMP_PAYLOAD_LEN; i++) payload[i] = (uint8_t)('a' + (i % 23));
    put_be16((uint8_t *)&icmp->checksum, checksum16(icmp, sizeof(icmp_hdr_t) + ICMP_PAYLOAD_LEN));

    uint64_t t_start = time_ms();
    if (net_write_retry(frame, sizeof(frame)) < 0) return DO_PING_ERR_TX;

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
            if (get_be16((uint8_t *)&ricmp->id) != icmp_id) continue;
            *out_rtt_ms = (uint32_t)(time_ms() - t_start);
            return 0;
        }
        yield();
    }
    return DO_PING_ERR_TIMEOUT;
}

static uint16_t udp_alloc_ephemeral_port(void) {
    uint16_t port = g_next_ephemeral_udp_port++;
    if (g_next_ephemeral_udp_port == 0) g_next_ephemeral_udp_port = 49152; // wrapped past 65535
    return port;
}

// Sends one UDP datagram to target_ip:dst_port and waits up to
// timeout_ms for one reply datagram back from that same (ip, port) to
// our chosen src_port. *inout_src_port == 0 picks an ephemeral port
// (and reports back which one was used); a nonzero value is used as-is.
// On success, copies up to reply_cap bytes of the reply payload into
// out_reply and reports its real length via *out_reply_len, regardless
// of whether it was truncated to fit.
static int do_udp_send(const uint8_t target_ip[4], uint16_t dst_port,
                        uint16_t *inout_src_port,
                        const uint8_t *payload, uint16_t payload_len,
                        uint32_t timeout_ms,
                        uint8_t *out_reply, uint16_t reply_cap, uint16_t *out_reply_len,
                        uint32_t *out_rtt_ms) {
    if (payload_len > NETD_UDP_MAX_PAYLOAD) return -1;

    uint8_t gw_mac[6];
    if (arp_resolve(g_gateway_ip, timeout_ms, gw_mac) != 0) return -1;

    uint16_t src_port = *inout_src_port ? *inout_src_port : udp_alloc_ephemeral_port();
    *inout_src_port = src_port;

    size_t udp_len = sizeof(udp_hdr_t) + payload_len;
    size_t ip_len  = sizeof(ipv4_hdr_t) + udp_len;
    size_t frame_len_raw = sizeof(eth_hdr_t) + ip_len;
    size_t frame_len = frame_len_raw < ETH_FRAME_MIN ? ETH_FRAME_MIN : frame_len_raw;

    uint8_t frame[UDP_FRAME_MAX];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst_mac, gw_mac, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)ip_len);
    put_be16((uint8_t *)&ip->id, (uint16_t)(time_ms() & 0xFFFF));
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    memcpy(ip->src_ip, g_our_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    put_be16((uint8_t *)&udp->src_port, src_port);
    put_be16((uint8_t *)&udp->dst_port, dst_port);
    put_be16((uint8_t *)&udp->length, (uint16_t)udp_len);
    uint8_t *udp_payload = (uint8_t *)udp + sizeof(udp_hdr_t);
    memcpy(udp_payload, payload, payload_len);
    put_be16((uint8_t *)&udp->checksum,
             udp_checksum(g_our_ip, target_ip, udp, payload, payload_len));

    uint64_t t_start = time_ms();
    if (net_write_retry(frame, frame_len) < 0) return -1;

    uint64_t deadline = t_start + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t rxbuf[1600];
        ssize_t rxlen = 0;
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen)) {
            size_t min_len = sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t);
            if ((size_t)rxlen < min_len) continue;
            ipv4_hdr_t *rip = (ipv4_hdr_t *)(rxbuf + sizeof(eth_hdr_t));
            if (rip->proto != IP_PROTO_UDP || !ip_eq(rip->src_ip, target_ip)) continue;
            uint8_t ihl = (uint8_t)((rip->ver_ihl & 0x0F) * 4);
            udp_hdr_t *rudp = (udp_hdr_t *)((uint8_t *)rip + ihl);
            if (get_be16((uint8_t *)&rudp->src_port) != dst_port) continue;
            if (get_be16((uint8_t *)&rudp->dst_port) != src_port) continue;

            uint16_t rudp_len = get_be16((uint8_t *)&rudp->length);
            if (rudp_len < sizeof(udp_hdr_t)) continue;
            uint16_t rpayload_len = (uint16_t)(rudp_len - sizeof(udp_hdr_t));
            uint8_t *rpayload = (uint8_t *)rudp + sizeof(udp_hdr_t);

            uint16_t n = rpayload_len < reply_cap ? rpayload_len : reply_cap;
            if (out_reply && n) memcpy(out_reply, rpayload, n);
            if (out_reply_len) *out_reply_len = rpayload_len; // real length, even if truncated
            *out_rtt_ms = (uint32_t)(time_ms() - t_start);
            return 0;
        }
        yield();
    }
    return -1;
}

// ── DHCP client (v4) ─────────────────────────────────────────────────
//
// Minimal DISCOVER -> OFFER -> REQUEST -> ACK client. No DECLINE, no
// INFORM, no proper RENEWING/REBINDING state machine -- when the lease
// is due, dhcp_tick() just reruns the whole four-way exchange from
// scratch (see its comment). That's wasteful compared to a real client
// but this network only ever has one host on it (the VM's gateway), so
// there's nothing to be polite to.
//
// Sends are always broadcast, both because we don't have a gateway MAC
// to ARP for before we're bound, and because a bare broadcast is what
// every DHCP server expects to see regardless. Receiving is done by
// reusing net_poll_once() (it already hands back any non-TCP IPv4 frame
// verbatim) and filtering for UDP port 67->68 with a matching xid --
// that works whether the server unicasts or broadcasts its reply, since
// net0 delivers frames addressed to our MAC either way; we don't need an
// IP configured to receive them.

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363u

#define DHCP_OP_BOOTREQUEST 1u
#define DHCP_OP_BOOTREPLY   2u
#define DHCP_HTYPE_ETHERNET 1u

#define DHCP_OPT_PAD           0u
#define DHCP_OPT_SUBNET_MASK   1u
#define DHCP_OPT_ROUTER        3u
#define DHCP_OPT_REQUESTED_IP  50u
#define DHCP_OPT_LEASE_TIME    51u
#define DHCP_OPT_MSG_TYPE      53u
#define DHCP_OPT_SERVER_ID     54u
#define DHCP_OPT_PARAM_REQUEST 55u
#define DHCP_OPT_END           255u

#define DHCP_DISCOVER 1u
#define DHCP_OFFER    2u
#define DHCP_REQUEST  3u
#define DHCP_ACK      5u
#define DHCP_NAK      6u

// BOOTP fixed part (236 bytes) + 4-byte magic cookie + options. Options
// sized to match the classic 300-byte-total BOOTP minimum some servers
// still assume, with headroom for the handful of options we request.
#define DHCP_OPTIONS_LEN 312

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[DHCP_OPTIONS_LEN];
} dhcp_pkt_t;

// Everything in dhcp_pkt_t up to (not including) the options array --
// used both as the minimum receive length and to size the checksum
// buffer. Computed this way instead of offsetof() purely out of
// caution about what this libc's headers provide.
#define DHCP_FIXED_LEN (sizeof(dhcp_pkt_t) - DHCP_OPTIONS_LEN)

#define DHCP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + \
                         sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t))

static uint32_t g_dhcp_lease_secs;
static uint64_t g_dhcp_lease_deadline; // time_ms() at which dhcp_tick() should renew

// UDP checksum over a DHCP packet. Can't reuse udp_checksum() above --
// its scratch buffer is sized for NETD_UDP_MAX_PAYLOAD (512B) and a
// dhcp_pkt_t is ~550B, so that would overflow a stack buffer. Same
// pseudo-header math, just sized for this one payload type.
static uint16_t dhcp_udp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                                   const udp_hdr_t *udp, const dhcp_pkt_t *pkt) {
    uint8_t buf[sizeof(pseudo_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t)];
    pseudo_hdr_t ph;
    memcpy(ph.src_ip, src_ip, 4);
    memcpy(ph.dst_ip, dst_ip, 4);
    ph.zero = 0;
    ph.proto = IP_PROTO_UDP;
    put_be16((uint8_t *)&ph.seg_len, get_be16((const uint8_t *)&udp->length));

    size_t off = 0;
    memcpy(buf + off, &ph, sizeof(ph));    off += sizeof(ph);
    memcpy(buf + off, udp, sizeof(*udp));  off += sizeof(*udp);
    memcpy(buf + off, pkt, sizeof(*pkt));  off += sizeof(*pkt);

    uint16_t sum = checksum16(buf, off);
    return sum == 0 ? 0xFFFF : sum;
}

// Fills in the fixed BOOTP fields + magic cookie shared by every message
// we send; caller fills in options afterward. ciaddr (and, via
// dhcp_broadcast(), the IP header's source) is 0.0.0.0 until we actually
// hold a lease -- per RFC 2131 that's mandatory for DISCOVER, and a
// DISCOVER/REQUEST that claims a ciaddr the server never handed out
// looks like a bogus renewal, which is exactly what was happening here:
// g_our_ip still held the SLIRP-style static fallback the first time
// this ran, so every DISCOVER went out with ciaddr=10.0.2.15 and passt
// (reasonably) just ignored it instead of answering. Only dhcp_tick()'s
// genuine renewals, once g_dhcp_bound is set, get to claim ciaddr.
static void dhcp_pkt_init(dhcp_pkt_t *p, uint32_t xid) {
    memset(p, 0, sizeof(*p));
    p->op = DHCP_OP_BOOTREQUEST;
    p->htype = DHCP_HTYPE_ETHERNET;
    p->hlen = 6;
    put_be32((uint8_t *)&p->xid, xid);
    put_be16((uint8_t *)&p->flags, 0x8000); // ask for a broadcast reply; costs us nothing either way
    if (g_dhcp_bound) memcpy(p->ciaddr, g_our_ip, 4); // else leave zeroed (0.0.0.0)
    memcpy(p->chaddr, g_our_mac, 6);
    put_be32((uint8_t *)&p->magic_cookie, DHCP_MAGIC_COOKIE);
}

// Appends one TLV option, returns the new write offset.
static size_t dhcp_put_opt(uint8_t *opts, size_t off, uint8_t code, uint8_t len, const void *data) {
    opts[off++] = code;
    opts[off++] = len;
    if (len) memcpy(opts + off, data, len);
    return off + len;
}

// Finds option `code` in a received packet. Doesn't handle the RFC 1533
// "option overload" case (options spilling into sname/file) -- none of
// dnsmasq/passt/isc-dhcpd need it for the handful of options we ask for.
static const uint8_t *dhcp_find_opt(const dhcp_pkt_t *p, uint8_t code, uint8_t *out_len) {
    size_t i = 0;
    while (i + 1 < sizeof(p->options)) {
        uint8_t c = p->options[i];
        if (c == DHCP_OPT_END) break;
        if (c == DHCP_OPT_PAD) { i++; continue; }
        uint8_t len = p->options[i + 1];
        if (i + 2 + (size_t)len > sizeof(p->options)) break;
        if (c == code) { if (out_len) *out_len = len; return &p->options[i + 2]; }
        i += 2 + len;
    }
    return NULL;
}

// Broadcasts one already-built DHCP message.
static int dhcp_broadcast(const dhcp_pkt_t *pkt) {
    uint8_t frame[DHCP_FRAME_MAX];
    memset(frame, 0, sizeof(frame));

    size_t udp_len = sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t);
    size_t ip_len  = sizeof(ipv4_hdr_t) + udp_len;
    size_t frame_len_raw = sizeof(eth_hdr_t) + ip_len;
    size_t frame_len = frame_len_raw < ETH_FRAME_MIN ? ETH_FRAME_MIN : frame_len_raw;

    static const uint8_t bcast_ip[4] = {255, 255, 255, 255};

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)ip_len);
    put_be16((uint8_t *)&ip->id, (uint16_t)(time_ms() & 0xFFFF));
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    memcpy(ip->src_ip, pkt->ciaddr, 4);
    memcpy(ip->dst_ip, bcast_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    put_be16((uint8_t *)&udp->src_port, DHCP_CLIENT_PORT);
    put_be16((uint8_t *)&udp->dst_port, DHCP_SERVER_PORT);
    put_be16((uint8_t *)&udp->length, (uint16_t)udp_len);
    memcpy((uint8_t *)udp + sizeof(udp_hdr_t), pkt, sizeof(*pkt));
    put_be16((uint8_t *)&udp->checksum, dhcp_udp_checksum(pkt->ciaddr, bcast_ip, udp, pkt));

    return net_write_retry(frame, frame_len) < 0 ? -1 : 0;
}

// Waits for one reply matching xid. expect_type == 0 means "any BOOTREPLY",
// used for the REQUEST step where either ACK or NAK is a valid answer and
// the caller decides which happened. Copies the (zero-padded-if-short)
// packet into *out on success.
static int dhcp_wait_reply(uint32_t xid, uint8_t expect_type, uint32_t timeout_ms, dhcp_pkt_t *out) {
    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t rxbuf[1600];
        ssize_t rxlen = 0;
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen)) {
            size_t min_len = sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + DHCP_FIXED_LEN;
            if ((size_t)rxlen < min_len) continue;
            ipv4_hdr_t *rip = (ipv4_hdr_t *)(rxbuf + sizeof(eth_hdr_t));
            if (rip->proto != IP_PROTO_UDP) continue;
            uint8_t ihl = (uint8_t)((rip->ver_ihl & 0x0F) * 4);
            udp_hdr_t *rudp = (udp_hdr_t *)((uint8_t *)rip + ihl);
            if (get_be16((uint8_t *)&rudp->src_port) != DHCP_SERVER_PORT) continue;
            if (get_be16((uint8_t *)&rudp->dst_port) != DHCP_CLIENT_PORT) continue;

            dhcp_pkt_t *rpkt = (dhcp_pkt_t *)((uint8_t *)rudp + sizeof(udp_hdr_t));
            size_t hdrs_len = sizeof(eth_hdr_t) + ihl + sizeof(udp_hdr_t);
            size_t avail = (size_t)rxlen > hdrs_len ? (size_t)rxlen - hdrs_len : 0;
            size_t copy_len = avail < sizeof(dhcp_pkt_t) ? avail : sizeof(dhcp_pkt_t);
            if (copy_len < DHCP_FIXED_LEN) continue;
            if (get_be32((uint8_t *)&rpkt->xid) != xid) continue;
            if (rpkt->op != DHCP_OP_BOOTREPLY) continue;
            if (get_be32((uint8_t *)&rpkt->magic_cookie) != DHCP_MAGIC_COOKIE) continue;

            memset(out, 0, sizeof(*out));
            memcpy(out, rpkt, copy_len);

            uint8_t opt_len = 0;
            const uint8_t *mt = dhcp_find_opt(out, DHCP_OPT_MSG_TYPE, &opt_len);
            if (!mt || opt_len != 1) continue;
            if (expect_type && *mt != expect_type) continue;

            return 0;
        }
        yield();
    }
    return -1;
}

#define DHCP_DISCOVER_RETRIES    4
#define DHCP_DISCOVER_TIMEOUT_MS 2000
#define DHCP_REQUEST_RETRIES     4
#define DHCP_REQUEST_TIMEOUT_MS  2000

static int dhcp_discover_offer(uint32_t xid, dhcp_pkt_t *offer_out) {
    for (int attempt = 0; attempt < DHCP_DISCOVER_RETRIES; attempt++) {
        dhcp_pkt_t pkt;
        dhcp_pkt_init(&pkt, xid);
        size_t off = 0;
        uint8_t mt = DHCP_DISCOVER;
        off = dhcp_put_opt(pkt.options, off, DHCP_OPT_MSG_TYPE, 1, &mt);
        static const uint8_t params[] = { DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_LEASE_TIME };
        off = dhcp_put_opt(pkt.options, off, DHCP_OPT_PARAM_REQUEST, sizeof(params), params);
        pkt.options[off++] = DHCP_OPT_END;

        if (dhcp_broadcast(&pkt) != 0) continue;
        if (dhcp_wait_reply(xid, DHCP_OFFER, DHCP_DISCOVER_TIMEOUT_MS, offer_out) == 0) return 0;
    }
    return -1;
}

// Returns 0 on ACK, -1 on NAK or timeout/no-answer.
static int dhcp_request_ack(uint32_t xid, const dhcp_pkt_t *offer, dhcp_pkt_t *ack_out) {
    uint8_t server_id_len = 0;
    const uint8_t *server_id = dhcp_find_opt(offer, DHCP_OPT_SERVER_ID, &server_id_len);
    if (!server_id || server_id_len != 4) return -1;

    for (int attempt = 0; attempt < DHCP_REQUEST_RETRIES; attempt++) {
        dhcp_pkt_t pkt;
        dhcp_pkt_init(&pkt, xid);
        size_t off = 0;
        uint8_t mt = DHCP_REQUEST;
        off = dhcp_put_opt(pkt.options, off, DHCP_OPT_MSG_TYPE, 1, &mt);
        off = dhcp_put_opt(pkt.options, off, DHCP_OPT_REQUESTED_IP, 4, offer->yiaddr);
        off = dhcp_put_opt(pkt.options, off, DHCP_OPT_SERVER_ID, 4, server_id);
        pkt.options[off++] = DHCP_OPT_END;

        if (dhcp_broadcast(&pkt) != 0) continue;
        if (dhcp_wait_reply(xid, 0, DHCP_REQUEST_TIMEOUT_MS, ack_out) != 0) continue;

        uint8_t opt_len = 0;
        const uint8_t *mt2 = dhcp_find_opt(ack_out, DHCP_OPT_MSG_TYPE, &opt_len);
        if (mt2 && opt_len == 1 && *mt2 == DHCP_ACK) return 0;
        if (mt2 && opt_len == 1 && *mt2 == DHCP_NAK) return -1; // server said no -- no point retrying this offer
    }
    return -1;
}

// Runs the full DISCOVER/OFFER/REQUEST/ACK exchange. On success, updates
// g_our_ip/g_gateway_ip/g_subnet_mask and arms the lease-renewal
// deadline. On failure, leaves all of those untouched -- callers (both
// md_main() and dhcp_tick()) rely on that to fall back to whatever was
// there before (the SLIRP-style static default on first boot, or the
// still-technically-valid old lease on a failed renewal).
static int dhcp_client_run(void) {
    uint32_t xid = (uint32_t)time_ms() ^ ((uint32_t)g_our_mac[4] << 8) ^ g_our_mac[5];

    dhcp_pkt_t offer, ack;
    if (dhcp_discover_offer(xid, &offer) != 0) return -1;
    if (dhcp_request_ack(xid, &offer, &ack) != 0) return -1;

    memcpy(g_our_ip, ack.yiaddr, 4);

    uint8_t opt_len = 0;
    const uint8_t *mask = dhcp_find_opt(&ack, DHCP_OPT_SUBNET_MASK, &opt_len);
    if (mask && opt_len == 4) memcpy(g_subnet_mask, mask, 4);

    const uint8_t *router = dhcp_find_opt(&ack, DHCP_OPT_ROUTER, &opt_len);
    if (router && opt_len >= 4) memcpy(g_gateway_ip, router, 4); // first router if server sent a list

    const uint8_t *lease = dhcp_find_opt(&ack, DHCP_OPT_LEASE_TIME, &opt_len);
    g_dhcp_lease_secs = (lease && opt_len == 4) ? get_be32(lease) : 3600; // sane default if server omits it

    // Renew at the halfway point -- the same rule of thumb dhclient/
    // systemd-networkd use, minus their separate T2/rebind phase.
    g_dhcp_lease_deadline = time_ms() + (uint64_t)g_dhcp_lease_secs * 1000ull / 2;
    g_dhcp_bound = 1;

    // Any cached ARP entries are for whatever network we were on before
    // (matters on renewal, if the lease moved us to a different subnet).
    memset(g_arp_cache, 0, sizeof(g_arp_cache));

    printf("netd: dhcp bound ip=%d.%d.%d.%d gw=%d.%d.%d.%d mask=%d.%d.%d.%d lease=%ds\n",
           g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3],
           g_gateway_ip[0], g_gateway_ip[1], g_gateway_ip[2], g_gateway_ip[3],
           g_subnet_mask[0], g_subnet_mask[1], g_subnet_mask[2], g_subnet_mask[3],
           (int)g_dhcp_lease_secs);
    return 0;
}

// Runs every main-loop iteration, same spirit as tcp_tick_all(). Once
// bound and past the renewal point, just reruns the whole exchange --
// on failure the old lease is left in place (see dhcp_client_run()'s
// comment) and we try again next tick rather than blocking the loop.
static void dhcp_tick(void) {
    if (!g_dhcp_bound) return;
    if (time_ms() < g_dhcp_lease_deadline) return;
    if (dhcp_client_run() != 0) {
        g_dhcp_lease_deadline = time_ms() + 30000; // back off 30s before retrying
    }
}

// ── TCP application-facing calls (blocking; netd's own poll+tick loop
//    drives progress while we wait) ──────────────────────────────────────

static int tcp_open(const uint8_t target_ip[4], uint16_t dst_port, uint32_t timeout_ms) {
    int idx = tcp_alloc();
    if (idx < 0) return -1;
    tcp_conn_t *c = &g_tcp_conns[idx];

    memcpy(c->remote_ip, target_ip, 4);
    c->remote_port = dst_port;
    c->local_port = g_next_ephemeral_tcp_port++;
    if (g_next_ephemeral_tcp_port == 0) g_next_ephemeral_tcp_port = 49152;

    uint32_t iss = (uint32_t)time_ms() * 1000u + (uint32_t)idx; // distinct-ish, not cryptographic
    c->snd_una = iss;
    c->snd_nxt = iss; // SYN goes out at seq=iss
    c->state = TCP_SYN_SENT;

    if (tcp_send_segment(c, TCP_FLAG_SYN, NULL, 0, 1) != 0) { tcp_free(c); return -1; }
    c->snd_nxt = iss + 1; // SYN consumes one sequence number

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL);
        tcp_tick_all();
        if (!g_tcp_conns[idx].used) return -1; // RST'd or retransmit-exhausted
        if (g_tcp_conns[idx].state == TCP_ESTABLISHED) return idx;
        yield();
    }
    tcp_free(&g_tcp_conns[idx]);
    return -1;
}

static int tcp_app_send(int handle, const uint8_t *data, uint16_t len, uint32_t timeout_ms) {
    tcp_conn_t *c = tcp_find_by_index(handle);
    if (!c || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)) return -1;
    if (len > TCP_MSS) len = TCP_MSS; // caller chunks anything bigger, see http_client.c

    uint32_t seq_before = c->snd_nxt;
    if (tcp_send_segment(c, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len, 1) != 0) return -1;
    c->snd_nxt = seq_before + len;

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL);
        tcp_tick_all();
        c = tcp_find_by_index(handle); // may have been freed by a RST/exhaustion
        if (!c) return -1;
        if (c->snd_una == c->snd_nxt) return (int)len; // fully acked
        yield();
    }
    return -1;
}

static int tcp_app_recv(int handle, uint8_t *out, uint16_t cap, uint32_t timeout_ms,
                         uint16_t *out_len, uint8_t *out_eof) {
    tcp_conn_t *c = tcp_find_by_index(handle);
    if (!c) return -1;
    *out_eof = 0;

    uint64_t deadline = time_ms() + timeout_ms;
    while (c->recv_len == 0 && !c->fin_received && time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL);
        tcp_tick_all();
        c = tcp_find_by_index(handle);
        if (!c) return -1;
        yield();
    }

    uint16_t n = (uint16_t)(c->recv_len < cap ? c->recv_len : cap);
    if (n > 0) {
        memcpy(out, c->recv_buf, n);
        memmove(c->recv_buf, c->recv_buf + n, c->recv_len - n);
        c->recv_len -= n;
    }
    *out_len = n;
    *out_eof = (uint8_t)(n == 0 && c->fin_received);
    return 0;
}

static int tcp_app_close(int handle) {
    tcp_conn_t *c = tcp_find_by_index(handle);
    if (!c) return 0; // already gone, treat as success

    if (c->state == TCP_ESTABLISHED) {
        uint32_t seq_before = c->snd_nxt;
        tcp_send_segment(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0, 1);
        c->snd_nxt = seq_before + 1;
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        uint32_t seq_before = c->snd_nxt;
        tcp_send_segment(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0, 1);
        c->snd_nxt = seq_before + 1;
        c->state = TCP_LAST_ACK;
    }
    // Fire-and-forget from the caller's point of view -- tcp_tick_all()
    // drives the rest (retransmitting the FIN, TIME_WAIT, eventual free).
    return 0;
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
            int pr = do_ping(req->target_ip, timeout, &rtt);
            if (pr == 0) {
                resp->success = 1;
                resp->rtt_ms = rtt;
            } else {
                resp->success = 0;
                const char *why = (pr == DO_PING_ERR_ARP)     ? "could not resolve gateway MAC" :
                                   (pr == DO_PING_ERR_TX)      ? "failed to transmit (net0 busy)" :
                                                                  "ping timed out (no reply)";
                snprintf(resp->message, sizeof(resp->message), "%s", why);
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
            memcpy(resp->netmask, g_subnet_mask, 4);
            resp->link_up = 1;
            resp->dhcp_bound = (uint8_t)g_dhcp_bound;
            break;
        }

        case NETD_CMD_UDP_SEND: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 3000;
            uint16_t src_port = req->udp_src_port; // 0 => netd picks one
            uint32_t rtt = 0;
            uint16_t reply_len = 0;
            if (do_udp_send(req->target_ip, req->udp_dst_port, &src_port,
                             req->udp_payload, req->udp_payload_len, timeout,
                             resp->udp_reply, sizeof(resp->udp_reply), &reply_len,
                             &rtt) == 0) {
                resp->success = 1;
                resp->rtt_ms = rtt;
                resp->udp_reply_len = reply_len;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "udp send/recv timed out");
            }
            break;
        }

        case NETD_CMD_TCP_OPEN: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            int handle = tcp_open(req->target_ip, req->tcp_dst_port, timeout);
            if (handle >= 0) {
                resp->success = 1;
                resp->tcp_handle = handle;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "tcp connect failed/timed out");
            }
            break;
        }

        case NETD_CMD_TCP_SEND: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            int r = tcp_app_send(req->tcp_handle, req->tcp_payload, req->tcp_payload_len, timeout);
            if (r >= 0) {
                resp->success = 1;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "tcp send failed/timed out");
            }
            break;
        }

        case NETD_CMD_TCP_RECV: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            uint16_t cap = req->tcp_recv_cap ? req->tcp_recv_cap : NETD_TCP_MAX_PAYLOAD;
            if (cap > NETD_TCP_MAX_PAYLOAD) cap = NETD_TCP_MAX_PAYLOAD;
            uint16_t got = 0; uint8_t eof = 0;
            if (tcp_app_recv(req->tcp_handle, resp->tcp_payload, cap, timeout, &got, &eof) == 0) {
                resp->success = 1;
                resp->tcp_payload_len = got;
                resp->tcp_eof = eof;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "tcp recv: bad handle");
            }
            break;
        }

        case NETD_CMD_TCP_CLOSE: {
            tcp_app_close(req->tcp_handle);
            resp->success = 1;
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
    memset(g_tcp_conns, 0, sizeof(g_tcp_conns));

    printf("netd: requesting address via DHCP...\n");
    if (dhcp_client_run() != 0) {
        printf("netd: dhcp got no answer, falling back to static ip=%d.%d.%d.%d gw=%d.%d.%d.%d\n",
               g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3],
               g_gateway_ip[0], g_gateway_ip[1], g_gateway_ip[2], g_gateway_ip[3]);
    }

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

    // Single loop does quadruple duty:
    //  - drains net0 opportunistically so ARP replies get cached, TCP
    //    segments get dispatched into their TCB, and the RX ring never
    //    backs up, even with no client waiting on anything;
    //  - ticks every open TCP connection (retransmits, TIME_WAIT expiry)
    //    regardless of whether a client is mid-call;
    //  - checks whether the DHCP lease needs renewing;
    //  - services at most one pending mailbox request per pass. Request
    //    handling for PING/RESOLVE_ARP/UDP_SEND is a blocking spin loop
    //    same as netd v2; TCP_OPEN/SEND/RECV block the same way but poll
    //    +tick exactly like this outer loop does, so a slow TCP call
    //    doesn't starve ARP/TCP servicing for other connections.
    while (1) {
        net_poll_once(NULL, 0, NULL);
        tcp_tick_all();
        dhcp_tick();

        if (netd_ipc_get_state(shm) == NETD_SLOT_REQUEST_PENDING) {
            netd_process_request(&shm->req, &shm->resp);
            netd_ipc_set_state(shm, NETD_SLOT_RESPONSE_READY);
        }

        yield();
    }

    return 0;
}