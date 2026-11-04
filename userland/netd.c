// netd.c — network daemon: owns net0 exclusively and exposes ARP resolution,
// ping, one-shot UDP send/receive, a DHCP client, and TCP connections to
// clients through a SHM mailbox (see netd_ipc.h).
//
// Structure:
//   - One cooperative loop (md_main) drains net0, ticks TCP timers, ticks
//     the DHCP lease, and services at most one mailbox request per pass.
//   - Client calls that wait (PING, UDP_SEND, TCP_*) spin on the same
//     drain+tick pair, so other connections keep making progress while a
//     call blocks. The one exception is a DHCP renewal (dhcp_tick), which
//     blocks for up to several seconds and delays TCP timers meanwhile.
//   - All outbound IPv4 traffic is sent to the gateway's MAC; there is no
//     on-link ARP for other hosts.
//
// DHCP: dhcp_client_run() runs once at startup, before the mailbox exists.
// g_our_ip/g_gateway_ip/g_subnet_mask start as a static 10.0.2.x fallback
// that is used only if no DHCP server answers.
//
// TCP (active and passive open -- listen/accept, SYN_RCVD):
//   - Sliding window: data waits in a per-connection send buffer until
//     ACKed; the sender honours the peer's window and a congestion window
//     (slow start, congestion avoidance, fast retransmit, NewReno-style
//     recovery).
//   - RTT is measured (Karn's rule) and the retransmit timeout is computed
//     Jacobson/Karels-style, with exponential backoff.
//   - MSS option is sent on SYN and parsed from SYN-ACK. No SACK, window
//     scaling or timestamps.
//   - Receive side accepts only in-order data (out-of-order segments get a
//     duplicate ACK and the peer retransmits). The advertised window is the
//     free space in the receive buffer, with receiver-side silly-window
//     avoidance.
//   - Zero-window probing reuses the retransmit timer.
//   - States: everything in RFC 793 except LISTEN and SYN_RECEIVED, plus
//     TCP_DEAD (reset/timed out, kept until the client closes the handle so
//     it can learn what happened). Simultaneous close (CLOSING) is
//     handled; simultaneous open is not.
//   - TIME_WAIT is much shorter than the RFC's 2*MSL, to keep the small
//     connection table from filling up.
//   - Handles are (generation << 8) | table index. The generation changes
//     every time a slot is allocated, and a handle stops resolving as soon
//     as the client has closed it.
//   - Initial sequence numbers are clock-based, not cryptographically random.

#include "libc/libc.h"
#include "netd_ipc.h"

// ── Wire structs ────────────────────────────────────────────────────────

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
#define IP_FLAG_DF 0x4000u
#define DHCP_OPT_DNS           6u

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

#define UDP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + \
                        sizeof(udp_hdr_t) + NETD_UDP_MAX_PAYLOAD)

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;   // high nibble = header length in 32-bit words
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

#define TCP_OPT_MSS 2u

// Accept TCP segments only if their checksum verifies. Set to 0 if the NIC
// driver ever delivers frames whose checksums were left to be filled in
// later (checksum offload).
#define TCP_VERIFY_RX_CHECKSUM 1

// ── Byte order, checksums ───────────────────────────────────────────────

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

static uint32_t u32_min(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t u32_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

// Ones-complement sum accumulator. Only the final chunk of a checksum may
// have odd length.
static uint32_t csum_acc(uint32_t sum, const uint8_t *p, size_t len) {
    while (len > 1) { sum += get_be16(p); p += 2; len -= 2; }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static uint16_t checksum16(const void *data, size_t len) {
    return csum_fold(csum_acc(0, (const uint8_t *)data, len));
}

// UDP/TCP checksum: ones-complement over a pseudo-header (src IP, dst IP,
// protocol, segment length) followed by the whole transport segment
// (header + payload, checksum field zero when generating). Run over a
// received segment with its checksum field intact, a valid segment
// yields 0.
static uint16_t l4_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4], uint8_t proto,
                             const void *seg, size_t seg_len) {
    uint32_t sum = 0;
    sum += get_be16(src_ip); sum += get_be16(src_ip + 2);
    sum += get_be16(dst_ip); sum += get_be16(dst_ip + 2);
    sum += proto;
    sum += (uint16_t)seg_len;
    sum = csum_acc(sum, (const uint8_t *)seg, seg_len);
    return csum_fold(sum);
}

// RFC 768: an all-zero UDP checksum field means "no checksum", so a
// computed value of 0 is transmitted as 0xFFFF.
static uint16_t udp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                              const udp_hdr_t *udp, size_t udp_len) {
    uint16_t sum = l4_checksum(src_ip, dst_ip, IP_PROTO_UDP, udp, udp_len);
    return sum == 0 ? 0xFFFF : sum;
}

// ── Interface state ─────────────────────────────────────────────────────

#define ARP_CACHE_SIZE 16
typedef struct {
    int used;
    uint8_t ip[4];
    uint8_t mac[6];
} arp_entry_t;

static arp_entry_t g_arp_cache[ARP_CACHE_SIZE];
static int g_net_fd = -1;
static uint8_t g_our_mac[6];

// Static fallback, used only if dhcp_client_run() gets no answer at startup.
static uint8_t g_our_ip[4]     = {10, 0, 2, 15};
static uint8_t g_gateway_ip[4] = {10, 0, 2, 2};
static uint8_t g_subnet_mask[4] = {255, 255, 255, 0};
static uint8_t g_dns_ip[4]     = {10, 0, 2, 3};
static int     g_dhcp_bound = 0; // 1 once dhcp_client_run() has succeeded at least once

// Nonzero while an inbound frame is being dispatched. Code running in that
// context must not poll the NIC (see arp_resolve()).
static int g_rx_dispatch = 0;

#define NET_CMD_GET_MAC 4u

// net0's write() may report EAGAIN when the TX ring is momentarily full.
// Retry for up to NET_WRITE_RETRY_MS before giving up; any other error is
// returned immediately.
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
        if (!e) e = &g_arp_cache[0]; // full: overwrite slot 0
    }
    e->used = 1;
    memcpy(e->ip, ip, 4);
    memcpy(e->mac, mac, 6);
}

// ── TCP: connection table ───────────────────────────────────────────────

#define MAX_TCP_CONNS       8
#define TCP_SEND_BUF_SIZE   4096
#define TCP_RECV_BUF_SIZE   4096

#define TCP_LOCAL_MSS        1460  // Ethernet MTU 1500 - 20 (IP) - 20 (TCP)
#define TCP_DEFAULT_PEER_MSS 536   // RFC 1122 default when the peer sends no MSS option
#define TCP_MAX_SEG_PAYLOAD  TCP_LOCAL_MSS
#define TCP_OPT_MAX          4     // only the MSS option is ever sent

#define TCP_INIT_RTO_MS      1000
#define TCP_MIN_RTO_MS       200
#define TCP_MAX_RTO_MS       16000
#define TCP_MAX_RETRIES      8     // consecutive timeouts on data before the connection is dropped
#define TCP_SYN_RETRIES      5
#define TCP_TIME_WAIT_MS     4000
#define TCP_FIN_WAIT2_MS     30000

#define TCP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t) + \
                        TCP_OPT_MAX + TCP_MAX_SEG_PAYLOAD)

// Error codes returned (negated-style) by the application-facing calls.
#define TCP_E_BADHANDLE (-1)
#define TCP_E_RESET     (-2)
#define TCP_E_TIMEOUT   (-3)
#define TCP_E_REFUSED   (-4)

typedef enum {
    TCP_CLOSED = 0,      // free slot
    TCP_SYN_SENT,
    TCP_SYN_RCVD,        // passive open: SYN-ACK sent, waiting for the final ACK
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
    TCP_DEAD,            // reset or timed out; waiting for the client to close the handle
} tcp_conn_state_t;

typedef struct {
    int used;
    int app_closed;      // client called TCP_CLOSE; the stack owns the slot from here on
    tcp_conn_state_t state;
    int error;           // TCP_E_* when state == TCP_DEAD
    int listener;        // 1-based listener index until accept()ed, else 0
    int accept_ready;    // handshake complete, not yet returned by ACCEPT

    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;

    // Send sequence space
    uint32_t iss;
    uint32_t snd_una;    // oldest unacknowledged sequence number
    uint32_t snd_nxt;    // next sequence number to send (rewound on timeout)
    uint32_t snd_max;    // highest sequence number ever sent
    uint32_t snd_wnd;    // peer's advertised window
    uint32_t snd_wl1;    // seq of the segment that last updated snd_wnd
    uint32_t snd_wl2;    // ack of the segment that last updated snd_wnd
    uint32_t peer_mss;

    // Receive sequence space
    uint32_t irs;
    uint32_t rcv_nxt;
    uint16_t last_adv;   // window value in the most recent segment we sent

    // Congestion control
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t recover;    // snd_max when fast recovery began
    int      in_recovery;
    int      dupacks;

    // RTT estimation / retransmit timer
    uint32_t srtt, rttvar, rto;
    int      rtt_active;
    uint32_t rtt_seq;
    uint64_t rtt_start;
    int      rto_armed;
    uint64_t rto_deadline;
    int      retx_count;

    // Send buffer: sbuf[0] is the byte at sequence number snd_una. It holds
    // both sent-but-unacked and not-yet-sent bytes. A FIN occupies one
    // sequence number after the last data byte but is not stored here.
    uint8_t  sbuf[TCP_SEND_BUF_SIZE];
    size_t   sbuf_len;
    int      fin_queued;     // client closed; send FIN after all buffered data
    int      fin_sent;       // snd_nxt currently covers our FIN
    int      fin_ever_sent;
    uint32_t fin_seq;        // sequence number of our FIN once sent

    // Receive buffer (in-order bytes not yet read by the client)
    uint8_t  recv_buf[TCP_RECV_BUF_SIZE];
    size_t   recv_len;
    int      fin_received;

    uint64_t time_wait_deadline; // also used as the FIN_WAIT_2 timeout
} tcp_conn_t;

#define SEQ_LT(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) >= 0)

static tcp_conn_t g_tcp_conns[MAX_TCP_CONNS];
static uint32_t   g_tcp_gen[MAX_TCP_CONNS];  // survives tcp_free(); bumped on every alloc
static uint16_t   g_next_ephemeral_udp_port = 49152;
static uint16_t   g_next_ephemeral_tcp_port = 49152;
static uint32_t   g_iss_bump = 0;

static int tcp_conn_index(const tcp_conn_t *c) { return (int)(c - g_tcp_conns); }

static int tcp_handle_of(const tcp_conn_t *c) {
    int i = tcp_conn_index(c);
    return (int)((g_tcp_gen[i] & 0x7FFFFFu) << 8) | i;
}

// Client-facing lookup: fails for stale handles and for connections the
// client has already closed.
static tcp_conn_t *tcp_find_by_handle(int handle) {
    if (handle < 0) return NULL;
    int idx = handle & 0xFF;
    if (idx >= MAX_TCP_CONNS) return NULL;
    tcp_conn_t *c = &g_tcp_conns[idx];
    if (!c->used || c->app_closed) return NULL;
    if ((g_tcp_gen[idx] & 0x7FFFFFu) != ((uint32_t)handle >> 8)) return NULL;
    return c;
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
            do { g_tcp_gen[i]++; } while ((g_tcp_gen[i] & 0x7FFFFFu) == 0);
            tcp_conn_t *c = &g_tcp_conns[i];
            c->used = 1;
            c->rto = TCP_INIT_RTO_MS;
            c->peer_mss = TCP_DEFAULT_PEER_MSS;
            c->ssthresh = 0x0FFFFFFFu;
            return i;
        }
    }
    return -1;
}

static void tcp_free(tcp_conn_t *c) {
    memset(c, 0, sizeof(*c));
}

// Marks the connection failed. If the client has already closed it nobody
// can observe the failure, so the slot is released immediately; otherwise
// it lingers as TCP_DEAD until the client closes the handle.
static void tcp_dead(tcp_conn_t *c, int err) {
    if (c->app_closed) { tcp_free(c); return; }
    c->state = TCP_DEAD;
    c->error = err;
    c->sbuf_len = 0;
    c->fin_queued = 0;
    c->rto_armed = 0;
}

static uint16_t tcp_alloc_port(void) {
    for (;;) {
        uint16_t port = g_next_ephemeral_tcp_port++;
        if (g_next_ephemeral_tcp_port == 0) g_next_ephemeral_tcp_port = 49152;
        int in_use = 0;
        for (int i = 0; i < MAX_TCP_CONNS; i++)
            if (g_tcp_conns[i].used && g_tcp_conns[i].local_port == port) { in_use = 1; break; }
        if (!in_use) return port;
    }
}

// ── TCP: segment output ─────────────────────────────────────────────────

// Smallest window increase worth announcing / smallest non-zero window we
// advertise (receiver-side silly window avoidance).
static size_t tcp_wnd_thresh(void) {
    size_t half = TCP_RECV_BUF_SIZE / 2;
    return half < TCP_LOCAL_MSS ? half : TCP_LOCAL_MSS;
}

static uint16_t tcp_adv_window(const tcp_conn_t *c) {
    size_t free_space = sizeof(c->recv_buf) - c->recv_len;
    return free_space < tcp_wnd_thresh() ? 0 : (uint16_t)free_space;
}

// Builds and transmits one TCP segment to dst_ip via mac. Does not touch
// any connection state.
static int tcp_emit(const uint8_t mac[6], const uint8_t dst_ip[4],
                     uint16_t sport, uint16_t dport,
                     uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                     const uint8_t *opts, size_t opts_len,
                     const uint8_t *payload, size_t payload_len) {
    if (opts_len > TCP_OPT_MAX || payload_len > TCP_MAX_SEG_PAYLOAD) return -1;

    size_t hdr_len = sizeof(tcp_hdr_t) + opts_len;
    size_t tcp_len = hdr_len + payload_len;
    size_t ip_len = sizeof(ipv4_hdr_t) + tcp_len;
    size_t frame_len_raw = sizeof(eth_hdr_t) + ip_len;
    size_t frame_len = frame_len_raw < ETH_FRAME_MIN ? ETH_FRAME_MIN : frame_len_raw;

    uint8_t frame[TCP_FRAME_MAX];
    memset(frame, 0, frame_len);

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst_mac, mac, 6);
    memcpy(eth->src_mac, g_our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)ip_len);
    put_be16((uint8_t *)&ip->id, (uint16_t)(time_ms() & 0xFFFF));
    put_be16((uint8_t *)&ip->flags_frag, IP_FLAG_DF);
    ip->ttl = 64;
    ip->proto = IP_PROTO_TCP;
    memcpy(ip->src_ip, g_our_ip, 4);
    memcpy(ip->dst_ip, dst_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    tcp_hdr_t *tcp = (tcp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    put_be16((uint8_t *)&tcp->src_port, sport);
    put_be16((uint8_t *)&tcp->dst_port, dport);
    put_be32((uint8_t *)&tcp->seq, seq);
    put_be32((uint8_t *)&tcp->ack, ack);
    tcp->data_offset = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags = flags;
    put_be16((uint8_t *)&tcp->window, window);
    if (opts_len) memcpy((uint8_t *)tcp + sizeof(tcp_hdr_t), opts, opts_len);
    if (payload_len) memcpy((uint8_t *)tcp + hdr_len, payload, payload_len);
    put_be16((uint8_t *)&tcp->checksum, l4_checksum(g_our_ip, dst_ip, IP_PROTO_TCP, tcp, tcp_len));

    return net_write_retry(frame, frame_len) < 0 ? -1 : 0;
}

// Resolves the MAC all TCP traffic is sent to (the gateway). When called
// while dispatching an inbound frame this never polls the NIC, so it only
// succeeds if the MAC is already cached.
static int tcp_mac(uint8_t mac[6]) {
    return arp_resolve(g_gateway_ip, 2000, mac);
}

static int tcp_xmit(tcp_conn_t *c, const uint8_t mac[6], uint32_t seq, uint8_t flags,
                     const uint8_t *opts, size_t opts_len,
                     const uint8_t *payload, size_t payload_len) {
    uint16_t win = tcp_adv_window(c);
    int r = tcp_emit(mac, c->remote_ip, c->local_port, c->remote_port,
                      seq, (flags & TCP_FLAG_ACK) ? c->rcv_nxt : 0, flags, win,
                      opts, opts_len, payload, payload_len);
    if (r == 0 && (flags & TCP_FLAG_ACK)) c->last_adv = win;
    return r;
}

static int tcp_send_ack(tcp_conn_t *c) {
    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return -1;
    if (!c->used) return -1; // freed while waiting for ARP
    return tcp_xmit(c, mac, c->snd_nxt, TCP_FLAG_ACK, NULL, 0, NULL, 0);
}

static int tcp_send_syn(tcp_conn_t *c) {
    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return -1;
    if (!c->used) return -1;
    uint8_t opt[4] = { TCP_OPT_MSS, 4, (uint8_t)(TCP_LOCAL_MSS >> 8), (uint8_t)(TCP_LOCAL_MSS & 0xFF) };
    return tcp_xmit(c, mac, c->iss, TCP_FLAG_SYN, opt, sizeof(opt), NULL, 0);
}

static int tcp_send_synack(tcp_conn_t *c) {
    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return -1;
    if (!c->used) return -1;
    uint8_t opt[4] = { TCP_OPT_MSS, 4, (uint8_t)(TCP_LOCAL_MSS >> 8), (uint8_t)(TCP_LOCAL_MSS & 0xFF) };
    return tcp_xmit(c, mac, c->iss, TCP_FLAG_SYN | TCP_FLAG_ACK, opt, sizeof(opt), NULL, 0);
}

// Replies to a segment that matched no connection (RFC 793 reset rules).
static void tcp_send_rst(const uint8_t dst_ip[4], uint16_t sport, uint16_t dport,
                          uint32_t seq, uint32_t ack, int with_ack) {
    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return;
    tcp_emit(mac, dst_ip, sport, dport, seq, ack,
             (uint8_t)(TCP_FLAG_RST | (with_ack ? TCP_FLAG_ACK : 0)), 0, NULL, 0, NULL, 0);
}

static void tcp_reply_unmatched(const uint8_t src_ip[4], const tcp_hdr_t *tcp, size_t payload_len) {
    uint8_t flags = tcp->flags;
    if (flags & TCP_FLAG_RST) return;
    uint16_t our_port   = get_be16((const uint8_t *)&tcp->dst_port);
    uint16_t their_port = get_be16((const uint8_t *)&tcp->src_port);
    if (flags & TCP_FLAG_ACK) {
        tcp_send_rst(src_ip, our_port, their_port, get_be32((const uint8_t *)&tcp->ack), 0, 0);
    } else {
        uint32_t len = (uint32_t)payload_len + ((flags & TCP_FLAG_SYN) ? 1 : 0)
                                              + ((flags & TCP_FLAG_FIN) ? 1 : 0);
        tcp_send_rst(src_ip, our_port, their_port, 0,
                     get_be32((const uint8_t *)&tcp->seq) + len, 1);
    }
}

// Resends the oldest unacknowledged segment (data from the start of the
// send buffer, with our FIN attached if it is that segment's tail).
static void tcp_retransmit_head(tcp_conn_t *c) {
    uint32_t flight = c->snd_max - c->snd_una;
    if (flight == 0) return;
    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return;
    if (!c->used) return;

    size_t data_flight = (c->fin_ever_sent && SEQ_GT(c->snd_max, c->fin_seq)) ? flight - 1 : flight;
    if (data_flight > c->sbuf_len) data_flight = c->sbuf_len;
    size_t len = data_flight < c->peer_mss ? data_flight : c->peer_mss;
    uint8_t flags = TCP_FLAG_ACK;
    if (len == data_flight && c->fin_ever_sent && SEQ_GT(c->snd_max, c->fin_seq)) flags |= TCP_FLAG_FIN;
    if (len == 0 && !(flags & TCP_FLAG_FIN)) return;
    tcp_xmit(c, mac, c->snd_una, flags, NULL, 0, c->sbuf, len);
    c->rtt_active = 0; // Karn: never time a retransmitted segment
}

static void tcp_arm_timer(tcp_conn_t *c) {
    if (!c->rto_armed) {
        c->rto_armed = 1;
        c->rto_deadline = time_ms() + c->rto;
    }
}

// Sends as much buffered data (and then our FIN) as the peer's window and
// the congestion window allow. Returns the number of segments sent.
static int tcp_output(tcp_conn_t *c) {
    switch (c->state) {
    case TCP_ESTABLISHED: case TCP_CLOSE_WAIT: case TCP_FIN_WAIT_1:
    case TCP_CLOSING: case TCP_LAST_ACK:
        break;
    default:
        return 0;
    }

    {   // Cheap early-out so idle connections don't ARP on every tick.
        uint32_t flight = c->snd_nxt - c->snd_una;
        size_t data_flight = (c->fin_sent && flight > 0) ? flight - 1 : flight;
        int need_fin = c->fin_queued && !c->fin_sent;
        if (c->sbuf_len <= data_flight && !need_fin) return 0;
    }

    uint8_t mac[6];
    if (tcp_mac(mac) != 0) return 0;
    if (!c->used) return 0; // state may have changed while waiting for ARP

    int sent = 0;
    for (;;) {
        uint32_t flight = c->snd_nxt - c->snd_una;
        size_t data_flight = (c->fin_sent && flight > 0) ? flight - 1 : flight;
        size_t unsent = c->sbuf_len > data_flight ? c->sbuf_len - data_flight : 0;
        int need_fin = c->fin_queued && !c->fin_sent;
        if (!unsent && !need_fin) break;

        uint32_t wnd = u32_min(c->snd_wnd, c->cwnd);
        uint32_t usable = wnd > flight ? wnd - flight : 0;
        uint32_t seq = c->snd_nxt;
        uint8_t flags = TCP_FLAG_ACK;
        size_t len = 0;

        if (unsent) {
            len = unsent < c->peer_mss ? unsent : c->peer_mss;
            if (len > usable) {
                if (c->snd_wnd == 0 && flight == 0) len = 1;           // zero-window probe
                else if (usable == 0) break;
                else if (usable < c->peer_mss && flight > 0) break;     // don't send runts, wait for ACKs
                else len = usable;
            }
            if (len == unsent) {
                flags |= TCP_FLAG_PSH;
                if (need_fin) flags |= TCP_FLAG_FIN;
            }
        } else {
            flags |= TCP_FLAG_FIN;
        }

        if (tcp_xmit(c, mac, seq, flags, NULL, 0, c->sbuf + data_flight, len) != 0) break;

        c->snd_nxt = seq + (uint32_t)len;
        if (flags & TCP_FLAG_FIN) {
            c->fin_seq = c->snd_nxt;
            c->fin_ever_sent = 1;
            c->fin_sent = 1;
            c->snd_nxt++;
        }
        if (!c->rtt_active && SEQ_GEQ(seq, c->snd_max)) {
            c->rtt_active = 1;
            c->rtt_seq = c->snd_nxt;
            c->rtt_start = time_ms();
        }
        if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        tcp_arm_timer(c);
        sent++;
    }
    return sent;
}

// ── TCP: segment input ──────────────────────────────────────────────────

static void tcp_rtt_sample(tcp_conn_t *c, uint32_t r) {
    if (r == 0) r = 1;
    if (c->srtt == 0) {
        c->srtt = r;
        c->rttvar = r / 2;
    } else {
        uint32_t d = r > c->srtt ? r - c->srtt : c->srtt - r;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + r) / 8;
    }
    uint32_t rto = c->srtt + 4 * c->rttvar;
    if (rto < TCP_MIN_RTO_MS) rto = TCP_MIN_RTO_MS;
    if (rto > TCP_MAX_RTO_MS) rto = TCP_MAX_RTO_MS;
    c->rto = rto;
}

static void tcp_enter_time_wait(tcp_conn_t *c) {
    c->state = TCP_TIME_WAIT;
    c->time_wait_deadline = time_ms() + TCP_TIME_WAIT_MS;
    c->rto_armed = 0;
}

static void tcp_parse_mss(tcp_conn_t *c, const tcp_hdr_t *tcp, size_t hdr_len) {
    const uint8_t *o = (const uint8_t *)tcp + sizeof(tcp_hdr_t);
    size_t n = hdr_len - sizeof(tcp_hdr_t);
    uint32_t mss = TCP_DEFAULT_PEER_MSS;
    size_t i = 0;
    while (i < n) {
        uint8_t kind = o[i];
        if (kind == 0) break;                 // end of options
        if (kind == 1) { i++; continue; }     // NOP
        if (i + 1 >= n) break;
        uint8_t olen = o[i + 1];
        if (olen < 2 || i + olen > n) break;
        if (kind == TCP_OPT_MSS && olen == 4) mss = get_be16(o + i + 2);
        i += olen;
    }
    if (mss < 64) mss = 64;
    if (mss > TCP_MAX_SEG_PAYLOAD) mss = TCP_MAX_SEG_PAYLOAD;
    c->peer_mss = mss;
}

// Runs the state machine for one inbound segment already matched to its
// connection.
static void tcp_input(tcp_conn_t *c, const tcp_hdr_t *tcp, size_t hdr_len,
                       const uint8_t *payload, size_t payload_len) {
    uint32_t seq = get_be32((const uint8_t *)&tcp->seq);
    uint32_t ack = get_be32((const uint8_t *)&tcp->ack);
    uint32_t wnd = get_be16((const uint8_t *)&tcp->window);
    uint8_t flags = tcp->flags;

    if (c->state == TCP_DEAD) return;

    // ── SYN_SENT: waiting for SYN-ACK ──
    if (c->state == TCP_SYN_SENT) {
        int ack_ok = 0;
        if (flags & TCP_FLAG_ACK) {
            if (SEQ_LEQ(ack, c->iss) || SEQ_GT(ack, c->snd_max)) {
                if (!(flags & TCP_FLAG_RST))
                    tcp_send_rst(c->remote_ip, c->local_port, c->remote_port, ack, 0, 0);
                return;
            }
            ack_ok = 1;
        }
        if (flags & TCP_FLAG_RST) {
            if (ack_ok) tcp_dead(c, TCP_E_REFUSED);
            return;
        }
        if ((flags & TCP_FLAG_SYN) && ack_ok) {
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            if (SEQ_GT(c->snd_una, c->snd_nxt)) c->snd_nxt = c->snd_una;
            tcp_parse_mss(c, tcp, hdr_len);
            c->snd_wnd = wnd;
            c->snd_wl1 = seq;
            c->snd_wl2 = ack;
            if (c->rtt_active && c->retx_count == 0)
                tcp_rtt_sample(c, (uint32_t)(time_ms() - c->rtt_start));
            c->rtt_active = 0;
            c->cwnd = 4 * c->peer_mss;
            c->retx_count = 0;
            c->rto_armed = 0;
            c->state = TCP_ESTABLISHED;
            tcp_send_ack(c);
        }
        // A bare SYN (simultaneous open) is ignored.
        return;
    }

    // ── SYN_RCVD: waiting for the final ACK of the handshake ──
    if (c->state == TCP_SYN_RCVD) {
        if (flags & TCP_FLAG_RST) {
            if (seq == c->rcv_nxt) tcp_free(c);      // nobody holds a handle yet
            return;
        }
        if (flags & TCP_FLAG_SYN) { tcp_send_synack(c); return; }   // our SYN-ACK was lost
        if (!(flags & TCP_FLAG_ACK)) return;
        if (ack != c->iss + 1) {
            tcp_send_rst(c->remote_ip, c->local_port, c->remote_port, ack, 0, 0);
            return;
        }
        if (c->rtt_active && c->retx_count == 0)
            tcp_rtt_sample(c, (uint32_t)(time_ms() - c->rtt_start));
        c->rtt_active = 0;
        c->snd_una = ack;
        c->snd_wnd = wnd;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
        c->retx_count = 0;
        c->rto_armed = 0;
        c->state = TCP_ESTABLISHED;
        c->accept_ready = 1;
        // No return: fall through so data piggybacked on this ACK is processed.
    }

    // ── Synchronized states ──
    if (c->state == TCP_TIME_WAIT && (flags & TCP_FLAG_RST)) { tcp_free(c); return; }

    if (flags & TCP_FLAG_RST) {
        size_t room = sizeof(c->recv_buf) - c->recv_len;
        uint32_t lim = c->rcv_nxt + (room ? (uint32_t)room : 1);
        if (SEQ_GEQ(seq, c->rcv_nxt) && SEQ_LT(seq, lim)) tcp_dead(c, TCP_E_RESET);
        return;
    }
    if (flags & TCP_FLAG_SYN) { tcp_send_ack(c); return; } // retransmitted SYN-ACK: our ACK was lost
    if (!(flags & TCP_FLAG_ACK)) return;

    // ACK processing
    if (SEQ_GT(ack, c->snd_max)) { tcp_send_ack(c); return; } // acks data we never sent

    if (SEQ_GEQ(ack, c->snd_una)) {
        int wnd_changed = (wnd != c->snd_wnd);
        if (SEQ_LT(c->snd_wl1, seq) || (c->snd_wl1 == seq && SEQ_LEQ(c->snd_wl2, ack))) {
            c->snd_wnd = wnd;
            c->snd_wl1 = seq;
            c->snd_wl2 = ack;
        }

        if (ack == c->snd_una) {
            // Duplicate ACK: same ack, data outstanding, no payload/FIN, no window change.
            if (c->snd_una != c->snd_max && payload_len == 0 && !(flags & TCP_FLAG_FIN) && !wnd_changed) {
                c->dupacks++;
                if (c->dupacks == 3 && !c->in_recovery) {
                    uint32_t fl = c->snd_max - c->snd_una;
                    c->ssthresh = u32_max(fl / 2, 2 * c->peer_mss);
                    c->recover = c->snd_max;
                    c->in_recovery = 1;
                    tcp_retransmit_head(c);
                    c->cwnd = c->ssthresh + 3 * c->peer_mss;
                } else if (c->in_recovery && c->dupacks > 3) {
                    c->cwnd += c->peer_mss;
                }
            }
        } else {
            uint32_t acked = ack - c->snd_una;
            int fin_acked = c->fin_ever_sent && SEQ_GT(ack, c->fin_seq);
            uint32_t data_acked = fin_acked ? acked - 1 : acked;
            if (data_acked > c->sbuf_len) data_acked = (uint32_t)c->sbuf_len;
            if (data_acked) {
                memmove(c->sbuf, c->sbuf + data_acked, c->sbuf_len - data_acked);
                c->sbuf_len -= data_acked;
            }
            c->snd_una = ack;
            if (SEQ_GT(ack, c->snd_nxt)) c->snd_nxt = ack; // ACK covers data re-sent after a rewind
            if (fin_acked) c->fin_sent = 1;
            c->dupacks = 0;

            if (c->rtt_active && SEQ_GEQ(ack, c->rtt_seq)) {
                tcp_rtt_sample(c, (uint32_t)(time_ms() - c->rtt_start));
                c->rtt_active = 0;
            }

            if (c->in_recovery) {
                if (SEQ_GEQ(ack, c->recover)) {
                    c->cwnd = c->ssthresh;
                    c->in_recovery = 0;
                } else {
                    tcp_retransmit_head(c); // partial ACK: next hole
                    c->cwnd = c->cwnd > acked ? c->cwnd - acked + c->peer_mss : c->peer_mss;
                }
            } else if (c->cwnd < c->ssthresh) {
                c->cwnd += u32_min(acked, c->peer_mss);
            } else {
                uint32_t inc = (c->peer_mss * c->peer_mss) / c->cwnd;
                c->cwnd += inc ? inc : 1;
            }
            if (c->cwnd > 0x100000u) c->cwnd = 0x100000u;

            c->retx_count = 0;
            if (c->snd_una == c->snd_max) {
                c->rto_armed = 0;
            } else {
                c->rto_armed = 1;
                c->rto_deadline = time_ms() + c->rto;
            }

            if (fin_acked) {
                if (c->state == TCP_FIN_WAIT_1) {
                    c->state = TCP_FIN_WAIT_2;
                    c->time_wait_deadline = time_ms() + TCP_FIN_WAIT2_MS;
                } else if (c->state == TCP_CLOSING) {
                    tcp_enter_time_wait(c);
                } else if (c->state == TCP_LAST_ACK) {
                    tcp_free(c);
                    return;
                }
            }
        }
    }

    // Data / FIN processing
    int need_ack = 0;
    int fin = (flags & TCP_FLAG_FIN) != 0;

    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2) {
        if (payload_len || fin) {
            if (SEQ_LT(seq, c->rcv_nxt)) {  // overlaps data we already have
                uint32_t d = c->rcv_nxt - seq;
                need_ack = 1;
                if (d > payload_len) { payload_len = 0; fin = 0; } // entirely old (FIN included)
                else { payload += d; payload_len -= d; }
                seq = c->rcv_nxt;
            }
            if (seq != c->rcv_nxt) {
                need_ack = 1; // gap: duplicate ACK tells the peer where to resume
            } else {
                size_t room = sizeof(c->recv_buf) - c->recv_len;
                size_t n = payload_len < room ? payload_len : room;
                if (n) {
                    memcpy(c->recv_buf + c->recv_len, payload, n);
                    c->recv_len += n;
                    c->rcv_nxt += (uint32_t)n;
                    need_ack = 1;
                }
                if (n < payload_len) {
                    need_ack = 1; // buffer full: only the accepted bytes are ACKed, peer must resend the rest
                } else if (fin) {
                    c->rcv_nxt += 1;
                    c->fin_received = 1;
                    need_ack = 1;
                    if (c->state == TCP_ESTABLISHED)      c->state = TCP_CLOSE_WAIT;
                    else if (c->state == TCP_FIN_WAIT_1)  c->state = TCP_CLOSING;
                    else                                   tcp_enter_time_wait(c);
                }
            }
        }
    } else if (payload_len || fin) {
        // CLOSE_WAIT / CLOSING / LAST_ACK / TIME_WAIT: peer already sent its FIN,
        // so this is a retransmission whose ACK was lost.
        need_ack = 1;
        if (c->state == TCP_TIME_WAIT && fin) c->time_wait_deadline = time_ms() + TCP_TIME_WAIT_MS;
    }

    int sent = tcp_output(c);
    if (need_ack && !sent) tcp_send_ack(c);
}

// ── TCP: passive open (listen / accept) ─────────────────────────────────

#define MAX_TCP_LISTENERS  4
#define TCP_LISTEN_BACKLOG 4   // max connections per listener waiting for accept()

typedef struct { int used; uint16_t port; } tcp_listener_t;
static tcp_listener_t g_tcp_listeners[MAX_TCP_LISTENERS];
static uint32_t       g_tcp_listener_gen[MAX_TCP_LISTENERS];

static int tcp_listener_handle_of(int i) {
    return (int)((g_tcp_listener_gen[i] & 0x7FFFFFu) << 8) | i;
}

static int tcp_listener_find(int handle) {
    if (handle < 0) return -1;
    int idx = handle & 0xFF;
    if (idx >= MAX_TCP_LISTENERS || !g_tcp_listeners[idx].used) return -1;
    if ((g_tcp_listener_gen[idx] & 0x7FFFFFu) != ((uint32_t)handle >> 8)) return -1;
    return idx;
}

// Drops every connection that is still waiting to be accepted by listener li.
static void tcp_listener_drop_pending(int li) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_tcp_conns[i];
        if (!c->used || c->listener != li + 1) continue;
        if (c->state != TCP_SYN_RCVD)
            tcp_send_rst(c->remote_ip, c->local_port, c->remote_port, c->snd_nxt, c->rcv_nxt, 1);
        tcp_free(c);
    }
}

// Called for a segment that matched no connection. Returns 1 if it was a SYN
// for a listening port (consumed, even if dropped for lack of room: the peer
// retransmits), 0 if the caller should treat it as unmatched.
static int tcp_try_accept_syn(const uint8_t src_ip[4], const tcp_hdr_t *tcp, size_t hdr_len) {
    uint8_t flags = tcp->flags;
    if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK | TCP_FLAG_RST)) != TCP_FLAG_SYN) return 0;

    uint16_t lport = get_be16((const uint8_t *)&tcp->dst_port);
    int li = -1;
    for (int i = 0; i < MAX_TCP_LISTENERS; i++)
        if (g_tcp_listeners[i].used && g_tcp_listeners[i].port == lport) { li = i; break; }
    if (li < 0) return 0;

    int pending = 0;
    for (int i = 0; i < MAX_TCP_CONNS; i++)
        if (g_tcp_conns[i].used && g_tcp_conns[i].listener == li + 1) pending++;
    if (pending >= TCP_LISTEN_BACKLOG) return 1;

    int idx = tcp_alloc();
    if (idx < 0) return 1;                       // table full: drop, peer will retry
    tcp_conn_t *c = &g_tcp_conns[idx];

    uint32_t seq = get_be32((const uint8_t *)&tcp->seq);
    memcpy(c->remote_ip, src_ip, 4);
    c->remote_port = get_be16((const uint8_t *)&tcp->src_port);
    c->local_port  = lport;
    c->listener    = li + 1;

    c->irs = seq;
    c->rcv_nxt = seq + 1;
    tcp_parse_mss(c, tcp, hdr_len);
    c->snd_wnd = get_be16((const uint8_t *)&tcp->window);
    c->snd_wl1 = seq;
    c->snd_wl2 = 0;

    g_iss_bump += 64013u;
    c->iss = (uint32_t)time_ms() * 250u + g_iss_bump;
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;                     // our SYN consumes one sequence number
    c->snd_max = c->snd_nxt;
    c->cwnd = 4 * c->peer_mss;
    c->state = TCP_SYN_RCVD;

    c->rtt_active = 1;
    c->rtt_seq = c->snd_nxt;
    c->rtt_start = time_ms();
    c->rto_armed = 1;
    c->rto_deadline = time_ms() + c->rto;

    tcp_send_synack(c);                          // if ARP isn't cached the timer resends it
    return 1;
}

// ── TCP: timers ─────────────────────────────────────────────────────────

static void tcp_on_timeout(tcp_conn_t *c, uint64_t now) {
    if (c->state == TCP_SYN_RCVD) {
        if (++c->retx_count > TCP_SYN_RETRIES) { tcp_free(c); return; }   // nobody has a handle
        c->rtt_active = 0;
        c->rto = u32_min(c->rto * 2, TCP_MAX_RTO_MS);
        c->rto_deadline = now + c->rto;
        tcp_send_synack(c);
        return;
    }
    if (c->state == TCP_SYN_SENT) {
        if (++c->retx_count > TCP_SYN_RETRIES) { tcp_dead(c, TCP_E_TIMEOUT); return; }
        c->rtt_active = 0;
        c->rto = u32_min(c->rto * 2, TCP_MAX_RTO_MS);
        c->rto_deadline = now + c->rto;
        tcp_send_syn(c);
        return;
    }

    // With a zero peer window the retransmit timer acts as the persist
    // timer: it keeps probing and never gives up on its own.
    if (c->snd_wnd != 0) {
        if (++c->retx_count > TCP_MAX_RETRIES) { tcp_dead(c, TCP_E_TIMEOUT); return; }
        uint32_t fl = c->snd_max - c->snd_una;
        c->ssthresh = u32_max(fl / 2, 2 * c->peer_mss);
        c->cwnd = c->peer_mss;
    }
    c->in_recovery = 0;
    c->dupacks = 0;
    c->rtt_active = 0;
    c->rto = u32_min(c->rto * 2, TCP_MAX_RTO_MS);
    c->rto_armed = 1;
    c->rto_deadline = now + c->rto;

    // Go-back-N from the oldest unacked byte; slow start paces the resend.
    c->snd_nxt = c->snd_una;
    c->fin_sent = 0;
    tcp_output(c);
}

// Runs every main-loop pass and inside every blocking wait: retransmit
// timers, TIME_WAIT / FIN_WAIT_2 expiry, and flushing buffered data that
// became sendable.
static void tcp_tick_all(void) {
    uint64_t now = time_ms();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_tcp_conns[i];
        if (!c->used) continue;

        if (c->state == TCP_DEAD) continue;
        if ((c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT_2) && now >= c->time_wait_deadline) {
            tcp_free(c);
            continue;
        }

        if (c->rto_armed && now >= c->rto_deadline) {
            tcp_on_timeout(c, now);
            if (!c->used) continue;
        }
        tcp_output(c);
    }
}

// ── Receive path ────────────────────────────────────────────────────────

#define NET_POLL_EMPTY   0  // nothing was waiting on net0
#define NET_POLL_HANDLED 1  // one frame read and consumed internally (ARP, TCP, or dropped)
#define NET_POLL_FRAME   2  // one other IPv4 frame was copied into out_buf

static void net_rx_tcp(const uint8_t *frame, size_t len) {
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    if ((ip->ver_ihl >> 4) != 4) return;
    size_t ihl = (size_t)(ip->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_hdr_t)) return;
    size_t ip_total = get_be16((const uint8_t *)&ip->total_len);
    if (ip_total < ihl + sizeof(tcp_hdr_t) || sizeof(eth_hdr_t) + ip_total > len) return;
    if (get_be16((const uint8_t *)&ip->flags_frag) & 0x3FFF) return; // fragments unsupported
    if (!ip_eq(ip->dst_ip, g_our_ip)) return;

    const tcp_hdr_t *tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ihl);
    size_t seg_len = ip_total - ihl; // trims Ethernet padding
    size_t hdr_len = (size_t)(tcp->data_offset >> 4) * 4;
    if (hdr_len < sizeof(tcp_hdr_t) || hdr_len > seg_len) return;

#if TCP_VERIFY_RX_CHECKSUM
    if (l4_checksum(ip->src_ip, ip->dst_ip, IP_PROTO_TCP, tcp, seg_len) != 0) return;
#endif

    const uint8_t *payload = (const uint8_t *)tcp + hdr_len;
    size_t payload_len = seg_len - hdr_len;

    // Every inbound frame arrives via the gateway. Remember its MAC so a SYN-ACK
    // can be sent without an ARP round trip (inside RX dispatch ARP can't block).
    if (!arp_cache_find(g_gateway_ip))
        arp_cache_put(g_gateway_ip, ((const eth_hdr_t *)frame)->src_mac);

    tcp_conn_t *c = tcp_find_by_tuple(ip->src_ip,
                                       get_be16((const uint8_t *)&tcp->src_port),
                                       get_be16((const uint8_t *)&tcp->dst_port));
    g_rx_dispatch++;
    if (c) tcp_input(c, tcp, hdr_len, payload, payload_len);
    else if (!tcp_try_accept_syn(ip->src_ip, tcp, hdr_len))
        tcp_reply_unmatched(ip->src_ip, tcp, payload_len);
    g_rx_dispatch--;
}

// Reads at most one frame from net0. ARP replies feed the cache and TCP
// segments are dispatched to their connection as side effects. Any other
// IPv4 frame is copied into out_buf if the caller supplied one (so it can
// look for ping/UDP/DHCP replies); otherwise it is dropped.
static int net_poll_once(uint8_t *out_buf, size_t out_cap, ssize_t *out_len) {
    uint8_t buf[1600];
    ssize_t r = read(g_net_fd, buf, sizeof(buf));
    if (r <= 0) return NET_POLL_EMPTY;

    if ((size_t)r < sizeof(eth_hdr_t)) return NET_POLL_HANDLED;
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    uint16_t et = get_be16((uint8_t *)&eth->ethertype);

    if (et == ETHERTYPE_ARP) {
        if ((size_t)r >= sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
            arp_pkt_t *arp = (arp_pkt_t *)(buf + sizeof(eth_hdr_t));
            if (get_be16((uint8_t *)&arp->oper) == ARP_OP_REPLY) arp_cache_put(arp->spa, arp->sha);
        }
        return NET_POLL_HANDLED;
    }

    if (et == ETHERTYPE_IPV4) {
        if ((size_t)r < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return NET_POLL_HANDLED;
        ipv4_hdr_t *rip = (ipv4_hdr_t *)(buf + sizeof(eth_hdr_t));

        if (rip->proto == IP_PROTO_TCP) {
            net_rx_tcp(buf, (size_t)r);
            return NET_POLL_HANDLED;
        }

        if (out_buf) {
            size_t n = (size_t)r < out_cap ? (size_t)r : out_cap;
            memcpy(out_buf, buf, n);
            if (out_len) *out_len = (ssize_t)n;
            return NET_POLL_FRAME;
        }
    }

    return NET_POLL_HANDLED;
}

// Drains whatever is waiting on net0 (bounded, so a flood can't starve the
// rest of the loop).
static void net_service(void) {
    for (int i = 0; i < 16; i++)
        if (net_poll_once(NULL, 0, NULL) == NET_POLL_EMPTY) break;
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

    // Inside RX dispatch we must not read more frames (that would re-enter
    // TCP input mid-update); the request is sent and the caller's own
    // retransmit logic recovers once the reply has been cached.
    if (g_rx_dispatch) return -1;

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_poll_once(NULL, 0, NULL);
        arp_entry_t *e = arp_cache_find(target_ip);
        if (e) { memcpy(out_mac, e->mac, 6); return 0; }
        yield();
    }
    return -1;
}

// ── Ping ────────────────────────────────────────────────────────────────

static uint16_t g_next_icmp_id = 0x1000;

// Distinguishes *why* a ping failed so the client can tell "couldn't get
// the packet onto the wire" from "sent fine, nobody answered".
#define DO_PING_ERR_ARP     -1
#define DO_PING_ERR_TX      -2
#define DO_PING_ERR_TIMEOUT -3

static int do_ping(const uint8_t target_ip[4], uint32_t timeout_ms, uint32_t *out_rtt_ms) {
    uint8_t gw_mac[6];
    if (arp_resolve(g_gateway_ip, timeout_ms, gw_mac) != 0) return DO_PING_ERR_ARP;

    uint16_t icmp_id = g_next_icmp_id++;
    if (g_next_icmp_id == 0) g_next_icmp_id = 0x1000;

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
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen) == NET_POLL_FRAME) {
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

// ── UDP ─────────────────────────────────────────────────────────────────

static uint16_t udp_alloc_ephemeral_port(void) {
    uint16_t port = g_next_ephemeral_udp_port++;
    if (g_next_ephemeral_udp_port == 0) g_next_ephemeral_udp_port = 49152;
    return port;
}

// Sends one UDP datagram to target_ip:dst_port and waits up to timeout_ms
// for one reply datagram from that same (ip, port) to our source port.
// *inout_src_port == 0 picks an ephemeral port (and reports which); a
// nonzero value is used as-is. On success, copies up to reply_cap bytes of
// the reply into out_reply and reports its real length via *out_reply_len,
// even if it was truncated to fit.
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
    memcpy((uint8_t *)udp + sizeof(udp_hdr_t), payload, payload_len);
    put_be16((uint8_t *)&udp->checksum, udp_checksum(g_our_ip, target_ip, udp, udp_len));

    uint64_t t_start = time_ms();
    if (net_write_retry(frame, frame_len) < 0) return -1;

    uint64_t deadline = t_start + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t rxbuf[1600];
        ssize_t rxlen = 0;
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen) == NET_POLL_FRAME) {
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
            if (out_reply_len) *out_reply_len = rpayload_len;
            *out_rtt_ms = (uint32_t)(time_ms() - t_start);
            return 0;
        }
        yield();
    }
    return -1;
}

// ── DHCP client ─────────────────────────────────────────────────────────
//
// Minimal DISCOVER -> OFFER -> REQUEST -> ACK client. There is no DECLINE,
// INFORM, or RENEWING/REBINDING state machine: when half the lease has
// elapsed, dhcp_tick() simply reruns the whole four-message exchange.
//
// Messages are always broadcast (there's no gateway MAC to ARP for before
// we're bound). Replies are read through net_poll_once(), which hands back
// any non-TCP IPv4 frame, and are matched on UDP ports 67->68 and the xid
// only -- no IP configuration is needed to receive them.

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

// RFC 2131: a client must be able to accept at least 312 bytes of options.
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

// Size of everything before the options array (BOOTP fixed fields plus the
// magic cookie): the minimum length of a valid received DHCP packet.
#define DHCP_FIXED_LEN (sizeof(dhcp_pkt_t) - DHCP_OPTIONS_LEN)

#define DHCP_FRAME_MAX (sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + \
                         sizeof(udp_hdr_t) + sizeof(dhcp_pkt_t))

static uint32_t g_dhcp_lease_secs;
static uint64_t g_dhcp_lease_deadline; // time_ms() at which dhcp_tick() renews

// Fills in the fixed BOOTP fields and magic cookie common to every message
// we send; the caller adds options. ciaddr (and therefore the IP source
// address in dhcp_broadcast()) is 0.0.0.0 until a lease has been obtained:
// RFC 2131 requires it for DISCOVER, and a message claiming an address the
// server never gave us is treated as a bogus renewal and ignored.
static void dhcp_pkt_init(dhcp_pkt_t *p, uint32_t xid) {
    memset(p, 0, sizeof(*p));
    p->op = DHCP_OP_BOOTREQUEST;
    p->htype = DHCP_HTYPE_ETHERNET;
    p->hlen = 6;
    put_be32((uint8_t *)&p->xid, xid);
    put_be16((uint8_t *)&p->flags, 0x8000); // ask the server to broadcast its reply
    if (g_dhcp_bound) memcpy(p->ciaddr, g_our_ip, 4);
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

// Finds option `code` in a received packet. Option overload (options
// continuing in the sname/file fields) is not handled.
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
    put_be16((uint8_t *)&udp->checksum, udp_checksum(pkt->ciaddr, bcast_ip, udp, udp_len));

    return net_write_retry(frame, frame_len) < 0 ? -1 : 0;
}

// Waits for one reply matching xid. expect_type == 0 accepts any message
// type (used for the REQUEST step, where either ACK or NAK is a valid
// answer and the caller decides). A short packet is zero-padded into *out.
static int dhcp_wait_reply(uint32_t xid, uint8_t expect_type, uint32_t timeout_ms, dhcp_pkt_t *out) {
    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t rxbuf[1600];
        ssize_t rxlen = 0;
        if (net_poll_once(rxbuf, sizeof(rxbuf), &rxlen) == NET_POLL_FRAME) {
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

// Returns 0 on ACK, -1 on NAK or if no answer arrives.
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
        if (mt2 && opt_len == 1 && *mt2 == DHCP_NAK) return -1; // retrying this offer is pointless
    }
    return -1;
}

// Runs the full DISCOVER/OFFER/REQUEST/ACK exchange. On success updates
// g_our_ip/g_gateway_ip/g_subnet_mask and arms the renewal deadline. On
// failure all of those are left untouched, so callers keep whatever was
// there before (the static fallback at startup, or the previous lease on a
// failed renewal).
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
    if (router && opt_len >= 4) memcpy(g_gateway_ip, router, 4); // first router if several

    const uint8_t *lease = dhcp_find_opt(&ack, DHCP_OPT_LEASE_TIME, &opt_len);
    g_dhcp_lease_secs = (lease && opt_len == 4) ? get_be32(lease) : 3600; // default if omitted

    // Renew at the halfway point (no separate T2/rebind phase).
    g_dhcp_lease_deadline = time_ms() + (uint64_t)g_dhcp_lease_secs * 1000ull / 2;
    g_dhcp_bound = 1;

    // Cached MACs may belong to a previous network.
    memset(g_arp_cache, 0, sizeof(g_arp_cache));

    printf("netd: dhcp bound ip=%d.%d.%d.%d gw=%d.%d.%d.%d mask=%d.%d.%d.%d lease=%ds\n",
           g_our_ip[0], g_our_ip[1], g_our_ip[2], g_our_ip[3],
           g_gateway_ip[0], g_gateway_ip[1], g_gateway_ip[2], g_gateway_ip[3],
           g_subnet_mask[0], g_subnet_mask[1], g_subnet_mask[2], g_subnet_mask[3],
           (int)g_dhcp_lease_secs);
    return 0;
}

// Runs every main-loop pass. Once bound and past the renewal point it
// reruns the whole exchange (blocking for as long as that takes); on
// failure the old lease stays in place and it retries after 30 s.
static void dhcp_tick(void) {
    if (!g_dhcp_bound) return;
    if (time_ms() < g_dhcp_lease_deadline) return;
    if (dhcp_client_run() != 0) {
        g_dhcp_lease_deadline = time_ms() + 30000;
    }
}

// ── TCP: application-facing calls ───────────────────────────────────────
//
// These block (spinning on net_service()+tcp_tick_all()) but never wait for
// more than the client-supplied timeout. Error returns are TCP_E_*.

static const char *tcp_err_text(int err) {
    switch (err) {
        case TCP_E_RESET:   return "tcp: connection reset";
        case TCP_E_TIMEOUT: return "tcp: timed out";
        case TCP_E_REFUSED: return "tcp: connection refused";
        default:            return "tcp: bad handle or connection state";
    }
}

// Returns a handle (>= 0) once the connection is ESTABLISHED, else TCP_E_*.
static int tcp_open(const uint8_t target_ip[4], uint16_t dst_port, uint32_t timeout_ms) {
    int idx = tcp_alloc();
    if (idx < 0) return TCP_E_BADHANDLE;
    tcp_conn_t *c = &g_tcp_conns[idx];

    memcpy(c->remote_ip, target_ip, 4);
    c->remote_port = dst_port;
    c->local_port = tcp_alloc_port();

    g_iss_bump += 64013u;
    c->iss = (uint32_t)time_ms() * 250u + g_iss_bump;
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;   // the SYN consumes one sequence number
    c->snd_max = c->snd_nxt;
    c->state = TCP_SYN_SENT;

    c->rtt_active = 1;
    c->rtt_seq = c->snd_nxt;
    c->rtt_start = time_ms();
    c->rto_armed = 1;
    c->rto_deadline = time_ms() + c->rto;

    tcp_send_syn(c); // if this fails (e.g. ARP), the SYN timer retries

    int handle = tcp_handle_of(c);
    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        net_service();
        tcp_tick_all();
        c = &g_tcp_conns[idx];
        if (!c->used) return TCP_E_TIMEOUT;
        if (c->state == TCP_DEAD) { int err = c->error; tcp_free(c); return err; }
        if (c->state == TCP_ESTABLISHED) return handle;
        yield();
    }
    tcp_free(&g_tcp_conns[idx]);
    return TCP_E_TIMEOUT;
}

// Queues len bytes for sending (all or nothing) and returns once they are
// in the send buffer, not once they are acknowledged; a failed connection
// is reported by later calls. Waits up to timeout_ms for buffer space.
static int tcp_app_send(int handle, const uint8_t *data, uint16_t len, uint32_t timeout_ms) {
    if (len > TCP_SEND_BUF_SIZE) return TCP_E_BADHANDLE;
    uint64_t deadline = time_ms() + timeout_ms;
    tcp_conn_t *c;
    for (;;) {
        c = tcp_find_by_handle(handle);
        if (!c) return TCP_E_BADHANDLE;
        if (c->state == TCP_DEAD) return c->error;
        if (c->fin_queued || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
            return TCP_E_BADHANDLE;
        if (sizeof(c->sbuf) - c->sbuf_len >= len) break;
        if (time_ms() >= deadline) return TCP_E_TIMEOUT;
        net_service();
        tcp_tick_all();
        yield();
    }
    memcpy(c->sbuf + c->sbuf_len, data, len);
    c->sbuf_len += len;
    tcp_output(c);
    return (int)len;
}

// Waits up to timeout_ms for data or EOF. Returns 0 with *out_len == 0 and
// *out_eof == 0 if the timeout expires with nothing to read. Buffered data
// is returned before any connection error is reported.
static int tcp_app_recv(int handle, uint8_t *out, uint16_t cap, uint32_t timeout_ms,
                         uint16_t *out_len, uint8_t *out_eof) {
    *out_len = 0;
    *out_eof = 0;
    uint64_t deadline = time_ms() + timeout_ms;
    tcp_conn_t *c;
    for (;;) {
        c = tcp_find_by_handle(handle);
        if (!c) return TCP_E_BADHANDLE;
        if (c->recv_len > 0 || c->fin_received) break;
        if (c->state == TCP_DEAD) return c->error;
        if (time_ms() >= deadline) break;
        net_service();
        tcp_tick_all();
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

    // Tell the peer when reading has opened up a meaningfully larger window.
    if (n > 0 && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)) {
        uint16_t adv = tcp_adv_window(c);
        if (adv > c->last_adv && (c->last_adv == 0 || adv - c->last_adv >= tcp_wnd_thresh()))
            tcp_send_ack(c);
    }
    return 0;
}

// Begins a graceful close and releases the handle immediately. The stack
// finishes the shutdown (flush buffered data, FIN, TIME_WAIT) on its own.
static int tcp_app_close(int handle) {
    tcp_conn_t *c = tcp_find_by_handle(handle);
    if (!c) return 0;
    c->app_closed = 1;

    switch (c->state) {
    case TCP_ESTABLISHED:
        c->fin_queued = 1;
        c->state = TCP_FIN_WAIT_1;
        tcp_output(c);
        break;
    case TCP_CLOSE_WAIT:
        c->fin_queued = 1;
        c->state = TCP_LAST_ACK;
        tcp_output(c);
        break;
    case TCP_SYN_SENT:
    case TCP_DEAD:
        tcp_free(c);
        break;
    default:
        break;
    }
    return 0;
}

// Starts listening on `port`. Listening again on a port that already has a
// listener (e.g. one left behind by a crashed server) replaces it.
static int tcp_app_listen(uint16_t port) {
    if (port == 0) return TCP_E_BADHANDLE;
    int slot = -1;
    for (int i = 0; i < MAX_TCP_LISTENERS; i++)
        if (g_tcp_listeners[i].used && g_tcp_listeners[i].port == port) { slot = i; break; }
    if (slot >= 0) {
        tcp_listener_drop_pending(slot);
    } else {
        for (int i = 0; i < MAX_TCP_LISTENERS; i++)
            if (!g_tcp_listeners[i].used) { slot = i; break; }
    }
    if (slot < 0) return TCP_E_BADHANDLE;      // listener table full
    g_tcp_listeners[slot].used = 1;
    g_tcp_listeners[slot].port = port;
    do { g_tcp_listener_gen[slot]++; } while ((g_tcp_listener_gen[slot] & 0x7FFFFFu) == 0);
    return tcp_listener_handle_of(slot);
}

// Waits up to timeout_ms for an established connection. Returns 0 with
// *out_handle == 0 if none arrived in time, 0 with a handle if one did.
static int tcp_app_accept(int lhandle, uint32_t timeout_ms, int *out_handle,
                           uint8_t out_ip[4], uint16_t *out_port) {
    *out_handle = 0;
    uint64_t deadline = time_ms() + timeout_ms;
    for (;;) {
        int li = tcp_listener_find(lhandle);
        if (li < 0) return TCP_E_BADHANDLE;
        for (int i = 0; i < MAX_TCP_CONNS; i++) {
            tcp_conn_t *c = &g_tcp_conns[i];
            if (c->used && c->listener == li + 1 && c->accept_ready) {
                c->listener = 0;
                c->accept_ready = 0;
                memcpy(out_ip, c->remote_ip, 4);
                *out_port = c->remote_port;
                *out_handle = tcp_handle_of(c);
                return 0;
            }
        }
        if (time_ms() >= deadline) return 0;
        net_service();
        tcp_tick_all();
        yield();
    }
}

static int tcp_app_unlisten(int lhandle) {
    int li = tcp_listener_find(lhandle);
    if (li < 0) return 0;
    g_tcp_listeners[li].used = 0;
    tcp_listener_drop_pending(li);
    return 0;
}

// ── Mailbox request handler ─────────────────────────────────────────────

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
                snprintf(resp->message, sizeof(resp->message), "%s", tcp_err_text(handle));
            }
            break;
        }

        case NETD_CMD_TCP_SEND: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            int r = req->tcp_payload_len > NETD_TCP_MAX_PAYLOAD
                        ? TCP_E_BADHANDLE
                        : tcp_app_send(req->tcp_handle, req->tcp_payload, req->tcp_payload_len, timeout);
            if (r >= 0) {
                resp->success = 1;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "%s", tcp_err_text(r));
            }
            break;
        }

        case NETD_CMD_TCP_RECV: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            uint16_t cap = req->tcp_recv_cap ? req->tcp_recv_cap : NETD_TCP_MAX_PAYLOAD;
            if (cap > NETD_TCP_MAX_PAYLOAD) cap = NETD_TCP_MAX_PAYLOAD;
            uint16_t got = 0; uint8_t eof = 0;
            int r = tcp_app_recv(req->tcp_handle, resp->tcp_payload, cap, timeout, &got, &eof);
            if (r == 0) {
                resp->success = 1;
                resp->tcp_payload_len = got;
                resp->tcp_eof = eof;
            } else {
                resp->success = 0;
                snprintf(resp->message, sizeof(resp->message), "%s", tcp_err_text(r));
            }
            break;
        }

        case NETD_CMD_TCP_LISTEN: {
            int h = tcp_app_listen(req->tcp_dst_port);
            if (h >= 0) { resp->success = 1; resp->tcp_handle = h; }
            else { resp->success = 0; snprintf(resp->message, sizeof(resp->message), "%s", tcp_err_text(h)); }
            break;
        }

        case NETD_CMD_TCP_ACCEPT: {
            uint32_t timeout = req->timeout_ms ? req->timeout_ms : 5000;
            int newh = 0;
            int r = tcp_app_accept(req->tcp_handle, timeout, &newh, resp->peer_ip, &resp->peer_port);
            if (r == 0) { resp->success = 1; resp->tcp_handle = newh; }   // newh == 0: nobody yet
            else { resp->success = 0; snprintf(resp->message, sizeof(resp->message), "%s", tcp_err_text(r)); }
            break;
        }

        case NETD_CMD_TCP_UNLISTEN: {
            tcp_app_unlisten(req->tcp_handle);
            resp->success = 1;
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
    // SHM_O_EXCL tolerates a segment left behind by a previous netd.
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

    // Only one netd instance owns the mailbox, so resetting it to idle on
    // startup is safe.
    memset(shm, 0, sizeof(*shm));

    printf("netd: mailbox '%s' ready, owns net0 exclusively.\n", NETD_IPC_SHM_NAME);
    printf("netd: mac=");
    for (int i = 0; i < 6; i++) printf("%02x%s", g_our_mac[i], i < 5 ? ":" : "\n");

    // Each pass:
    //  - drains a bounded number of frames from net0 (ARP replies are
    //    cached, TCP segments go to their connection);
    //  - ticks TCP timers and flushes sendable data on every connection,
    //    whether or not a client is mid-call;
    //  - renews the DHCP lease if it is due;
    //  - services at most one pending mailbox request. Requests block, but
    //    the blocking TCP calls keep draining and ticking while they wait.
    while (1) {
        net_service();
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