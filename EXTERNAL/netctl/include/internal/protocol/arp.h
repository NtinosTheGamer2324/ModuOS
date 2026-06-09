#ifndef NETCTL_PROTOCOL_ARP_H
#define NETCTL_PROTOCOL_ARP_H

/*
 * arp.h  –  ARP (RFC 826) definitions and API
 */

#include <stdint.h>
#include <stddef.h>

/* ── Constants ───────────────────────────────────────────────────────── */
#define ARP_ETHERNET  0x0001u
#define ARP_IPV4      0x0800u
#define ARP_OP_REQ    0x0001u
#define ARP_OP_REPLY  0x0002u

/* Alias matching convention used by arp.c */
#define ARP_OP_REQUEST ARP_OP_REQ

/* ── ARP Header ──────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t hw_type;
    uint16_t prot_type;
    uint8_t  hw_len;
    uint8_t  prot_len;
    uint16_t opcode;
    uint8_t  src_mac[6];
    uint32_t src_ip;
    uint8_t  dst_mac[6];
    uint32_t dst_ip;
} arp_header_t;

/* ── Full Ethernet-framed ARP packet ─────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t      dst_mac[6];
    uint8_t      src_mac[6];
    uint16_t     ethertype;  /* 0x0806 */
    arp_header_t arp;
} arp_packet_t;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Register ARP with the netctl ethertype dispatch table. */
void arp_register(void);

/*
 * RX handler — called by the dispatch table; not normally called directly.
 * Handles incoming ARP requests (reply with our MAC) and replies (populate cache).
 */
void arp_rx(const uint8_t *frame, size_t len);

/*
 * Send a gratuitous ARP to announce our IP/MAC mapping to all hosts.
 * Called automatically on init, but can be re-issued after an IP change.
 */
void arp_announce(void);

/*
 * Resolve an IPv4 address to a MAC address.
 * Checks the ARP cache first; if not found, sends an ARP request and polls
 * for up to ~500 ms.
 * Returns a pointer to a 6-byte MAC (valid until next cache eviction),
 * or NULL on timeout.
 */
const uint8_t *arp_resolve(uint32_t ip);

#endif /* NETCTL_PROTOCOL_ARP_H */