#ifndef NETCTL_DRIVER_H
#define NETCTL_DRIVER_H

/*
 * driver.h  –  netctl HAL vtable
 *
 * netctl_driver_t abstracts the NIC hardware from the network stack.
 * driver.c implements it by talking to the SQRM NIC module's devfs node
 * ($/dev/net/netN) via sqrm_net_cmd_t / sqrm_net_reply_t.
 *
 * Nothing above this (netctl.c, protocol modules) touches devfs or SQRM.
 */

#include <stdint.h>
#include <stddef.h>

typedef struct netctl_driver {
    int  (*get_mac)    (struct netctl_driver *drv, uint8_t out[6]);
    int  (*get_mtu)    (struct netctl_driver *drv, uint32_t *out);
    int  (*get_link_up)(struct netctl_driver *drv);
    int  (*tx)         (struct netctl_driver *drv, const uint8_t *frame, size_t len);
    int  (*rx_poll)    (struct netctl_driver *drv, uint8_t *buf, size_t buf_len, size_t *out_len);
    void *priv;
} netctl_driver_t;

/*
 * net_driver_open  –  open the SQRM NIC module's devfs node at `dev_path`
 *                     and return a ready-to-use netctl_driver_t.
 *                     Returns NULL if the node can't be opened.
 */
netctl_driver_t *net_driver_open(const char *dev_path);

#endif /* NETCTL_DRIVER_H */