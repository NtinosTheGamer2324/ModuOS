#ifndef SQRM_ETH_H
#define SQRM_ETH_H

/*
 * sqrm_eth.h  –  Userland wire protocol for SQRM NIC devfs nodes
 *
 * The SQRM kernel subsystem loads NIC modules (ring 0) and exposes each
 * one as a devfs node at $/dev/net/netN.  Userland talks to that node
 * using plain write() / read() with the structs defined here.
 *
 * Kernel-side implementation: sqrm_net_devfs_write() / _read() in the
 * SQRM net glue layer.  That code is ring-0 only and is NOT included here.
 *
 * Usage from userland (driver.c):
 *
 *   sqrm_net_cmd_t cmd = {0};
 *   cmd.cmd = NET_CMD_GET_MAC;
 *   write(fd, &cmd, sizeof(cmd));
 *
 *   sqrm_net_reply_t rep = {0};
 *   read(fd, &rep, sizeof(rep));
 *   // rep.data[0..5] now contains the MAC address
 *
 * Place this file at:
 *   netctl/include/internal/sqrm_eth.h
 */

#include <stdint.h>

/* ── Command codes (written by userland, dispatched by kernel) ────────── */
#define NET_CMD_GET_MODE     1u   /* returns NET_MODE_* in rep.data        */
#define NET_CMD_GET_LINK_UP  2u   /* returns uint32_t 1/0 in rep.data      */
#define NET_CMD_GET_MTU      3u   /* returns uint32_t MTU in rep.data      */
#define NET_CMD_GET_MAC      4u   /* returns 6 MAC bytes in rep.data       */
#define NET_CMD_TX_FRAME     5u   /* cmd.data[0..cmd.len] = frame to send  */
#define NET_CMD_RX_POLL      6u   /* rep.data[0..rep.len] = received frame */

/* ── Mode values (returned by NET_CMD_GET_MODE) ───────────────────────── */
#define NET_MODE_ETH         1u
#define NET_MODE_WIFI        2u

/* ── Command struct (userland → kernel, via write()) ─────────────────── */
typedef struct {
    uint32_t cmd;           /* NET_CMD_*                                   */
    uint32_t len;           /* payload length in data[] (TX only)          */
    uint8_t  data[1500];    /* frame data for NET_CMD_TX_FRAME, else unused */
} sqrm_net_cmd_t;

/* ── Reply struct (kernel → userland, via read()) ────────────────────── */
typedef struct {
    uint32_t cmd;           /* echoes the command that produced this reply */
    int32_t  status;        /* 0 = success, negative = error               */
    uint32_t len;           /* valid bytes in data[]                       */
    uint8_t  data[1500];    /* reply payload (MAC, MTU, RX frame, …)       */
} sqrm_net_reply_t;

#endif /* SQRM_ETH_H */