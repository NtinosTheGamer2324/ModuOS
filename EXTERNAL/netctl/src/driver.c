/*
 * driver.c  –  netctl interface to the SQRM NIC module
 */

#define LIBC_NO_START
#include "libc.h"
#include "sqrm_eth.h"
#include "driver.h"

typedef struct {
    int net_fd;
} net_priv_t;

/* ── Static transact buffers (kept off the stack) ─────────────────────── */
static sqrm_net_cmd_t   s_cmd;
static sqrm_net_reply_t s_rep;

/* ── How many bytes to write for a given command ──────────────────────── */
static size_t cmd_write_size(uint32_t cmd_code, uint32_t payload_len)
{
    /* Header is cmd(4) + len(4) = 8 bytes.
     * Only TX_FRAME carries a data payload. */
    if (cmd_code == NET_CMD_TX_FRAME)
        return 8 + payload_len;
    return 8;
}

/* ── Reliable read: loop until we have at least `need` bytes ──────────── */
static int read_full(int fd, void *buf, size_t need)
{
    size_t got = 0;
    uint8_t *p = (uint8_t *)buf;
    while (got < need) {
        ssize_t n = read(fd, p + got, need - got);
        if (n < 0) return -1;   /* hard error  */
        if (n == 0) break;      /* EOF / empty */
        got += (size_t)n;
    }
    return (int)got;
}

/* ── Minimum reply size: header only (cmd + status + len) ─────────────── */
#define REP_HDR_SIZE  (sizeof(uint32_t) + sizeof(int32_t) + sizeof(uint32_t))

/* ── Transact helper ──────────────────────────────────────────────────── */
static int sqrm_transact(int fd,
                         const sqrm_net_cmd_t  *cmd,
                               sqrm_net_reply_t *rep,
                               size_t            write_len)
{
    if (write(fd, cmd, write_len) < 0) return -1;

    memset(rep, 0, sizeof(*rep));
    uint8_t *p   = (uint8_t *)rep;
    size_t   got = 0;
    size_t   max = sizeof(*rep) + 1;  /* header + data + null terminator */

    while (got < max) {
        ssize_t n = read(fd, p + got, max - got);
        if (n < 0) return -1;
        if (n == 0) { yield(); continue; }  /* not ready yet, retry */

        got += (size_t)n;

        /* stop as soon as we see the null terminator */
        if (p[got - 1] == 0) break;
    }

    /* must have at least the 12-byte header */
    if (got < 12) return -1;

    return rep->status;
}
/* ── vtable ───────────────────────────────────────────────────────────── */
static int drv_get_mac(netctl_driver_t *drv, uint8_t out[6]) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_MAC;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep, cmd_write_size(NET_CMD_GET_MAC, 0)) != 0) return -1;
    if (s_rep.len < 6) return -1;
    memcpy(out, s_rep.data, 6);
    return 0;
}

static int drv_get_mtu(netctl_driver_t *drv, uint32_t *out) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_MTU;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep, cmd_write_size(NET_CMD_GET_MTU, 0)) != 0) return -1;
    if (s_rep.len < sizeof(uint32_t)) return -1;
    memcpy(out, s_rep.data, sizeof(uint32_t));
    return 0;
}

static int drv_get_link_up(netctl_driver_t *drv) {
    net_priv_t *p = (net_priv_t *)drv->priv;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_GET_LINK_UP;
    if (sqrm_transact(p->net_fd, &s_cmd, &s_rep, cmd_write_size(NET_CMD_GET_LINK_UP, 0)) != 0) return -1;
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
    return sqrm_transact(p->net_fd, &s_cmd, &s_rep, cmd_write_size(NET_CMD_TX_FRAME, (uint32_t)len));
}

static int drv_rx_poll(netctl_driver_t *drv,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    net_priv_t *p = (net_priv_t *)drv->priv;
    *out_len = 0;
    memset(&s_cmd, 0, sizeof(s_cmd));
    s_cmd.cmd = NET_CMD_RX_POLL;
    int rc = sqrm_transact(p->net_fd, &s_cmd, &s_rep, cmd_write_size(NET_CMD_RX_POLL, 0));
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