#include "libc.h"
#include "string.h"
#include "netman/protocol/arp.h"
#include "netman/protocol/ip/ipv4.h"
#include "netman/core/dhcp.h"

/* ── Driver command codes ────────────────────────────────────────────────── */
#define NET_CMD_GET_MODE    1
#define NET_CMD_GET_LINK_UP 2
#define NET_CMD_GET_MTU     3
#define NET_CMD_GET_MAC     4
#define NET_CMD_TX_FRAME    5
#define NET_CMD_RX_POLL     6

/* ── Driver wire types ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t cmd;
    int32_t  status;
    uint32_t len;
    uint8_t  data[1500];
} net_reply_t;

typedef struct {
    uint32_t cmd;
    uint32_t len;
    uint8_t  data[1500];
} net_cmd_t;

/* ── net_cmd: send a command and receive a reply ─────────────────────────── */
static int net_cmd(int fd, uint32_t cmd,
                   const void *tx_data, uint32_t tx_len,
                   net_reply_t *out) {
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cmd = cmd;
    c.len = tx_len;
    if (tx_data && tx_len) {
        if (tx_len > sizeof(c.data)) return -1;
        memcpy(c.data, tx_data, tx_len);
    }

    if (write(fd, &c, sizeof(c)) < 0) return -1;
    if (read(fd, out, sizeof(*out)) < 0) return -1;
    return out->status;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void print_mac(const uint8_t *m) {
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char buf[3];
        buf[0] = hex[(m[i] >> 4) & 0xF];
        buf[1] = hex[m[i] & 0xF];
        buf[2] = 0;
        printf("%s", buf);
        if (i < 5) printf(":");
    }
    printf("\n");
}

/* ── netman_open: open the NIC fd and return it ──────────────────────────── */
int netman_open(void) {
    int fd = open("$/dev/net/net0", O_RDWR, 0);
    return fd;   /* -1 if absent */
}

/* ── netman_get_mac ───────────────────────────────────────────────────────── */
int netman_get_mac(int fd, uint8_t mac_out[6]) {
    net_reply_t r;
    if (net_cmd(fd, NET_CMD_GET_MAC, NULL, 0, &r) != 0) return -1;
    memcpy(mac_out, r.data, 6);
    return 0;
}

/* ── netman_get_mtu ───────────────────────────────────────────────────────── */
int netman_get_mtu(int fd, uint32_t *mtu_out) {
    net_reply_t r;
    if (net_cmd(fd, NET_CMD_GET_MTU, NULL, 0, &r) != 0) return -1;
    memcpy(mtu_out, r.data, sizeof(*mtu_out));
    return 0;
}

/* ── netman_get_link_up ───────────────────────────────────────────────────── */
int netman_get_link_up(int fd) {
    net_reply_t r;
    if (net_cmd(fd, NET_CMD_GET_LINK_UP, NULL, 0, &r) != 0) return -1;
    uint32_t up;
    memcpy(&up, r.data, sizeof(up));
    return (int)up;
}

/* ── netman_tx_frame ──────────────────────────────────────────────────────── */
int netman_tx_frame(int fd, const void *frame, uint32_t len) {
    net_reply_t r;
    return net_cmd(fd, NET_CMD_TX_FRAME, frame, len, &r);
}

/* ── netman_rx_poll ───────────────────────────────────────────────────────── */
int netman_rx_poll(int fd, void *buf, uint32_t *len_out) {
    net_reply_t r;
    if (net_cmd(fd, NET_CMD_RX_POLL, NULL, 0, &r) != 0) return -1;
    if (r.len == 0) return 0;
    if (r.len > 1500) r.len = 1500;
    memcpy(buf, r.data, r.len);
    *len_out = r.len;
    return 1;
}

/* ── netman_print_info ────────────────────────────────────────────────────── */
void netman_print_info(void) {
    int fd = netman_open();
    if (fd < 0) {
        printf("No network device\n");
        return;
    }

    net_reply_t r;

    /* Link status */
    if (net_cmd(fd, NET_CMD_GET_LINK_UP, NULL, 0, &r) == 0) {
        uint32_t up;
        memcpy(&up, r.data, sizeof(up));
        printf("Link: %s\n", up ? "UP" : "DOWN");
    }

    /* MTU */
    if (net_cmd(fd, NET_CMD_GET_MTU, NULL, 0, &r) == 0) {
        uint32_t mtu;
        memcpy(&mtu, r.data, sizeof(mtu));
        printf("MTU: %u\n", mtu);
    }

    /* MAC address */
    if (net_cmd(fd, NET_CMD_GET_MAC, NULL, 0, &r) == 0) {
        printf("MAC: ");
        print_mac(r.data);
    }

    close(fd);
}

/*
 * ── netman_bring_up ────────────────────────────────────────────────────────
 *
 * Full bring-up sequence:
 *   1. Open NIC
 *   2. Check link
 *   3. Read MAC
 *   4. Run DHCP DORA to obtain an IP
 *   5. Print obtained lease
 *
 * Returns 0 on success, negative on any failure.
 */
int netman_bring_up(void) {
    int fd = netman_open();
    if (fd < 0) {
        printf("netman: no network device\n");
        return -1;
    }

    /* Link check */
    int link = netman_get_link_up(fd);
    if (link <= 0) {
        printf("netman: link is DOWN, aborting\n");
        close(fd);
        return -1;
    }
    printf("netman: link UP\n");

    /* MAC */
    uint8_t mac[6] = {0};
    if (netman_get_mac(fd, mac) < 0) {
        printf("netman: failed to read MAC\n");
        close(fd);
        return -1;
    }
    printf("netman: MAC ");
    print_mac(mac);

    /* IPv4 context (will be configured by DHCP) */
    static ipv4_ctx_t ipv4;
    uint8_t zero_ip[4] = {0};
    ipv4_init(&ipv4, fd, mac, zero_ip);

    /* DHCP */
    static dhcp_ctx_t dhcp;
    dhcp_init(&dhcp, fd, mac, &ipv4);

    printf("netman: running DHCP...\n");
    if (dhcp_request(&dhcp, 10000) < 0) {
        printf("netman: DHCP failed\n");
        close(fd);
        return -1;
    }

    dhcp_lease_print(&dhcp.lease);

    /* ARP: announce our IP (gratuitous ARP) */
    static arp_ctx_t arp;
    arp_init(&arp, fd, mac, dhcp.lease.ip);
    arp_send_request(&arp, dhcp.lease.ip);   /* gratuitous ARP */

    /* fd intentionally left open – caller may call netman_rx_poll() */
    return fd;
}

/* ── md_main ──────────────────────────────────────────────────────────────── */
int md_main(long argc, char **argv) {
    /* Default: print interface info */
    if (argc < 2) {
        netman_print_info();
        return 0;
    }

    /* "netman up" – full DHCP bring-up */
    if (strcmp(argv[1], "up") == 0) {
        int fd = netman_bring_up();
        if (fd >= 0) close(fd);
        return (fd >= 0) ? 0 : 1;
    }

    /* "netman arp" – print ARP cache after bring-up */
    if (strcmp(argv[1], "arp") == 0) {
        int fd = netman_bring_up();
        if (fd < 0) return 1;

        uint8_t mac[6] = {0};
        netman_get_mac(fd, mac);

        static arp_ctx_t arp;
        uint8_t zero[4] = {0};
        arp_init(&arp, fd, mac, zero);
        arp_cache_print(&arp);
        close(fd);
        return 0;
    }

    printf("Usage: netman [up|arp]\n");
    return 1;
}