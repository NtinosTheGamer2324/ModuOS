// arp_test.c — send one ARP request, wait for the reply, print the MAC.
//
// This is the first "does networking actually round-trip" test: unlike
// net_test.c's broadcast frame (which nothing ever answers), a real host
// on the link is expected to reply to an ARP request for its own IP.
// If this works, frames are going out AND coming back through the RX
// ring correctly — the prerequisite for ICMP/ping and everything above it.
//
// Usage: arp_test [target_ip] [sender_ip]
//   target_ip  - IP to resolve (default 10.0.2.2,  QEMU usermode gateway)
//   sender_ip  - IP to claim as ours in the request (default 10.0.2.15,
//                QEMU usermode's default guest address). We have no real
//                IP yet (no DHCP/static config exists), so this is just
//                the address we put in the ARP packet -- it doesn't have
//                to be "assigned" anywhere for the request/reply to work.
//
 

#include "libc.h"

#define NET_CMD_GET_MAC 4u

#define ETHERTYPE_ARP 0x0806u

#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_OP_REQUEST     1u
#define ARP_OP_REPLY       2u

#define ETH_FRAME_MIN 60  /* without FCS */

typedef struct __attribute__((packed)) {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype; /* big-endian */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype; /* big-endian */
    uint16_t ptype; /* big-endian */
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;  /* big-endian */
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_pkt_t;

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

int md_main(long argc, char **argv) {
    uint8_t target_ip[4] = {10, 0, 2, 2};   /* QEMU usermode gateway  */
    uint8_t sender_ip[4] = {10, 0, 2, 15};  /* QEMU usermode guest IP */

    if (argc >= 2) {
        if (parse_ipv4(argv[1], target_ip) != 0) {
            printf("arp_test: bad target IP '%s'\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        if (parse_ipv4(argv[2], sender_ip) != 0) {
            printf("arp_test: bad sender IP '%s'\n", argv[2]);
            return 1;
        }
    }

    const char *path = "$/dev/net/net0";
    int fd = open(path, O_RDWR | O_NONBLOCK, 0);
    if (fd < 0) {
        printf("arp_test: could not open %s (errno=%d)\n", path, errno);
        return 1;
    }

    /* Our own MAC, via invoke() -- goes in the ARP sender fields and the
     * Ethernet source address. */
    uint8_t our_mac[6];
    {
        uint32_t cmd = NET_CMD_GET_MAC;
        ssize_t r = invoke(fd, &cmd, sizeof(cmd), our_mac, sizeof(our_mac));
        if (r < 0) {
            printf("arp_test: GET_MAC invoke failed (errno=%d)\n", errno);
            close(fd);
            return 1;
        }
    }

    printf("arp_test: who-has ");
    print_ipv4(target_ip);
    printf("? tell ");
    print_ipv4(sender_ip);
    printf(" (");
    print_mac(our_mac);
    printf(")\n");

    /* ── Build the frame: Ethernet header + ARP request, zero-padded to
     * the minimum Ethernet frame size. ── */
    uint8_t frame[ETH_FRAME_MIN];
    memset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    memset(eth->dst_mac, 0xFF, 6);              /* broadcast */
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
    memset(arp->tha, 0x00, 6);                  /* unknown -- that's the point */
    memcpy(arp->tpa, target_ip, 4);

    ssize_t w = write(fd, frame, sizeof(frame));
    if (w < 0) {
        printf("arp_test: write() failed, rc=%ld\n", (long)w);
        close(fd);
        return 1;
    }
    printf("arp_test: request sent (%ld bytes), waiting for reply...\n", (long)w);

    /* ── Wait for the reply. read() is non-blocking (O_NONBLOCK), so we
     * poll for up to ~3s of wall-clock time, yielding between attempts
     * instead of busy-spinning. SYS_SLEEP only takes whole seconds, which
     * is too coarse here, so time_ms() + yield() is used instead. */
    int got_reply = 0;
    uint64_t deadline = time_ms() + 3000;
    while (time_ms() < deadline && !got_reply) {
        uint8_t buf[1600];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break; /* ring empty for now */

            if (r < (ssize_t)(sizeof(eth_hdr_t) + sizeof(arp_pkt_t))) continue;

            eth_hdr_t *rx_eth = (eth_hdr_t *)buf;
            uint16_t et = get_be16((uint8_t *)&rx_eth->ethertype);
            if (et != ETHERTYPE_ARP) continue; /* not ARP -- ignore */

            arp_pkt_t *rx_arp = (arp_pkt_t *)(buf + sizeof(eth_hdr_t));
            uint16_t oper = get_be16((uint8_t *)&rx_arp->oper);
            if (oper != ARP_OP_REPLY) continue;
            if (!ip_eq(rx_arp->spa, target_ip)) continue; /* wrong host */

            printf("arp_test: reply -- ");
            print_ipv4(target_ip);
            printf(" is at ");
            print_mac(rx_arp->sha);
            printf("\n");
            got_reply = 1;
            break;
        }
        if (!got_reply) yield();
    }

    if (!got_reply) {
        printf("arp_test: no reply after retries.\n");
        printf("arp_test: check that the target host exists and the link/NAT is set up.\n");
    }

    close(fd);
    return got_reply ? 0 : 1;
}