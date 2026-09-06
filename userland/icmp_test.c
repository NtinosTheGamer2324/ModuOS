// icmp_test.c — send an ICMP echo request, wait for the echo reply.
//
// Builds on arp_test.c: resolves the *next-hop* MAC via ARP first (either
// the target itself if it's on-link, or the gateway if it's not -- for
// simplicity this always ARPs the gateway, since that's correct whenever
// the target isn't on our local subnet, and still works fine even when it
// is, as long as the gateway will route to it).
//
// Usage: icmp_test <target_ip> [gateway_ip] [sender_ip]
//   target_ip  - required. Who to ping, e.g. 10.0.2.2 or 8.8.8.8
//   gateway_ip - default 10.0.2.2 (QEMU usermode gateway). This is who we
//                actually ARP and send the Ethernet frame to.
//   sender_ip  - default 10.0.2.15 (QEMU usermode guest IP). No real IP
//                config exists yet (no DHCP/static assignment), so this
//                is just the address we claim in the packets.
//
 

#include "libc.h"

#define NET_CMD_GET_MAC 4u

#define ETHERTYPE_ARP  0x0806u
#define ETHERTYPE_IPV4 0x0800u

#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_OP_REQUEST     1u
#define ARP_OP_REPLY       2u

#define IP_PROTO_ICMP 1u

#define ICMP_TYPE_ECHO_REQUEST 8u
#define ICMP_TYPE_ECHO_REPLY   0u

#define ETH_FRAME_MIN 60  /* without FCS */

typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype; /* big-endian */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;      /* version(4) | ihl(4), ihl in 32-bit words */
    uint8_t  dscp_ecn;
    uint16_t total_len;    /* big-endian */
    uint16_t id;           /* big-endian */
    uint16_t flags_frag;   /* big-endian */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;     /* big-endian */
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} ipv4_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;     /* big-endian */
    uint16_t id;           /* big-endian */
    uint16_t seq;          /* big-endian */
} icmp_hdr_t;

#define ICMP_PAYLOAD_LEN 32
#define IP_PACKET_LEN    (sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t) + ICMP_PAYLOAD_LEN)
#define FRAME_LEN_RAW    (sizeof(eth_hdr_t) + IP_PACKET_LEN)
#define FRAME_LEN        (FRAME_LEN_RAW < ETH_FRAME_MIN ? ETH_FRAME_MIN : FRAME_LEN_RAW)

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int parse_ipv4(const char *s, uint8_t out[4]) {
    /* This libc's sscanf() only understands bare "%d" tokens (no literal
     * '.' separators), so dotted-quad parsing is done by hand here. */
    int idx = 0, val = 0, digits = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
        } else if (*p == '.' || *p == 0) {
            if (digits == 0 || idx > 3) return -1;
            out[idx++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*p == 0) break;
        } else {
            return -1;
        }
    }
    return (idx == 4) ? 0 : -1;
}

static void print_ipv4(const uint8_t ip[4]) {
    printf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void print_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        printf("%02x", mac[i]);
        if (i < 5) printf(":");
    }
}

/* Standard Internet checksum (RFC 1071): one's-complement sum of 16-bit
 * words, folded, then complemented. Works for both the IP header and the
 * ICMP message (different "protocols", same algorithm). */
static uint16_t checksum16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += get_be16(p);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += ((uint16_t)p[0]) << 8; /* odd trailing byte, high-order */
    }

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum & 0xFFFF);
}

/* ── ARP resolve: send a request, wait up to timeout_ms for the reply. ── */
static int arp_resolve(int fd, const uint8_t our_mac[6], const uint8_t sender_ip[4],
                        const uint8_t target_ip[4], uint32_t timeout_ms, uint8_t out_mac[6]) {
    uint8_t frame[ETH_FRAME_MIN];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memset(eth->dst_mac, 0xFF, 6);
    memcpy(eth->src_mac, our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_ARP);

    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    put_be16((uint8_t *)&arp->htype, ARP_HTYPE_ETHERNET);
    put_be16((uint8_t *)&arp->ptype, ARP_PTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    put_be16((uint8_t *)&arp->oper, ARP_OP_REQUEST);
    memcpy(arp->sha, our_mac, 6);
    memcpy(arp->spa, sender_ip, 4);
    memset(arp->tha, 0x00, 6);
    memcpy(arp->tpa, target_ip, 4);

    if (write(fd, frame, sizeof(frame)) < 0) return -1;

    uint64_t deadline = time_ms() + timeout_ms;
    while (time_ms() < deadline) {
        uint8_t buf[1600];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (r < (ssize_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) continue;

            eth_hdr_t *rx_eth = (eth_hdr_t *)buf;
            if (get_be16((uint8_t *)&rx_eth->ethertype) != ETHERTYPE_ARP) continue;

            arp_pkt_t *rx_arp = (arp_pkt_t *)(buf + sizeof(eth_hdr_t));
            if (get_be16((uint8_t *)&rx_arp->oper) != ARP_OP_REPLY) continue;
            if (!ip_eq(rx_arp->spa, target_ip)) continue;

            memcpy(out_mac, rx_arp->sha, 6);
            return 0;
        }
        yield();
    }
    return -1;
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        printf("usage: icmp_test <target_ip> [gateway_ip] [sender_ip]\n");
        return 1;
    }

    uint8_t target_ip[4];
    if (parse_ipv4(argv[1], target_ip) != 0) {
        printf("icmp_test: bad target IP '%s'\n", argv[1]);
        return 1;
    }

    uint8_t gateway_ip[4] = {10, 0, 2, 2};   /* QEMU usermode gateway  */
    uint8_t sender_ip[4]  = {10, 0, 2, 15};  /* QEMU usermode guest IP */

    if (argc >= 3 && parse_ipv4(argv[2], gateway_ip) != 0) {
        printf("icmp_test: bad gateway IP '%s'\n", argv[2]);
        return 1;
    }
    if (argc >= 4 && parse_ipv4(argv[3], sender_ip) != 0) {
        printf("icmp_test: bad sender IP '%s'\n", argv[3]);
        return 1;
    }

    const char *path = "$/dev/net/net0";
    int fd = open(path, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        printf("icmp_test: could not open %s (errno=%d)\n", path, errno);
        return 1;
    }

    uint8_t our_mac[6];
    {
        uint32_t cmd = NET_CMD_GET_MAC;
        if (invoke(fd, &cmd, sizeof(cmd), our_mac, sizeof(our_mac)) < 0) {
            printf("icmp_test: GET_MAC invoke failed (errno=%d)\n", errno);
            close(fd);
            return 1;
        }
    }

    /* We always send the Ethernet frame to the gateway's MAC -- if the
     * target isn't on our local subnet (e.g. 8.8.8.8), the gateway is
     * who actually routes it onward; if it is on-link, ARPing "the
     * gateway" address here should just be pointed at the target itself. */
    printf("icmp_test: resolving gateway ");
    print_ipv4(gateway_ip);
    printf(" via ARP...\n");

    uint8_t gw_mac[6];
    if (arp_resolve(fd, our_mac, sender_ip, gateway_ip, 3000, gw_mac) != 0) {
        printf("icmp_test: ARP resolve for gateway failed, aborting.\n");
        close(fd);
        return 1;
    }
    printf("icmp_test: gateway is at ");
    print_mac(gw_mac);
    printf("\n");

    /* ── Build the frame: Ethernet + IPv4 + ICMP echo request. ── */
    uint8_t frame[FRAME_LEN];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memcpy(eth->dst_mac, gw_mac, 6);
    memcpy(eth->src_mac, our_mac, 6);
    put_be16((uint8_t *)&eth->ethertype, ETHERTYPE_IPV4);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl    = 0x45; /* version 4, IHL 5 (20-byte header, no options) */
    ip->dscp_ecn   = 0;
    put_be16((uint8_t *)&ip->total_len, (uint16_t)IP_PACKET_LEN);
    put_be16((uint8_t *)&ip->id, 1);
    put_be16((uint8_t *)&ip->flags_frag, 0);
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_ICMP;
    ip->checksum   = 0; /* filled in below */
    memcpy(ip->src_ip, sender_ip, 4);
    memcpy(ip->dst_ip, target_ip, 4);
    put_be16((uint8_t *)&ip->checksum, checksum16(ip, sizeof(ipv4_hdr_t)));

    icmp_hdr_t *icmp = (icmp_hdr_t *)((uint8_t *)ip + sizeof(ipv4_hdr_t));
    icmp->type     = ICMP_TYPE_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0; /* filled in below */
    put_be16((uint8_t *)&icmp->id, 1234);
    put_be16((uint8_t *)&icmp->seq, 1);

    uint8_t *icmp_payload = (uint8_t *)icmp + sizeof(icmp_hdr_t);
    for (int i = 0; i < ICMP_PAYLOAD_LEN; i++) icmp_payload[i] = (uint8_t)('a' + (i % 23));

    put_be16((uint8_t *)&icmp->checksum,
             checksum16(icmp, sizeof(icmp_hdr_t) + ICMP_PAYLOAD_LEN));

    printf("icmp_test: pinging ");
    print_ipv4(target_ip);
    printf(" (id=1234 seq=1)...\n");

    uint64_t t_start = time_ms();
    if (write(fd, frame, sizeof(frame)) < 0) {
        printf("icmp_test: write() failed.\n");
        close(fd);
        return 1;
    }

    int got_reply = 0;
    uint64_t deadline = time_ms() + 3000;
    while (time_ms() < deadline && !got_reply) {
        uint8_t buf[1600];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break;

            size_t min_len = sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_hdr_t);
            if ((size_t)r < min_len) continue;

            eth_hdr_t *rx_eth = (eth_hdr_t *)buf;
            if (get_be16((uint8_t *)&rx_eth->ethertype) != ETHERTYPE_IPV4) continue;

            ipv4_hdr_t *rx_ip = (ipv4_hdr_t *)(buf + sizeof(eth_hdr_t));
            if (rx_ip->proto != IP_PROTO_ICMP) continue;
            if (!ip_eq(rx_ip->src_ip, target_ip)) continue;

            uint8_t ihl_bytes = (uint8_t)((rx_ip->ver_ihl & 0x0F) * 4);
            icmp_hdr_t *rx_icmp = (icmp_hdr_t *)((uint8_t *)rx_ip + ihl_bytes);
            if (rx_icmp->type != ICMP_TYPE_ECHO_REPLY) continue;
            if (get_be16((uint8_t *)&rx_icmp->id) != 1234) continue;
            if (get_be16((uint8_t *)&rx_icmp->seq) != 1) continue;

            uint64_t rtt = time_ms() - t_start;
            printf("icmp_test: reply from ");
            print_ipv4(rx_ip->src_ip);
            printf(": seq=1 ttl=%d time=%lums\n", rx_ip->ttl, (unsigned long)rtt);
            got_reply = 1;
            break;
        }
        if (!got_reply) yield();
    }

    if (!got_reply) {
        printf("icmp_test: no reply after 3000ms (timeout).\n");
    }

    close(fd);
    return got_reply ? 0 : 1;
}