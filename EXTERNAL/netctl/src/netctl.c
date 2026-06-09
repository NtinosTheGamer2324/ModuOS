/*
 * netctl.c  –  ModuOS userland network manager core
 *
 * Knows nothing about devfs, sqrm_net_cmd_t, or file descriptors.
 * All hardware I/O goes through the netctl_driver_t vtable supplied
 * by the caller (driver.c for the SQRM backend).
 *
 * Responsibilities:
 *   - Store network configuration (MAC, IP, mask, gateway)
 *   - Manage the ethertype-based protocol dispatch table
 *   - Maintain the ARP cache
 *   - Provide netctl_eth_send() for protocol modules
 *   - Run the RX poll loop (netctl_run)
 */

#define LIBC_NO_START
#include "libc.h"
#include "driver.h"
#include "netctl.h"
#include "protocol/arp.h"
#include "protocol/ip.h"
#include "protocol/icmp.h"
#include "protocol/udp.h"

/* ── Global state ─────────────────────────────────────────────────────── */
netctl_cfg_t     g_netcfg;
arp_entry_t      g_arp_cache[ARP_CACHE_SIZE];

static netctl_driver_t  *g_driver      = NULL;
static proto_handler_t   g_protos[NETCTL_MAX_PROTOS];
static int               g_proto_count = 0;

/* ── Debug helpers ────────────────────────────────────────────────────── */
void netctl_print_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (i) putc(':');
        putc("0123456789abcdef"[mac[i] >> 4]);
        putc("0123456789abcdef"[mac[i] & 0xF]);
    }
}

void netctl_print_ip(ipv4_addr_t ip) {
    printf("%d.%d.%d.%d",
           (ip >> 24) & 0xFF,
           (ip >> 16) & 0xFF,
           (ip >>  8) & 0xFF,
           (ip      ) & 0xFF);
}

/* ── ARP cache ────────────────────────────────────────────────────────── */
const uint8_t *arp_cache_lookup(ipv4_addr_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip)
            return g_arp_cache[i].mac;
    return NULL;
}

void arp_cache_update(ipv4_addr_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp_cache[i].valid) {
            g_arp_cache[i].ip = ip;
            memcpy(g_arp_cache[i].mac, mac, ETH_ALEN);
            g_arp_cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: evict slot 0 */
    g_arp_cache[0].ip = ip;
    memcpy(g_arp_cache[0].mac, mac, ETH_ALEN);
    g_arp_cache[0].valid = 1;
}

/* ── Protocol registration ────────────────────────────────────────────── */
int netctl_proto_register(const proto_handler_t *h) {
    if (g_proto_count >= NETCTL_MAX_PROTOS) {
        printf("[netctl] ERROR: protocol table full (max %d)\n", NETCTL_MAX_PROTOS);
        return -1;
    }
    g_protos[g_proto_count++] = *h;
    printf("[netctl] registered ethertype 0x%04x (%s)\n", h->ethertype, h->name);
    return 0;
}

/* ── Raw Ethernet TX ──────────────────────────────────────────────────── */
int netctl_eth_send(const uint8_t *dst_mac,
                    uint16_t       ethertype,
                    const void    *payload,
                    size_t         payload_len)
{
    if (!g_driver) return -1;
    if (payload_len > ETH_MAX_PAYLOAD) {
        printf("[netctl] eth_send: payload too large (%d)\n", (int)payload_len);
        return -1;
    }

    static uint8_t frame[ETH_MAX_FRAME];
    eth_hdr_t *eth = (eth_hdr_t *)frame;

    memcpy(eth->dst, dst_mac,      ETH_ALEN);
    memcpy(eth->src, g_netcfg.mac, ETH_ALEN);
    eth->ethertype = htons(ethertype);

    if (payload && payload_len)
        memcpy(frame + ETH_HDR_LEN, payload, payload_len);

    return g_driver->tx(g_driver, frame, ETH_HDR_LEN + payload_len);
}

/* ── RX dispatch ──────────────────────────────────────────────────────── */
void netctl_rx_dispatch(const uint8_t *frame, size_t len) {
    if (len < ETH_HDR_LEN) return;

    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    uint16_t etype = ntohs(eth->ethertype);

    for (int i = 0; i < g_proto_count; i++) {
        if (g_protos[i].ethertype == etype) {
            g_protos[i].rx(frame, len);
            return;
        }
    }
}

/* ── Init ─────────────────────────────────────────────────────────────── */
int netctl_init(netctl_driver_t *drv,
                ipv4_addr_t      ip,
                ipv4_addr_t      netmask,
                ipv4_addr_t      gateway)
{
    g_driver = drv;
    memset(&g_netcfg,   0, sizeof(g_netcfg));
    memset(g_arp_cache, 0, sizeof(g_arp_cache));

    /* Query hardware via the driver vtable */
    if (drv->get_mac(drv, g_netcfg.mac) != 0) {
        printf("[netctl] ERROR: get_mac failed\n");
        return -1;
    }

    uint32_t mtu = 1500;
    drv->get_mtu(drv, &mtu);
    g_netcfg.mtu = mtu;

    g_netcfg.link_up = drv->get_link_up(drv);

    g_netcfg.ip      = ip;
    g_netcfg.netmask = netmask;
    g_netcfg.gateway = gateway;

    printf("[netctl]   MAC  : "); netctl_print_mac(g_netcfg.mac);    putc('\n');
    printf("[netctl]   IP   : "); netctl_print_ip(g_netcfg.ip);      putc('\n');
    printf("[netctl]   Mask : "); netctl_print_ip(g_netcfg.netmask); putc('\n');
    printf("[netctl]   GW   : "); netctl_print_ip(g_netcfg.gateway); putc('\n');
    printf("[netctl]   MTU  : %u\n", g_netcfg.mtu);
    printf("[netctl]   Link : %s\n", g_netcfg.link_up > 0 ? "up" : "DOWN");

    /* Register built-in protocol handlers */
    arp_register();
    ip_register();
    icmp_register();
    udp_register();

    return 0;
}

/* ── RX poll loop ─────────────────────────────────────────────────────── */
void netctl_run(void) {
    static uint8_t rx_buf[ETH_MAX_FRAME];
    printf("[netctl] RX loop started\n");

    for (;;) {
        size_t got = 0;
        int rc = g_driver->rx_poll(g_driver, rx_buf, sizeof(rx_buf), &got);

        if (rc < 0) {
            /* Hard error — back off */
            printf("[netctl] rx_poll error %d\n", rc);
            sleep(1);
            continue;
        }

        if (got == 0) {
            yield();
            continue;
        }

        netctl_rx_dispatch(rx_buf, got);
    }
}

/* ── Single-shot RX poll (used by arp_resolve and other blocking waits) ── */
int netctl_rx_poll_once(void) {
    if (!g_driver) return -1;
    static uint8_t rx_buf[ETH_MAX_FRAME];
    size_t got = 0;
    int rc = g_driver->rx_poll(g_driver, rx_buf, sizeof(rx_buf), &got);
    if (rc < 0) return -1;
    if (got == 0) return 0;
    netctl_rx_dispatch(rx_buf, got);
    return 1;
}