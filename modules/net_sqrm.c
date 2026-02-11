#include <stdint.h>
#include <stddef.h>

#include "moduos/kernel/sqrm.h"
#include "moduos/kernel/errno.h"

/*
 * net_sqrm.c
 *
 * Network SQRM skeleton.
 *
 * Purpose: export a stable network API surface to the kernel/other modules via
 * sqrm_service_register("net", ...).
 *
 * This does NOT implement actual networking.
 */

// NOTE: This module is a *network device* (NIC) skeleton.
// It should only expose link-layer (L2) functionality:
//   - link status
//   - MAC/MTU
//   - send/receive raw Ethernet frames
// Higher-level protocols (ARP/IP/TCP/UDP/DNS/HTTP) MUST live in userland (or a separate
// userland-managed "net stack" service).

// sqrm_net_api_v1_t is defined in moduos/kernel/sqrm.h

static int net_get_link_up(void) { return 0; }
static int net_get_mtu(uint32_t *out) { if (out) *out = 1500; return 0; }
static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return -EINVAL;
    // Locally-administered dummy MAC for skeleton.
    out_mac[0] = 0x02; out_mac[1] = 0x00; out_mac[2] = 0x00;
    out_mac[3] = 0x00; out_mac[4] = 0x00; out_mac[5] = 0x01;
    return 0;
}
static int net_tx_frame(const void *frame, size_t len) { (void)frame; (void)len; return -ENOSYS; }
static int net_rx_poll(void *out_frame, size_t out_cap, size_t *out_len) {
    (void)out_frame; (void)out_cap;
    if (out_len) *out_len = 0;
    return 0; // no frame
}
static int net_rx_consume(void) { return 0; }

static const sqrm_net_api_v1_t g_net_api = {
    .get_link_up = net_get_link_up,
    .get_mtu = net_get_mtu,
    .get_mac = net_get_mac,
    .tx_frame = net_tx_frame,
    .rx_poll = net_rx_poll,
    .rx_consume = net_rx_consume,
};

SQRM_DEFINE_MODULE_V2(SQRM_TYPE_NET, "net", 1, 0, 0, NULL);

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != 1) return -1;

    if (api->com_write_string) {
        api->com_write_string(0x3F8, "[NET] net loaded (skeleton)\n");
    }

    if (api->sqrm_service_register) {
        int r = api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
        if (api->com_write_string) {
            api->com_write_string(0x3F8, r == 0 ? "[NET] exported service: net\n" : "[NET] failed to export service: net\n");
        }
    }

    return 0;
}
