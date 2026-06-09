#ifndef NETCTL_H
#define NETCTL_H

/*
 * netctl.h  –  ModuOS userland network manager (core API)
 *
 * Architecture
 * ───────────
 *  netctl_init()  opens the devfs net node, reads MAC, configures IP.
 *  netctl_run()   is the RX poll loop.  It calls netctl_rx_dispatch()
 *                 on every received Ethernet frame.
 *
 *  Each protocol (ARP, IP, …) registers a proto_handler_t.
 *  Adding a new protocol = fill one struct + call netctl_proto_register().
 *
 * Endianness
 * ──────────
 *  All "on-wire" fields (ethertype, IP addresses stored in headers) are
 *  big-endian.  Host-side storage (netctl_cfg_t.ip, ARP cache) is also
 *  stored in network byte order so we can memcmp / memcpy directly.
 *  Use the htons/ntohs/htonl/ntohl macros below.
 */

#include <stdint.h>
#include <stddef.h>

/* ── Byte-order helpers (we're always on x86 / little-endian) ─────────── */
#define htons(x)  ((uint16_t)(((uint16_t)(x) >> 8) | ((uint16_t)(x) << 8)))
#define ntohs(x)  htons(x)
#define htonl(x)  ( (((uint32_t)(x) & 0x000000FFu) << 24) \
                  | (((uint32_t)(x) & 0x0000FF00u) <<  8) \
                  | (((uint32_t)(x) & 0x00FF0000u) >>  8) \
                  | (((uint32_t)(x) & 0xFF000000u) >> 24) )
#define ntohl(x)  htonl(x)

/* ── Ethernet constants ───────────────────────────────────────────────── */
#define ETH_ALEN          6
#define ETH_HDR_LEN       14
#define ETH_MAX_PAYLOAD   1500
#define ETH_MAX_FRAME     (ETH_HDR_LEN + ETH_MAX_PAYLOAD)

#define ETHERTYPE_ARP     0x0806u
#define ETHERTYPE_IP      0x0800u
#define ETHERTYPE_IPV6    0x86DDu

/* Forward declaration — full definition in driver.h */
struct netctl_driver;

/* ── Ethernet frame header (in-memory, host layout) ──────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;   /* network byte order */
} eth_hdr_t;

/* ── IP address helpers ───────────────────────────────────────────────── */
/* Stored in network byte order (big-endian uint32). */
typedef uint32_t ipv4_addr_t;

/* Make an IPv4 address from dotted-quad octets (result is network-endian). */
#define MAKE_IP(a,b,c,d) \
    ( ((uint32_t)(a) << 24) \
    | ((uint32_t)(b) << 16) \
    | ((uint32_t)(c) <<  8) \
    | ((uint32_t)(d)      ) )

/* Broadcast MAC */
static const uint8_t ETH_BROADCAST[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── Network configuration ────────────────────────────────────────────── */
typedef struct {
    uint8_t     mac[ETH_ALEN];   /* our MAC (filled by netctl_init)       */
    ipv4_addr_t ip;              /* our IPv4 (network byte order)         */
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;
    int         link_up;
    uint32_t    mtu;
    /* NOTE: no fd here — the driver owns all hardware handles */
} netctl_cfg_t;

/* Global config – defined in netctl.c, used by protocol modules. */
extern netctl_cfg_t g_netcfg;

/* ── ARP cache ────────────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE  16

typedef struct {
    ipv4_addr_t ip;
    uint8_t     mac[ETH_ALEN];
    int         valid;
} arp_entry_t;

extern arp_entry_t g_arp_cache[ARP_CACHE_SIZE];

/* Look up an IP in the cache.  Returns pointer to MAC or NULL. */
const uint8_t *arp_cache_lookup(ipv4_addr_t ip);

/* Insert / update an entry. */
void arp_cache_update(ipv4_addr_t ip, const uint8_t *mac);

/* ── Protocol handler vtable ──────────────────────────────────────────── */
/*
 * To add a new protocol:
 *   1.  Fill a proto_handler_t (ethertype + rx callback).
 *   2.  Call netctl_proto_register() from your init function.
 *   Done.
 *
 * rx() receives a pointer to the full Ethernet frame and its total length.
 * The ethertype in the Ethernet header is already in HOST byte order when
 * rx() is called (netctl_rx_dispatch normalises it).
 */
typedef struct {
    uint16_t  ethertype;           /* host byte order, e.g. ETHERTYPE_ARP  */
    const char *name;              /* for debug printing                   */
    void (*rx)(const uint8_t *frame, size_t len);
} proto_handler_t;

#define NETCTL_MAX_PROTOS  8

/* Register a protocol handler.  Returns 0 on success, -1 if table is full. */
int netctl_proto_register(const proto_handler_t *h);

/* ── Raw Ethernet TX ──────────────────────────────────────────────────── */
/*
 * Send a raw Ethernet frame.  The caller fills dst_mac + ethertype (host
 * byte order); netctl_eth_send fills src_mac and byte-swaps ethertype.
 * payload + payload_len is the layer-3 data.
 * Returns 0 on success, negative on error.
 */
int netctl_eth_send(const uint8_t *dst_mac,
                    uint16_t       ethertype,
                    const void    *payload,
                    size_t         payload_len);

/* ── RX dispatch ──────────────────────────────────────────────────────── */
/* Called by netctl_run() for each received frame. */
void netctl_rx_dispatch(const uint8_t *frame, size_t len);

/* ── Lifecycle ────────────────────────────────────────────────────────── */
/*
 * netctl_init  – attach a driver, query MAC/MTU/link, set static IP,
 *                register built-in protocols.
 *                The driver pointer must remain valid for the process lifetime.
 * netctl_run   – RX poll loop; returns only on unrecoverable error.
 */
int  netctl_init(struct netctl_driver *drv,
                 ipv4_addr_t ip,
                 ipv4_addr_t netmask,
                 ipv4_addr_t gateway);

void netctl_run(void);

/* Poll the driver once and dispatch any received frame.
 * Used by arp_resolve() to pump the RX path while waiting for a reply.
 * Returns 1 if a frame was received, 0 if nothing, -1 on error.
 */
int  netctl_rx_poll_once(void);

/* ── Debug helpers ────────────────────────────────────────────────────── */
void netctl_print_mac(const uint8_t *mac);
void netctl_print_ip(ipv4_addr_t ip);

#endif /* NETCTL_H */