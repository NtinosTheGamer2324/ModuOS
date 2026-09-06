// http_client.c — resolve a domain via netd's DNS (UDP) path, then GET a
// path over TCP, printing the raw response (headers + body) to stdout.
//
// This is the reference test for netd's TCP support, same role
// dns_client.c played for UDP: a deterministic way to exercise the full
// resolve -> handshake -> data transfer -> close path against something
// that actually answers.
//
// Usage: http_client <domain> [path]
//   domain  hostname to resolve via 8.8.8.8 and send as the Host: header
//   path    request path (defaults to "/")
//
// Talks only to netd's SHM mailbox, never touches net0 directly.
//


#include "libc.h"
#include "netd_ipc.h"

#define DNS_SERVER_IP    ((uint8_t[4]){8, 8, 8, 8})
#define DNS_SERVER_PORT  53
#define DNS_TIMEOUT_MS   3000

#define HTTP_PORT             80
#define HTTP_OPEN_TIMEOUT_MS  5000
#define HTTP_SEND_TIMEOUT_MS  5000
#define HTTP_RECV_TIMEOUT_MS  5000
#define HTTP_CLOSE_TIMEOUT_MS 2000

// ── Minimal DNS message building/parsing (just enough for one A query) ──
// Same shape as dns_client.c -- duplicated here rather than shared so
// http_client stays a single self-contained file.

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount, ancount, nscount, arcount;
} dns_hdr_t;

#define DNS_FLAG_RD        0x0100u
#define DNS_FLAG_QR_REPLY  0x8000u
#define DNS_QTYPE_A        1u
#define DNS_QCLASS_IN      1u

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int dns_encode_name(const char *host, uint8_t *out, size_t out_cap) {
    size_t out_pos = 0;
    const char *label_start = host;

    for (;;) {
        const char *p = label_start;
        while (*p && *p != '.') p++;
        size_t label_len = (size_t)(p - label_start);

        if (label_len == 0 || label_len > 63) return -1;
        if (out_pos + 1 + label_len >= out_cap) return -1;

        out[out_pos++] = (uint8_t)label_len;
        memcpy(out + out_pos, label_start, label_len);
        out_pos += label_len;

        if (*p == 0) break;
        label_start = p + 1;
    }

    if (out_pos + 1 > out_cap) return -1;
    out[out_pos++] = 0;
    return (int)out_pos;
}

static int dns_build_query(const char *host, uint16_t txid, uint8_t *out, size_t out_cap) {
    if (out_cap < sizeof(dns_hdr_t)) return -1;

    dns_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    put_be16((uint8_t *)&hdr.id, txid);
    put_be16((uint8_t *)&hdr.flags, DNS_FLAG_RD);
    put_be16((uint8_t *)&hdr.qdcount, 1);
    memcpy(out, &hdr, sizeof(hdr));

    int name_len = dns_encode_name(host, out + sizeof(hdr), out_cap - sizeof(hdr));
    if (name_len < 0) return -1;

    size_t pos = sizeof(hdr) + (size_t)name_len;
    if (pos + 4 > out_cap) return -1;
    put_be16(out + pos, DNS_QTYPE_A);   pos += 2;
    put_be16(out + pos, DNS_QCLASS_IN); pos += 2;

    return (int)pos;
}

static size_t dns_skip_name(const uint8_t *buf, size_t buf_len, size_t pos) {
    while (pos < buf_len) {
        uint8_t len = buf[pos];
        if ((len & 0xC0) == 0xC0) return pos + 2;
        if (len == 0) return pos + 1;
        pos += 1 + len;
    }
    return pos;
}

// Returns 1 and fills out_ip with the first A record found, or 0 if none.
static int dns_first_a_record(const uint8_t *buf, size_t buf_len, uint8_t out_ip[4]) {
    if (buf_len < sizeof(dns_hdr_t)) return 0;
    const dns_hdr_t *hdr = (const dns_hdr_t *)buf;
    uint16_t qdcount = get_be16((const uint8_t *)&hdr->qdcount);
    uint16_t ancount = get_be16((const uint8_t *)&hdr->ancount);

    size_t pos = sizeof(dns_hdr_t);
    for (uint16_t i = 0; i < qdcount; i++) {
        pos = dns_skip_name(buf, buf_len, pos);
        pos += 4;
    }

    for (uint16_t i = 0; i < ancount && pos < buf_len; i++) {
        pos = dns_skip_name(buf, buf_len, pos);
        if (pos + 10 > buf_len) break;

        uint16_t type     = get_be16(buf + pos);
        uint16_t rdlength = get_be16(buf + pos + 8);
        pos += 10;

        if (pos + rdlength > buf_len) break;

        if (type == DNS_QTYPE_A && rdlength == 4) {
            memcpy(out_ip, buf + pos, 4);
            return 1;
        }
        pos += rdlength;
    }
    return 0;
}

// Resolves `host` to an IPv4 address via netd's UDP_SEND, using 8.8.8.8.
static int resolve_host(netd_shm_t *shm, const char *host, uint8_t out_ip[4]) {
    netd_request_t req;
    netd_response_t resp;

    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_UDP_SEND;
    memcpy(req.target_ip, DNS_SERVER_IP, 4);
    req.udp_dst_port = DNS_SERVER_PORT;
    req.udp_src_port = 0;
    req.timeout_ms = DNS_TIMEOUT_MS;

    uint16_t txid = (uint16_t)(time_ms() & 0xFFFF);
    int qlen = dns_build_query(host, txid, req.udp_payload, sizeof(req.udp_payload));
    if (qlen < 0) {
        printf("http_client: hostname '%s' too long/invalid\n", host);
        return -1;
    }
    req.udp_payload_len = (uint16_t)qlen;

    printf("http_client: resolving '%s' via 8.8.8.8...\n", host);

    memset(&resp, 0, sizeof(resp));
    int r = netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS);
    if (r != 0) {
        printf("http_client: netd mailbox timed out during DNS lookup\n");
        return -1;
    }
    if (!resp.success) {
        printf("http_client: DNS lookup failed (%s)\n", resp.message[0] ? resp.message : "no response");
        return -1;
    }
    if (resp.udp_reply_len < sizeof(dns_hdr_t)) {
        printf("http_client: DNS reply too short\n");
        return -1;
    }

    const dns_hdr_t *rhdr = (const dns_hdr_t *)resp.udp_reply;
    uint16_t rid    = get_be16((const uint8_t *)&rhdr->id);
    uint16_t rflags = get_be16((const uint8_t *)&rhdr->flags);
    uint16_t ancount = get_be16((const uint8_t *)&rhdr->ancount);
    uint16_t rcode  = rflags & 0x000F;

    if (rid != txid) {
        printf("http_client: DNS reply txid mismatch -- ignoring\n");
        return -1;
    }
    if (!(rflags & DNS_FLAG_QR_REPLY)) {
        printf("http_client: DNS reply missing QR flag\n");
        return -1;
    }
    if (rcode != 0) {
        printf("http_client: DNS server returned error, rcode=%u\n", rcode);
        return -1;
    }
    if (ancount == 0) {
        printf("http_client: no DNS answers for '%s'\n", host);
        return -1;
    }
    if (!dns_first_a_record(resp.udp_reply, resp.udp_reply_len, out_ip)) {
        printf("http_client: no A record among the answers\n");
        return -1;
    }

    printf("http_client: '%s' resolved to %d.%d.%d.%d\n",
           host, out_ip[0], out_ip[1], out_ip[2], out_ip[3]);
    return 0;
}

int md_main(long argc, char **argv) {
    if (argc < 2) {
        printf("usage: http_client <domain> [path]\n");
        return 1;
    }

    const char *host = argv[1];
    const char *path = (argc >= 3) ? argv[2] : "/";

    int shm_handle = shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0);
    if (shm_handle < 0) {
        printf("http_client: could not open netd's mailbox (is netd running?)\n");
        return 1;
    }

    netd_shm_t *shm = (netd_shm_t *)mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W,
                                          MAP_SHARED, shm_handle);
    if (shm == MAP_FAILED) {
        printf("http_client: mmap of netd mailbox failed\n");
        return 1;
    }

    uint8_t ip[4];
    if (resolve_host(shm, host, ip) != 0) {
        munmap(shm, sizeof(netd_shm_t));
        return 1;
    }

    netd_request_t req;
    netd_response_t resp;

    // ── Connect ──────────────────────────────────────────────────────
    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_TCP_OPEN;
    memcpy(req.target_ip, ip, 4);
    req.tcp_dst_port = HTTP_PORT;
    req.timeout_ms = HTTP_OPEN_TIMEOUT_MS;

    memset(&resp, 0, sizeof(resp));
    if (netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS) != 0) {
        printf("http_client: netd mailbox timed out during connect\n");
        munmap(shm, sizeof(netd_shm_t));
        return 1;
    }
    if (!resp.success) {
        printf("http_client: connect failed (%s)\n", resp.message[0] ? resp.message : "no response");
        munmap(shm, sizeof(netd_shm_t));
        return 1;
    }

    int handle = resp.tcp_handle;
    printf("http_client: connected to %d.%d.%d.%d:%d, handle=%d\n",
           ip[0], ip[1], ip[2], ip[3], HTTP_PORT, handle);

    // ── Send request ─────────────────────────────────────────────────
    char get[256];
    int glen = snprintf(get, sizeof(get),
                         "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                         path, host);
    if (glen < 0 || (size_t)glen >= sizeof(get)) {
        printf("http_client: request line too long\n");
        munmap(shm, sizeof(netd_shm_t));
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_TCP_SEND;
    req.tcp_handle = handle;
    req.timeout_ms = HTTP_SEND_TIMEOUT_MS;
    memcpy(req.tcp_payload, get, (size_t)glen);
    req.tcp_payload_len = (uint16_t)glen;

    memset(&resp, 0, sizeof(resp));
    if (netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS) != 0 || !resp.success) {
        printf("http_client: send failed (%s)\n",
               resp.message[0] ? resp.message : "mailbox timed out");
        munmap(shm, sizeof(netd_shm_t));
        return 1;
    }

    // ── Drain response until EOF ─────────────────────────────────────
    printf("http_client: request sent, reading response:\n");
    printf("----------------------------------------\n");

    size_t total = 0;
    for (;;) {
        memset(&req, 0, sizeof(req));
        req.cmd = NETD_CMD_TCP_RECV;
        req.tcp_handle = handle;
        req.tcp_recv_cap = NETD_TCP_MAX_PAYLOAD;
        req.timeout_ms = HTTP_RECV_TIMEOUT_MS;

        memset(&resp, 0, sizeof(resp));
        int r = netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS);
        if (r != 0) {
            printf("\n----------------------------------------\n");
            printf("http_client: netd mailbox timed out during recv\n");
            break;
        }
        if (!resp.success) {
            printf("\n----------------------------------------\n");
            printf("http_client: recv failed (%s)\n", resp.message[0] ? resp.message : "bad handle");
            break;
        }

        if (resp.tcp_payload_len > 0) {
            // resp.tcp_payload is arbitrary bytes, not a C string (no
            // guaranteed NUL, and may contain embedded NULs from binary
            // response bodies) -- print it a byte at a time with "%c"
            // rather than "%s" or putchar (the latter isn't linked into
            // this libc).
            for (uint16_t i = 0; i < resp.tcp_payload_len; i++) {
                printf("%c", (char)resp.tcp_payload[i]);
            }
            total += resp.tcp_payload_len;
        }

        if (resp.tcp_eof) {
            printf("\n----------------------------------------\n");
            printf("http_client: done, %zu bytes total\n", total);
            break;
        }
    }

    // ── Close ────────────────────────────────────────────────────────
    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_TCP_CLOSE;
    req.tcp_handle = handle;
    memset(&resp, 0, sizeof(resp));
    netd_ipc_call(shm, &req, &resp, HTTP_CLOSE_TIMEOUT_MS);

    munmap(shm, sizeof(netd_shm_t));
    return 0;
}