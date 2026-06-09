/*
 * driver.c  –  netctl interface to the SQRM NIC module
 *
 * sqrm_net_cmd_t (1508 B) and sqrm_net_reply_t (1512 B) are too large
 * for stack allocation in deeply-nested calls (e.g. the arp_announce path
 * during netctl_init).  All cmd/rep locals are therefore static — the NIC
 * channel is single-threaded and non-re-entrant by design.
 */

#define LIBC_NO_START
#include "libc.h"
#include "sqrm_eth.h"
#include "driver.h"

/* ── Per-node private state ───────────────────────────────────────────── */
typedef struct {
    int net_fd;   /* fd to the SQRM NIC module's devfs node */
} net_priv_t;

/* ── Static transact buffers (3020 bytes total, kept off the stack) ───── */
static sqrm_net_cmd_t   s_cmd;
static sqrm_net_reply_t s_rep;

/* ── Transact helper ──────────────────────────────────────────────────── */
static int sqrm_transact(int fd,
                         const sqrm_net_cmd_t  *cmd,
                               sqrm_net_reply_t *rep)
{
    if (write(fd, cmd, sizeof(*cmd)) < 0) return -1;
    if (read (fd, rep, sizeof(*rep)) < 0) return -1;
    return rep->status;
}

/* ── vtable ───────────────────────────────────────────────────────────── */
static int drv_get_mac(netctl_driver_t *drv, uint8_t out[6]) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_MAC;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep) != 0) return -1;
    if (s_rep.len < 6) return -1;
    memcpy(out, s_rep.data, 6);
    return 0;
}

static int drv_get_mtu(netctl_driver_t *drv, uint32_t *out) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_MTU;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep) != 0) return -1;
    if (s_rep.len < sizeof(uint32_t)) return -1;
    memcpy(out, s_rep.data, sizeof(uint32_t));
    return 0;
}

static int drv_get_link_up(netctl_driver_t *drv) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_LINK_UP;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep) != 0) return -1;
    if (s_rep.len < sizeof(uint32_t)) return -1;
    uint32_t val; memcpy(&val, s_rep.data, sizeof(val));
    return (int)val;
}

static int drv_tx(netctl_driver_t *drv, const uint8_t *frame, size_t len) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    if (len == 0 || len > sizeof(s_cmd.data)) return -1;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_TX_FRAME;
    s_cmd.len = (uint32_t)len;
    memcpy(s_cmd.data, frame, len);
    return sqrm_transact(p->net_fd, &s_cmd, &s_rep);
}

static int drv_rx_poll(netctl_driver_t *drv,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    net_priv_t *p = (net_priv_t *)drv->priv;
    *out_len = 0;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_RX_POLL;
    int rc = sqrm_transact(p->net_fd, &s_cmd, &s_rep);
    if (rc != 0) return rc;
    if (s_rep.len == 0) return 0;
    size_t copy = s_rep.len < buf_len ? s_rep.len : buf_len;
    memcpy(buf, s_rep.data, copy);
    *out_len = copy;
    return 0;
}

/* ── Public factory ───────────────────────────────────────────────────── */
netctl_driver_t *net_driver_open(const char *dev_path) {
    int fd = open(dev_path, O_RDWR, 0);
    if (fd < 0) {
        printf("[driver] cannot open NIC node: %s\n", dev_path);
        return NULL;
    }

    net_priv_t *priv = (net_priv_t *)malloc(sizeof(net_priv_t));
    if (!priv) { close(fd); return NULL; }
    priv->net_fd = fd;

    netctl_driver_t *drv = (netctl_driver_t *)malloc(sizeof(netctl_driver_t));
    if (!drv) { free(priv); close(fd); return NULL; }

    drv->get_mac     = drv_get_mac;
    drv->get_mtu     = drv_get_mtu;
    drv->get_link_up = drv_get_link_up;
    drv->tx          = drv_tx;
    drv->rx_poll     = drv_rx_poll;
    drv->priv        = priv;

    return drv;
}