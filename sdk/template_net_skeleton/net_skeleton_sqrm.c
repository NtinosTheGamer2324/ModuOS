#include "../sqrm_sdk.h"

/*
 * Network SQRM module skeleton.
 *
 * This is a TEMPLATE for authors writing NIC (netdev) modules.
 * It does not implement networking.
 *
 * Recommended approach for real drivers:
 *   - Detect hardware (PCI: class 0x02)
 *   - Map MMIO BARs with api->ioremap
 *   - Set up DMA rings, interrupts, and rx/tx queues
 *   - Expose an L2 interface (raw Ethernet frames) via sqrm_service_register("net", ...)
 *
 * IMPORTANT LAYERING RULE:
 *   This module should NOT implement IP/TCP/UDP/DNS/HTTP.
 *   Those belong in userland (/ModuOS/System64/services/*.csv) on top of the netdev service.
 *
 * NOTE:
 *   This template returns -1 so that the kernel autoload process can
 *   continue searching for real network drivers.
 */

SQRM_DEFINE_MODULE(SQRM_TYPE_NET, "net_skel");

static const uint16_t COM1_PORT = 0x3F8;

static int net_get_link_up(void) { return 0; }
static int net_get_mtu(uint32_t *out) { if (out) *out = 1500; return 0; }
static int net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return -22; /* -EINVAL */
    out_mac[0] = 0x02; out_mac[1] = 0x00; out_mac[2] = 0x00;
    out_mac[3] = 0x00; out_mac[4] = 0x00; out_mac[5] = 0x01;
    return 0;
}

static int net_tx_frame(const void *frame, size_t len) {
    (void)frame; (void)len;
    return -38; /* -ENOSYS */
}

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

int sqrm_module_init(const sqrm_kernel_api_t *api) {
    if (!api || api->abi_version != SQRM_ABI_VERSION) return -1;

    if (api->com_write_string) {
        api->com_write_string(COM1_PORT, "[net_skel] loaded (skeleton)\n");
        api->com_write_string(COM1_PORT, "[net_skel] NOTE: not binding hardware; returning -1 so autoload continues\n");
    }

    if (api->sqrm_service_register) {
        (void)api->sqrm_service_register("net", &g_net_api, sizeof(g_net_api));
    }

    /*
     * Real driver would typically:
     *  - scan PCI for NICs and claim one
     *  - create device nodes with api->devfs_register_path("net/eth0", ...)
     *  - start RX/TX
     *  - export a full-featured network API via sqrm_service_register("net", ...)
     */

    // keep returning -1 by default so autoload continues
    return -1;
}
