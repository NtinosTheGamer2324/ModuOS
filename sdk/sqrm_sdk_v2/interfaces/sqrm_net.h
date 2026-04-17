#pragma once
/*
 * sqrm_net.h — SQRM L2 NIC service ABI (v1).
 *
 * Available to: NET modules.
 * Register the API via api->sqrm_service_register("sqrm_net_v1", ...).
 * Other modules can retrieve it via api->sqrm_service_get("sqrm_net_v1", ...).
 *
 * This is a raw NIC (L2) API only.
 * Higher-level networking (DHCP, DNS, TCP/IP, HTTP, etc.) is out of scope.
 *
 * All functions return 0 on success or a negative errno on failure.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Link state */
    int (*get_link_up)(void);

    /* MTU in bytes written to *out */
    int (*get_mtu)(uint32_t *out);

    /* MAC address written to out_mac[6] */
    int (*get_mac)(uint8_t out_mac[6]);

    /* Transmit a raw Ethernet frame of `len` bytes */
    int (*tx_frame)(const void *frame, size_t len);

    /*
     * rx_poll() — non-blocking receive.
     * Copies the next pending frame into out_frame (capacity out_cap).
     * Sets *out_len to the actual frame length.
     * Returns 1 if a frame was available, 0 if none, negative on error.
     */
    int (*rx_poll)(void *out_frame, size_t out_cap, size_t *out_len);

    /* rx_consume() — release the current RX descriptor back to hardware */
    int (*rx_consume)(void);
} sqrm_net_api_v1_t;

#ifdef __cplusplus
}
#endif