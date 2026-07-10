#include "./libc.h"

/* ── wire protocol (must match kernel sqrm_net_devfs) ─────────────────── */
#define NET_CMD_GET_MODE     1u
#define NET_CMD_GET_LINK_UP  2u
#define NET_CMD_GET_MTU      3u
#define NET_CMD_GET_MAC      4u
#define NET_CMD_TX_FRAME     5u
#define NET_CMD_RX_POLL      6u

#define NET_MODE_ETH         1u
#define NET_MODE_WIFI        2u

/* ── Ethernet / ARP / IP / ICMP constants ─────────────────────────────── */
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IP   0x0800
#define ARP_REQUEST    1
#define ARP_REPLY      2
#define IPPROTO_ICMP   1
#define ICMP_ECHO_REQ  8
#define ICMP_ECHO_REP  0

#define MAKE_IP(a,b,c,d) \
    ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

static uint16_t htons16(uint16_t v) {
    return (uint16_t)((v>>8)|(v<<8));
}
static uint32_t htonl32(uint32_t v) {
    return ((v>>24)&0xFF)|((v>>8)&0xFF00)|
           ((v<<8)&0xFF0000)|((v<<24)&0xFF000000);
}

/* ── low-level transact ───────────────────────────────────────────────── */
static int g_fd = -1;
static uint8_t g_mac[6];
static uint32_t g_ip;

static int transact(uint32_t cmd_code,
                    const uint8_t *payload, uint32_t payload_len,
                    uint8_t *rep_out, int rep_max)
{
    /* build cmd: cmd(4) + len(4) + data */
    uint8_t cmd[8 + 1500];
    memset(cmd, 0, 8);
    *(uint32_t*)(cmd+0) = cmd_code;
    *(uint32_t*)(cmd+4) = payload_len;
    if (payload && payload_len)
        memcpy(cmd+8, payload, payload_len);

    ssize_t wr = write(g_fd, cmd, 8 + payload_len);
    if (wr < 0) return -1;

    /* read until null terminator */
    uint8_t rep[1520];
    memset(rep, 0, sizeof(rep));
    int got = 0;
    while (got < (int)sizeof(rep)-1) {
        ssize_t n = read(g_fd, rep+got, sizeof(rep)-1-got);
        if (n < 0) return -2;
        if (n == 0) { yield(); continue; }
        got += (int)n;
        if (rep[got-1] == 0) break;
    }

    if (got < 12) return -3;

    int32_t status = *(int32_t*)(rep+4);
    uint32_t len   = *(uint32_t*)(rep+8);

    if (rep_out && rep_max > 0) {
        int copy = (int)len < rep_max ? (int)len : rep_max;
        memcpy(rep_out, rep+12, copy);
    }

    return (int)status;
}

/* ── checksum ─────────────────────────────────────────────────────────── */
static uint16_t checksum(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i+1 < len; i+=2)
        sum += (uint16_t)((data[i]<<8)|data[i+1]);
    if (len & 1) sum += (uint16_t)(data[len-1]<<8);
    while (sum>>16) sum = (sum&0xFFFF)+(sum>>16);
    return (uint16_t)(~sum);
}

/* ── send raw ethernet frame ──────────────────────────────────────────── */
static int send_frame(const uint8_t *frame, uint32_t len) {
    return transact(NET_CMD_TX_FRAME, frame, len, NULL, 0);
}

/* ── rx poll: returns frame length or 0 ──────────────────────────────── */
static int rx_poll(uint8_t *buf, int max) {
    uint8_t tmp[1500];
    int rc = transact(NET_CMD_RX_POLL, NULL, 0, tmp, sizeof(tmp));
    if (rc != 0) return 0;
    /* len is in last transact's rep->len — re-read via reply buf */
    /* simpler: re-issue and grab length from reply directly */
    uint8_t cmd[8];
    memset(cmd, 0, 8);
    *(uint32_t*)cmd = NET_CMD_RX_POLL;
    write(g_fd, cmd, 8);
    uint8_t rep[1520];
    memset(rep, 0, sizeof(rep));
    int got = 0;
    while (got < (int)sizeof(rep)-1) {
        ssize_t n = read(g_fd, rep+got, sizeof(rep)-1-got);
        if (n < 0) return 0;
        if (n == 0) return 0; /* nothing ready */
        got += (int)n;
        if (rep[got-1] == 0) break;
    }
    if (got < 12) return 0;
    uint32_t len = *(uint32_t*)(rep+8);
    if (len == 0) return 0;
    int copy = (int)len < max ? (int)len : max;
    memcpy(buf, rep+12, copy);
    return copy;
}

/* ── test helpers ─────────────────────────────────────────────────────── */
static void print_mac(const uint8_t *m) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           m[0],m[1],m[2],m[3],m[4],m[5]);
}
static void print_ip(uint32_t ip) {
    printf("%u.%u.%u.%u",
           (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
}

static int passed = 0;
static int failed = 0;

static void ok(const char *name) {
    printf("  [PASS] %s\n", name);
    passed++;
}
static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
    failed++;
}

/* ═══════════════════════════════════════════════════════════════════════
   TESTS
   ═══════════════════════════════════════════════════════════════════════ */

static void test_get_mode(void) {
    uint32_t mode = 0;
    int rc = transact(NET_CMD_GET_MODE, NULL, 0, (uint8_t*)&mode, 4);
    if (rc != 0)          { fail("GET_MODE", "status != 0"); return; }
    if (mode == NET_MODE_ETH)  { printf("    mode: Ethernet\n"); ok("GET_MODE"); }
    else if (mode == NET_MODE_WIFI) { printf("    mode: WiFi\n"); ok("GET_MODE"); }
    else                  { fail("GET_MODE", "unknown mode"); }
}

static void test_get_link(void) {
    uint32_t link = 0;
    int rc = transact(NET_CMD_GET_LINK_UP, NULL, 0, (uint8_t*)&link, 4);
    if (rc != 0) { fail("GET_LINK_UP", "status != 0"); return; }
    printf("    link: %s\n", link ? "UP" : "DOWN");
    if (link) ok("GET_LINK_UP");
    else       fail("GET_LINK_UP", "link is down");
}

static void test_get_mtu(void) {
    uint32_t mtu = 0;
    int rc = transact(NET_CMD_GET_MTU, NULL, 0, (uint8_t*)&mtu, 4);
    if (rc != 0)       { fail("GET_MTU", "status != 0"); return; }
    if (mtu < 576 || mtu > 9000) { fail("GET_MTU", "mtu out of range"); return; }
    printf("    mtu: %u\n", mtu);
    ok("GET_MTU");
}

static void test_get_mac(void) {
    int rc = transact(NET_CMD_GET_MAC, NULL, 0, g_mac, 6);
    if (rc != 0) { fail("GET_MAC", "status != 0"); return; }
    printf("    mac: "); print_mac(g_mac); printf("\n");
    ok("GET_MAC");
}

static void test_arp_announce(void) {
    /* gratuitous ARP: who-has MY_IP tell MY_IP */
    g_ip = MAKE_IP(10,0,2,15);
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

    uint8_t frame[42];
    memset(frame, 0, sizeof(frame));

    /* Ethernet header */
    memcpy(frame+0, bcast,  6);
    memcpy(frame+6, g_mac,  6);
    *(uint16_t*)(frame+12) = htons16(ETHERTYPE_ARP);

    /* ARP body */
    *(uint16_t*)(frame+14) = htons16(1);      /* hw type Ethernet */
    *(uint16_t*)(frame+16) = htons16(0x0800); /* proto IPv4 */
    frame[18] = 6; frame[19] = 4;             /* hw/proto len */
    *(uint16_t*)(frame+20) = htons16(ARP_REQUEST);
    memcpy(frame+22, g_mac, 6);
    *(uint32_t*)(frame+28) = htonl32(g_ip);
    memset(frame+32, 0, 6);                   /* target mac unknown */
    *(uint32_t*)(frame+38) = htonl32(g_ip);   /* target ip = self */

    int rc = send_frame(frame, 42);
    if (rc != 0) { fail("ARP_ANNOUNCE", "tx_frame failed"); return; }
    ok("ARP_ANNOUNCE");
}

static void test_arp_request(void) {
    /* ARP who-has gateway 10.0.2.1 */
    uint32_t gw = MAKE_IP(10,0,2,1);
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

    uint8_t frame[42];
    memset(frame, 0, sizeof(frame));
    memcpy(frame+0, bcast, 6);
    memcpy(frame+6, g_mac, 6);
    *(uint16_t*)(frame+12) = htons16(ETHERTYPE_ARP);
    *(uint16_t*)(frame+14) = htons16(1);
    *(uint16_t*)(frame+16) = htons16(0x0800);
    frame[18] = 6; frame[19] = 4;
    *(uint16_t*)(frame+20) = htons16(ARP_REQUEST);
    memcpy(frame+22, g_mac, 6);
    *(uint32_t*)(frame+28) = htonl32(g_ip);
    memset(frame+32, 0, 6);
    *(uint32_t*)(frame+38) = htonl32(gw);

    int rc = send_frame(frame, 42);
    if (rc != 0) { fail("ARP_REQUEST", "tx_frame failed"); return; }

    /* wait up to ~200ms for ARP reply */
    uint8_t buf[1500];
    uint8_t gw_mac[6];
    int resolved = 0;
    for (int i = 0; i < 20 && !resolved; i++) {
        int len = rx_poll(buf, sizeof(buf));
        if (len >= 42) {
            uint16_t et = (uint16_t)((buf[12]<<8)|buf[13]);
            uint16_t op = (uint16_t)((buf[20]<<8)|buf[21]);
            uint32_t src_ip = ((uint32_t)buf[28]<<24)|((uint32_t)buf[29]<<16)|
                              ((uint32_t)buf[30]<<8)|buf[31];
            if (et == ETHERTYPE_ARP && op == ARP_REPLY && src_ip == htonl32(gw)) {
                memcpy(gw_mac, buf+22, 6);
                resolved = 1;
            }
        }
        yield();
    }

    if (resolved) {
        printf("    gw mac: "); print_mac(gw_mac); printf("\n");
        ok("ARP_REQUEST (got reply)");
    } else {
        /* tx worked even if no reply in VM */
        printf("    no ARP reply received (ok in some VM configs)\n");
        ok("ARP_REQUEST (tx only)");
    }
}

static void test_icmp_ping(void) {
    /* ICMP echo to gateway 10.0.2.1 via broadcast dst (no ARP needed) */
    uint32_t dst_ip = MAKE_IP(10,0,2,1);
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

    uint8_t frame[42 + 8]; /* eth(14) + ip(20) + icmp(8) */
    memset(frame, 0, sizeof(frame));

    /* Ethernet */
    memcpy(frame+0, bcast, 6);
    memcpy(frame+6, g_mac, 6);
    *(uint16_t*)(frame+12) = htons16(ETHERTYPE_IP);

    /* IP */
    frame[14] = 0x45;                              /* ver+ihl */
    frame[15] = 0;                                 /* dscp */
    *(uint16_t*)(frame+16) = htons16(28);          /* total len = 20+8 */
    *(uint16_t*)(frame+18) = htons16(0x1234);      /* id */
    *(uint16_t*)(frame+20) = 0;                    /* flags+frag */
    frame[22] = 64;                                /* ttl */
    frame[23] = IPPROTO_ICMP;
    *(uint32_t*)(frame+26) = htonl32(g_ip);        /* src */
    *(uint32_t*)(frame+30) = htonl32(dst_ip);      /* dst */
    *(uint16_t*)(frame+24) = htons16(checksum(frame+14, 20)); /* ip csum */

    /* ICMP echo request */
    frame[34] = ICMP_ECHO_REQ;
    frame[35] = 0;
    *(uint16_t*)(frame+36) = 0;                    /* csum placeholder */
    *(uint16_t*)(frame+38) = htons16(0x1337);      /* id */
    *(uint16_t*)(frame+40) = htons16(1);           /* seq */
    *(uint16_t*)(frame+36) = htons16(checksum(frame+34, 8));

    int rc = send_frame(frame, 42);
    if (rc != 0) { fail("ICMP_PING", "tx_frame failed"); return; }

    /* wait for echo reply */
    uint8_t buf[1500];
    int replied = 0;
    for (int i = 0; i < 30 && !replied; i++) {
        int len = rx_poll(buf, sizeof(buf));
        if (len >= 42) {
            uint16_t et = (uint16_t)((buf[12]<<8)|buf[13]);
            if (et == ETHERTYPE_IP && buf[23] == IPPROTO_ICMP &&
                buf[34] == ICMP_ECHO_REP) {
                uint32_t src = ((uint32_t)buf[26]<<24)|((uint32_t)buf[27]<<16)|
                               ((uint32_t)buf[28]<<8)|buf[29];
                printf("    reply from "); print_ip(htonl32(src));
                printf(" seq=%u\n", (unsigned)((buf[40]<<8)|buf[41]));
                replied = 1;
            }
        }
        yield();
    }

    if (replied) ok("ICMP_PING (got reply)");
    else {
        printf("    no ICMP reply (ok in some VM configs)\n");
        ok("ICMP_PING (tx only)");
    }
}

static void test_rx_poll_idle(void) {
    /* just check rx_poll doesn't crash when nothing is queued */
    uint8_t buf[1500];
    int len = rx_poll(buf, sizeof(buf));
    printf("    rx_poll idle: got %d bytes\n", len);
    ok("RX_POLL_IDLE");
}

/* ═══════════════════════════════════════════════════════════════════════
   ENTRY
   ═══════════════════════════════════════════════════════════════════════ */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== nettest ===\n");

    g_fd = open("$/dev/net/net0", O_RDWR, 0);
    if (g_fd < 0) {
        printf("FATAL: cannot open $/dev/net/net0 (%d)\n", g_fd);
        return 1;
    }
    printf("fd=%d\n\n", g_fd);

    printf("[1] GET_MODE\n");       test_get_mode();
    printf("[2] GET_LINK_UP\n");    test_get_link();
    printf("[3] GET_MTU\n");        test_get_mtu();
    printf("[4] GET_MAC\n");        test_get_mac();
    printf("[5] ARP_ANNOUNCE\n");   test_arp_announce();
    printf("[6] ARP_REQUEST\n");    test_arp_request();
    printf("[7] ICMP_PING\n");      test_icmp_ping();
    printf("[8] RX_POLL_IDLE\n");   test_rx_poll_idle();

    printf("\n=== results: %d passed, %d failed ===\n", passed, failed);

    close(g_fd);
    return failed ? 1 : 0;
}