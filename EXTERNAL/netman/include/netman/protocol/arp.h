#pragma once
#include "libc.h"

/* ── ARP EtherType / Hardware type ───────────────────────────────────────── */
#define ARP_HTYPE_ETHERNET  1
#define ARP_PTYPE_IPV4      0x0800
#define ARP_HLEN_ETHERNET   6
#define ARP_PLEN_IPV4       4

#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

/* ── Wire-format ARP packet (Ethernet/IPv4) ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t htype;         /* Hardware type  */
    uint16_t ptype;         /* Protocol type  */
    uint8_t  hlen;          /* Hardware addr length */
    uint8_t  plen;          /* Protocol addr length */
    uint16_t oper;          /* Operation: request / reply */
    uint8_t  sha[6];        /* Sender hardware address */
    uint8_t  spa[4];        /* Sender protocol address */
    uint8_t  tha[6];        /* Target hardware address */
    uint8_t  tpa[4];        /* Target protocol address */
} arp_packet_t;

/* ── ARP cache entry ─────────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE      16
#define ARP_ENTRY_TIMEOUT_MS 30000   /* 30 s */

typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint64_t last_seen_ms;
    int      valid;
} arp_entry_t;

/* ── ARP context (passed to all arp_ functions) ──────────────────────────── */
typedef struct {
    int        net_fd;           /* open fd to $/dev/net/net0 */
    uint8_t    mac[6];           /* our MAC */
    uint8_t    ip[4];            /* our IP  */
    arp_entry_t cache[ARP_CACHE_SIZE];
} arp_ctx_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialise context (does NOT open the fd – caller owns it). */
void arp_init(arp_ctx_t *ctx, int net_fd,
              const uint8_t mac[6], const uint8_t ip[4]);

/* Send an ARP request asking "who has target_ip?". */
int  arp_send_request(arp_ctx_t *ctx, const uint8_t target_ip[4]);

/* Send an ARP reply to a received request packet. */
int  arp_send_reply(arp_ctx_t *ctx, const arp_packet_t *req);

/* Process one inbound ARP packet (updates cache, sends reply if needed). */
void arp_handle(arp_ctx_t *ctx, const uint8_t *frame, uint32_t len);

/* Look up MAC for ip (NULL if not in cache). */
const uint8_t *arp_lookup(arp_ctx_t *ctx, const uint8_t ip[4]);

/* Insert / refresh cache entry manually. */
void arp_cache_update(arp_ctx_t *ctx,
                      const uint8_t ip[4], const uint8_t mac[6]);

/* Evict stale entries older than ARP_ENTRY_TIMEOUT_MS. */
void arp_cache_tick(arp_ctx_t *ctx, uint64_t now_ms);

/* Dump cache to stdout (debug). */
void arp_cache_print(const arp_ctx_t *ctx);