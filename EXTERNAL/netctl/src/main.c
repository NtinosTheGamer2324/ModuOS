/*
 * main.c  –  netctl ring-3 networking service
 *
 * Mirrors the structure of calc_service.c exactly.
 * Registers at $/user/net with read, write, and invoke ops.
 *
 * read()   – returns current network info as a netctl_get_info_resp_t
 * write()  – accepts a netctl_set_ip_req_t to reconfigure IP
 * invoke() – full command/response API (ping, UDP send, ARP, etc.)
 */

#include "libc.h"
#include "driver.h"
#include "netctl.h"
#include "protocol/arp.h"
#include "protocol/icmp.h"
#include "protocol/udp.h"
#include "netctl_api.h"

/* ── read() – return current network info ────────────────────────────── */
static ssize_t net_read(void *ctx, void *buf, size_t count)
{
    (void)ctx;
    if (count < sizeof(netctl_get_info_resp_t)) return -1;

    netctl_get_info_resp_t *resp = (netctl_get_info_resp_t *)buf;
    memset(resp, 0, sizeof(*resp));
    memcpy(resp->mac, g_netcfg.mac, 6);
    resp->ip      = g_netcfg.ip;
    resp->netmask = g_netcfg.netmask;
    resp->gateway = g_netcfg.gateway;
    resp->mtu     = g_netcfg.mtu;
    resp->link_up = g_netcfg.link_up;
    resp->status  = NETCTL_OK;
    return (ssize_t)sizeof(*resp);
}

/* ── write() – reconfigure IP ────────────────────────────────────────── */
static ssize_t net_write(void *ctx, const void *buf, size_t count)
{
    (void)ctx;
    if (count < sizeof(netctl_set_ip_req_t)) return -1;

    const netctl_set_ip_req_t *req = (const netctl_set_ip_req_t *)buf;
    /* Validate it's actually a SET_IP command */
    if (req->cmd != NETCTL_CMD_SET_IP) return -1;

    g_netcfg.ip      = req->ip;
    g_netcfg.netmask = req->netmask;
    g_netcfg.gateway = req->gateway;
    arp_announce();
    return (ssize_t)count;
}

/* ── invoke() – full command API ─────────────────────────────────────── */
static ssize_t net_invoke(void       *ctx,
                          const void *in_buf,  size_t in_size,
                          void       *out_buf, size_t out_size)
{
    (void)ctx;
    if (!in_buf || in_size < sizeof(uint32_t)) return -1;

    uint32_t cmd;
    memcpy(&cmd, in_buf, sizeof(cmd));

    switch (cmd) {

        case NETCTL_CMD_GET_INFO: {
            if (out_size < sizeof(netctl_get_info_resp_t)) return -1;
            return net_read(NULL, out_buf, out_size);
        }

        case NETCTL_CMD_SET_IP: {
            if (in_size  < sizeof(netctl_set_ip_req_t))  return -1;
            if (out_size < sizeof(netctl_set_ip_resp_t)) return -1;
            net_write(NULL, in_buf, in_size);
            netctl_set_ip_resp_t *resp = (netctl_set_ip_resp_t *)out_buf;
            resp->status = NETCTL_OK;
            return (ssize_t)sizeof(*resp);
        }

        case NETCTL_CMD_PING: {
            if (in_size  < sizeof(netctl_ping_req_t))  return -1;
            if (out_size < sizeof(netctl_ping_resp_t)) return -1;
            const netctl_ping_req_t *req  = (const netctl_ping_req_t *)in_buf;
            netctl_ping_resp_t      *resp = (netctl_ping_resp_t *)out_buf;
            memset(resp, 0, sizeof(*resp));

            if (!g_netcfg.link_up) { resp->status = NETCTL_ERR_NO_LINK; return (ssize_t)sizeof(*resp); }

            if (icmp_send_echo_request(req->dst_ip, req->id, req->seq) != 0) {
                resp->status = NETCTL_ERR_TX_FAIL;
                return (ssize_t)sizeof(*resp);
            }

            if (req->timeout_ms > 0) {
                uint64_t deadline = time_ms() + req->timeout_ms;
                while (time_ms() < deadline) {
                    netctl_rx_poll_once();
                    yield();
                }
            }

            resp->status = NETCTL_OK;
            return (ssize_t)sizeof(*resp);
        }

        case NETCTL_CMD_UDP_SEND: {
            if (in_size  < sizeof(netctl_udp_send_req_t))  return -1;
            if (out_size < sizeof(netctl_udp_send_resp_t)) return -1;
            const netctl_udp_send_req_t *req  = (const netctl_udp_send_req_t *)in_buf;
            netctl_udp_send_resp_t      *resp = (netctl_udp_send_resp_t *)out_buf;

            if (!g_netcfg.link_up)              { resp->status = NETCTL_ERR_NO_LINK;  return (ssize_t)sizeof(*resp); }
            if (req->len > NETCTL_UDP_MAX_PAYLOAD) { resp->status = NETCTL_ERR_BAD_SIZE; return (ssize_t)sizeof(*resp); }

            int rc = udp_send(req->dst_ip, req->dst_port, req->src_port, req->data, req->len);
            resp->status = (rc == 0) ? NETCTL_OK : NETCTL_ERR_TX_FAIL;
            return (ssize_t)sizeof(*resp);
        }

        case NETCTL_CMD_ARP_LOOKUP: {
            if (in_size  < sizeof(netctl_arp_lookup_req_t))  return -1;
            if (out_size < sizeof(netctl_arp_lookup_resp_t)) return -1;
            const netctl_arp_lookup_req_t *req  = (const netctl_arp_lookup_req_t *)in_buf;
            netctl_arp_lookup_resp_t      *resp = (netctl_arp_lookup_resp_t *)out_buf;
            memset(resp, 0, sizeof(*resp));

            const uint8_t *mac = arp_resolve(req->ip);
            if (!mac) { resp->status = NETCTL_ERR_ARP_TIMEOUT; }
            else       { memcpy(resp->mac, mac, 6); resp->status = NETCTL_OK; }
            return (ssize_t)sizeof(*resp);
        }

        case NETCTL_CMD_ARP_FLUSH: {
            if (out_size < sizeof(netctl_arp_flush_resp_t)) return -1;
            netctl_arp_flush_resp_t *resp = (netctl_arp_flush_resp_t *)out_buf;
            memset(g_arp_cache, 0, sizeof(g_arp_cache));
            resp->status = NETCTL_OK;
            return (ssize_t)sizeof(*resp);
        }

        default: {
            if (out_size >= sizeof(int32_t)) {
                int32_t err = NETCTL_ERR_UNKNOWN_CMD;
                memcpy(out_buf, &err, sizeof(err));
            }
            return -1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   Entry point
   ═══════════════════════════════════════════════════════════════════════ */

int md_main(long argc, char **argv)
{
    (void)argc; (void)argv;

    printf("netctl Starting...\n");

    /* 1. Register $/user/net FIRST so the node exists immediately.
     *    Callbacks are safe to call before stack init — they check
     *    link_up and return NETCTL_ERR_NO_LINK if not ready yet.    */
    userfs_user_ops_t ops = {0};
    ops.read   = net_read;
    ops.write  = net_write;
    ops.invoke = net_invoke;

    userfs_user_node_t node = {0};
    node.path     = "net";
    node.owner_id = "netctl";
    node.perms    = USERFS_PERM_READ_WRITE | USERFS_PERM_INVOKE;
    node.ops      = ops;
    node.ctx      = NULL;

    int ret = userfs_register(&node);
    if (ret != 0) {
        printf("netctl: failed to register $/user/net: %d\n", ret);
        return 1;
    }

    printf("netctl: registered at $/user/net\n");

    /* 2. Open the SQRM NIC module's devfs node */
    netctl_driver_t *drv = net_driver_open("$/dev/net/net0");
    if (!drv) {
        printf("netctl: failed to open $/dev/net/net0\n");
        return 1;
    }

    /* 3. Bring up the network stack */
    ipv4_addr_t my_ip   = MAKE_IP(10,  0,  2, 15);
    ipv4_addr_t netmask = MAKE_IP(255, 255, 255,  0);
    ipv4_addr_t gateway = MAKE_IP(10,  0,  2,  1);

    if (netctl_init(drv, my_ip, netmask, gateway) != 0) {
        printf("netctl: stack init failed\n");
        return 1;
    }

    printf("netctl: stack ready. Waiting for invoke calls...\n");

    /* 4. RX loop — keep the stack alive */
    while (1) {
        if (netctl_rx_poll_once() == 0)
            yield();
    }

    return 0;
}