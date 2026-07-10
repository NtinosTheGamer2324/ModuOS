/*
 * example_client.c  –  Example app that uses the netctl service
 *
 * Compile and run separately from netctl itself.
 * Only needs: libc.h + netctl_api.h
 *
 * Usage:
 *   The netctl service must already be running (registered at $/user/net).
 */

#include "../include/libc.h"
#include "../include/netctl_api.h"
    
int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    int fd = open("$/user/net", O_RDWR, 0);
    if (fd < 0) {
        printf("Failed to open $/user/net — is netctl running?\n");
        return 1;
    }
    printf("Connected to netctl service.\n");

    /* ── Query network info ─────────────────────────────────────────── */
    {
        netctl_get_info_req_t  req  = { .cmd = NETCTL_CMD_GET_INFO };
        netctl_get_info_resp_t resp = {0};

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0
                && resp.status == NETCTL_OK) {
            printf("MAC  : %02x:%02x:%02x:%02x:%02x:%02x\n",
                   resp.mac[0], resp.mac[1], resp.mac[2],
                   resp.mac[3], resp.mac[4], resp.mac[5]);
            printf("IP   : %d.%d.%d.%d\n",
                   (resp.ip >> 24) & 0xFF, (resp.ip >> 16) & 0xFF,
                   (resp.ip >>  8) & 0xFF,  resp.ip        & 0xFF);
            printf("Link : %s\n", resp.link_up ? "up" : "down");
            printf("MTU  : %u\n", resp.mtu);
        }
    }

    /* ── Ping the gateway ───────────────────────────────────────────── */
/*
    {
        netctl_ping_req_t req = {
            .cmd        = NETCTL_CMD_PING,
            .dst_ip     = (10u << 24) | (0u << 16) | (2u << 8) | 1u,
            .id         = 1,
            .seq        = 1,
            .timeout_ms = 500,
        };
        netctl_ping_resp_t resp = {0};

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0) {
            if (resp.status == NETCTL_OK)
                printf("Ping: OK (rtt ~%u ms)\n", resp.rtt_ms);
            else
                printf("Ping: failed (status %d)\n", resp.status);
        }
    }
*/

    /* ── Send a UDP packet ──────────────────────────────────────────── */
    {
        /* netctl_udp_send_req_t is >1 KiB (data[1024] + header).
         * Declaring it as 'static' keeps it out of the limited user
         * stack and prevents a stack-overflow page fault.           */
        static netctl_udp_send_req_t req;
        memset(&req, 0, sizeof(req));
        req.cmd      = NETCTL_CMD_UDP_SEND;
        req.dst_ip   = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
        req.dst_port = 1234;
        req.src_port = 5000;

        const char *msg = "hello from ModuOS";
        req.len = strlen(msg);
        memcpy(req.data, msg, req.len);

        netctl_udp_send_resp_t resp = {0};
        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0) {
            printf("UDP send: %s\n",
                   resp.status == NETCTL_OK ? "OK" : "failed");
        }
    }

    /* ── ARP lookup ─────────────────────────────────────────────────── */
    {
        netctl_arp_lookup_req_t req = {
            .cmd = NETCTL_CMD_ARP_LOOKUP,
            .ip  = (10u << 24) | (0u << 16) | (2u << 8) | 1u,
        };
        netctl_arp_lookup_resp_t resp = {0};

        if (invoke(fd, &req, sizeof(req), &resp, sizeof(resp)) > 0) {
            if (resp.status == NETCTL_OK)
                printf("ARP  : %02x:%02x:%02x:%02x:%02x:%02x\n",
                       resp.mac[0], resp.mac[1], resp.mac[2],
                       resp.mac[3], resp.mac[4], resp.mac[5]);
            else
                printf("ARP  : no reply\n");
        }
    }

    close(fd);
    return 0;
}