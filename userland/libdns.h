// libdns.h -- universal DNS resolver for ModuOS apps (header-only, like sockets.h)
//
// Resolves hostnames to IPv4 addresses by sending a hand-built DNS query over
// the sockets.h UDP API (which is known to work), so every app can share one
// resolver instead of carrying its own copy.
//
//   #include "libdns.h"
//
//   uint8_t ip[4];
//   if (dns_resolve("example.com", ip) != 0)
//       printf("dns: %s\n", dns_strerror(dns_last_error()));
//
//   char ipstr[16];
//   dns_resolve_str("example.com", ipstr, sizeof ipstr);   // "93.184.216.34"
//
//   int fd = dns_create_connection("example.com", 80, 5000);  // resolve + connect
//
// Behaviour:
//  * Dotted quads pass straight through (no network traffic).
//  * Default servers are 8.8.8.8 then 1.1.1.1. Change them with
//    dns_set_server() / dns_add_server() / dns_reset_servers().
//  * Each server is tried in turn, for DNS_ROUNDS rounds, DNS_TIMEOUT_MS each.
//    NXDOMAIN / "no A record" answers are definitive and stop the search.
//  * CNAME chains are followed implicitly: the first A record in the answer
//    section is used.
//  * Successful lookups are cached for the lifetime of the process (apps here
//    are short-lived). Disable with `#define DNS_CACHE 0` before including,
//    or call dns_cache_clear().
//  * Like sockets.h this is single-threaded and uses one static buffer pair.
//
// Functions return 0 on success and -1 on failure; dns_last_error() then gives
// one of the DNS_E_* codes and errno is set to something sensible.

#pragma once
#include "sockets.h"

#define DNS_MAX_SERVERS   4
#define DNS_TIMEOUT_MS    2500
#define DNS_ROUNDS        2
#define DNS_CACHE_SIZE    8
#define DNS_PORT          53
#ifndef DNS_CACHE
#define DNS_CACHE         1
#endif

enum {
    DNS_OK          = 0,
    DNS_E_BADNAME   = 1,   // empty / too long / malformed host name
    DNS_E_NETDOWN   = 2,   // netd not running or network down
    DNS_E_TIMEOUT   = 3,   // no usable reply from any server
    DNS_E_NXDOMAIN  = 4,   // the name does not exist
    DNS_E_SERVFAIL  = 5,   // server reported a failure (and no other server helped)
    DNS_E_NOADDR    = 6,   // name exists but has no IPv4 (A) record
    DNS_E_MALFORMED = 7    // garbled reply
};

// ── State ───────────────────────────────────────────────────────────
typedef struct {
    uint8_t used;
    char    name[64];
    uint8_t ip[4];
} __dns_cache_t;

static uint8_t       __dns_servers[DNS_MAX_SERVERS][4];
static int           __dns_nservers = 0;
static int           __dns_err = DNS_OK;
static uint16_t      __dns_next_id = 0x1337;
static uint8_t       __dns_q[512];      // static: keeps 1 KB off the user stack
static uint8_t       __dns_r[512];
static __dns_cache_t __dns_cache[DNS_CACHE_SIZE];
static int           __dns_cache_next = 0;

static inline int dns_last_error(void) { return __dns_err; }

static inline const char *dns_strerror(int code) {
    switch (code) {
        case DNS_OK:          return "ok";
        case DNS_E_BADNAME:   return "invalid host name";
        case DNS_E_NETDOWN:   return "network is down";
        case DNS_E_TIMEOUT:   return "DNS server did not answer (timed out)";
        case DNS_E_NXDOMAIN:  return "no such domain (NXDOMAIN)";
        case DNS_E_SERVFAIL:  return "DNS server failure (SERVFAIL)";
        case DNS_E_NOADDR:    return "host has no IPv4 address";
        case DNS_E_MALFORMED: return "malformed DNS reply";
    }
    return "unknown DNS error";
}

// ── Server list ─────────────────────────────────────────────────────
static inline void dns_reset_servers(void) { __dns_nservers = 0; }   // back to defaults

static inline void dns_add_server(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    if (__dns_nservers >= DNS_MAX_SERVERS) return;
    __dns_servers[__dns_nservers][0] = a; __dns_servers[__dns_nservers][1] = b;
    __dns_servers[__dns_nservers][2] = c; __dns_servers[__dns_nservers][3] = d;
    __dns_nservers++;
}

// Replace the server list with a single server.
static inline void dns_set_server(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    __dns_nservers = 0;
    dns_add_server(a, b, c, d);
}

static inline void __dns_init_servers(void) {
    if (__dns_nservers > 0) return;
    dns_add_server(8, 8, 8, 8);
    dns_add_server(1, 1, 1, 1);
}

// ── Cache ───────────────────────────────────────────────────────────
static inline void dns_cache_clear(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) __dns_cache[i].used = 0;
    __dns_cache_next = 0;
}

static inline int __dns_ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// ── Packet building / parsing ───────────────────────────────────────
// Builds an A/IN query. Returns the length, or -1 for a bad name.
static inline int __dns_build_query(const char *name, uint16_t id, uint8_t *out, int cap) {
    int n = 0;
    if (cap < 18) return -1;
    out[n++] = (uint8_t)(id >> 8); out[n++] = (uint8_t)id;
    out[n++] = 0x01; out[n++] = 0x00;                // flags: RD
    out[n++] = 0; out[n++] = 1;                      // QDCOUNT = 1
    for (int i = 0; i < 6; i++) out[n++] = 0;        // AN/NS/AR = 0
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len == 0 || len > 63) return -1;
        if (n + 1 + len + 5 > cap) return -1;
        out[n++] = (uint8_t)len;
        memcpy(out + n, p, (size_t)len);
        n += len;
        p = dot;
        if (*p == '.') p++;                          // also allows a trailing dot
    }
    out[n++] = 0;
    out[n++] = 0; out[n++] = 1;                      // QTYPE  = A
    out[n++] = 0; out[n++] = 1;                      // QCLASS = IN
    return n;
}

// Bounds-checked: returns the offset just past the name, or -1.
static inline int __libdns_skip_name(const uint8_t *b, int len, int pos) {
    while (pos < len) {
        uint8_t c = b[pos];
        if (c == 0) return pos + 1;
        if ((c & 0xC0) == 0xC0) return (pos + 2 <= len) ? pos + 2 : -1;
        if (c & 0xC0) return -1;
        pos += 1 + c;
    }
    return -1;
}

// Returns DNS_OK and fills ip, or a DNS_E_* code.
static inline int __libdns_parse(const uint8_t *b, int len, uint16_t id,
                                 uint8_t ip[4]) {
    if (len < 12) return DNS_E_MALFORMED;
    if ((uint16_t)((b[0] << 8) | b[1]) != id) return DNS_E_MALFORMED;
    if (!(b[2] & 0x80)) return DNS_E_MALFORMED;      // not a response
    int rcode = b[3] & 0x0F;
    if (rcode == 3) return DNS_E_NXDOMAIN;
    if (rcode != 0) return DNS_E_SERVFAIL;

    int qd = (b[4] << 8) | b[5];
    int an = (b[6] << 8) | b[7];
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        pos = __libdns_skip_name(b, len, pos);
        if (pos < 0 || pos + 4 > len) return DNS_E_MALFORMED;
        pos += 4;
    }
    for (int i = 0; i < an; i++) {
        pos = __libdns_skip_name(b, len, pos);
        if (pos < 0 || pos + 10 > len) return DNS_E_MALFORMED;
        int type  = (b[pos] << 8) | b[pos + 1];
        int rdlen = (b[pos + 8] << 8) | b[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return DNS_E_MALFORMED;
        if (type == 1 && rdlen == 4) { memcpy(ip, b + pos, 4); return DNS_OK; }
        pos += rdlen;                                // CNAME etc: skip
    }
    return DNS_E_NOADDR;
}

// One query to one server. Returns DNS_OK or a DNS_E_* code.
static inline int __dns_ask(const uint8_t server[4], int qlen, uint16_t id, uint8_t ip[4]) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return DNS_E_NETDOWN;
    sock_settimeout(s, DNS_TIMEOUT_MS);

    char sip[16];
    snprintf(sip, sizeof(sip), "%d.%d.%d.%d", server[0], server[1], server[2], server[3]);

    // sock_sendto() performs the whole exchange and buffers the reply.
    if (sock_sendto(s, __dns_q, (size_t)qlen, sip, DNS_PORT) < 0) {
        int e = errno;
        sock_close(s);
        return (e == ENETDOWN) ? DNS_E_NETDOWN : DNS_E_TIMEOUT;
    }
    ssize_t r = sock_recvfrom(s, __dns_r, sizeof(__dns_r), NULL);
    sock_close(s);
    if (r < 0) return DNS_E_TIMEOUT;
    return __libdns_parse(__dns_r, (int)r, id, ip);
}

// ── Public API ──────────────────────────────────────────────────────
// Resolve `host` to an IPv4 address. Dotted quads pass straight through.
static inline int dns_resolve(const char *host, uint8_t ip[4]) {
    __dns_err = DNS_OK;
    if (!host || !*host || strlen(host) > 253) {
        __dns_err = DNS_E_BADNAME; errno = EINVAL; return -1;
    }
    if (inet_aton(host, ip)) return 0;

#if DNS_CACHE
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (__dns_cache[i].used && __dns_ci_eq(__dns_cache[i].name, host)) {
            memcpy(ip, __dns_cache[i].ip, 4);
            return 0;
        }
    }
#endif

    __dns_init_servers();
    int qlen = __dns_build_query(host, 0, __dns_q, (int)sizeof(__dns_q));
    if (qlen < 0) { __dns_err = DNS_E_BADNAME; errno = EINVAL; return -1; }

    int last = DNS_E_TIMEOUT;
    for (int round = 0; round < DNS_ROUNDS; round++) {
        for (int i = 0; i < __dns_nservers; i++) {
            uint16_t id = ++__dns_next_id;
            __dns_q[0] = (uint8_t)(id >> 8);
            __dns_q[1] = (uint8_t)id;

            uint8_t got[4];
            int rc = __dns_ask(__dns_servers[i], qlen, id, got);
            if (rc == DNS_OK) {
                memcpy(ip, got, 4);
#if DNS_CACHE
                if (strlen(host) < sizeof(__dns_cache[0].name)) {
                    __dns_cache_t *c = &__dns_cache[__dns_cache_next];
                    __dns_cache_next = (__dns_cache_next + 1) % DNS_CACHE_SIZE;
                    c->used = 1;
                    memcpy(c->name, host, strlen(host) + 1);
                    memcpy(c->ip, got, 4);
                }
#endif
                return 0;
            }
            if (rc == DNS_E_NXDOMAIN || rc == DNS_E_NOADDR) {   // definitive
                __dns_err = rc; errno = ENOENT; return -1;
            }
            if (rc == DNS_E_NETDOWN) {
                __dns_err = rc; errno = ENETDOWN; return -1;
            }
            last = rc;                                          // try the next server
        }
    }
    __dns_err = last;
    errno = (last == DNS_E_TIMEOUT) ? ETIMEDOUT : EHOSTUNREACH;
    return -1;
}

// Same, but writes the address as a dotted-quad string ("1.2.3.4").
// `out` needs at least 16 bytes.
static inline int dns_resolve_str(const char *host, char *out, size_t cap) {
    uint8_t ip[4];
    if (dns_resolve(host, ip) != 0) return -1;
    snprintf(out, cap, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

// create_connection() that resolves with libdns first. Returns a socket fd
// or -1 (check dns_last_error() if errno is ENOENT/EHOSTUNREACH/ETIMEDOUT).
static inline int dns_create_connection(const char *host, uint16_t port, int32_t timeout_ms) {
    char ipstr[16];
    if (dns_resolve_str(host, ipstr, sizeof(ipstr)) != 0) return -1;
    return create_connection(ipstr, port, timeout_ms);
}