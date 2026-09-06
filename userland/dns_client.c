// dns_client.c — sends a DNS "A" query for a hostname via netd's UDP_SEND
// command and prints back whatever IPv4 addresses come back in the reply.
//
// This exists mainly as a reference test for netd's UDP support: there's
// no general-purpose UDP echo service left on the public internet worth
// relying on, but a DNS resolver always answers a well-formed query, so
// it's a deterministic way to exercise the whole path -- UDP checksum
// (with the pseudo-header, the one new wrinkle vs ICMP), ephemeral source
// port allocation, and reply demuxing -- without standing up any extra
// infrastructure.
//
// Usage: dns_client [hostname]     (defaults to example.com)
//
// Talks only to netd's SHM mailbox, never touches net0 directly.
//


#include "libc.h"
#include "netd_ipc.h"

#define DNS_SERVER_IP   ((uint8_t[4]){8, 8, 8, 8})
#define DNS_SERVER_PORT 53
#define DNS_TIMEOUT_MS  3000

// ── Minimal DNS message building/parsing (just enough for one A query) ──

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount, ancount, nscount, arcount;
} dns_hdr_t; // all uint16_t fields, naturally 2-byte aligned; packed for clarity

#define DNS_FLAG_RD        0x0100u  // recursion desired
#define DNS_FLAG_QR_REPLY  0x8000u
#define DNS_QTYPE_A        1u
#define DNS_QCLASS_IN      1u

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Encodes "www.example.com" as DNS labels: 3 w w w 7 e x a m p l e 3 c o m 0.
// Returns bytes written, or -1 if it doesn't fit in out_cap.
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
    out[out_pos++] = 0; // root label
    return (int)out_pos;
}

// Builds a full DNS query packet for `host` (one question, type A, class
// IN, recursion desired). Returns the packet length, or -1 on error.
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
    put_be16(out + pos, DNS_QTYPE_A);  pos += 2;
    put_be16(out + pos, DNS_QCLASS_IN); pos += 2;

    return (int)pos;
}

// Skips one (possibly compressed) DNS name starting at buf[pos] and
// returns the position right after it. Doesn't follow compression
// pointers to resolve the name -- a pointer is always exactly 2 bytes
// wherever it appears, so that's all that's needed to skip past one.
static size_t dns_skip_name(const uint8_t *buf, size_t buf_len, size_t pos) {
    while (pos < buf_len) {
        uint8_t len = buf[pos];
        if ((len & 0xC0) == 0xC0) return pos + 2;      // compression pointer
        if (len == 0) return pos + 1;                   // root label, done
        pos += 1 + len;                                  // ordinary label
    }
    return pos;
}

static void print_ipv4(const uint8_t ip[4]) { printf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }

// Walks the answer section of a DNS reply, printing every A record found.
// Returns the number of A records printed.
static int dns_print_a_records(const uint8_t *buf, size_t buf_len) {
    if (buf_len < sizeof(dns_hdr_t)) return 0;
    const dns_hdr_t *hdr = (const dns_hdr_t *)buf;
    uint16_t qdcount = get_be16((const uint8_t *)&hdr->qdcount);
    uint16_t ancount = get_be16((const uint8_t *)&hdr->ancount);

    size_t pos = sizeof(dns_hdr_t);
    for (uint16_t i = 0; i < qdcount; i++) {
        pos = dns_skip_name(buf, buf_len, pos);
        pos += 4; // qtype + qclass
    }

    int printed = 0;
    for (uint16_t i = 0; i < ancount && pos < buf_len; i++) {
        pos = dns_skip_name(buf, buf_len, pos);
        if (pos + 10 > buf_len) break; // type(2)+class(2)+ttl(4)+rdlength(2)

        uint16_t type     = get_be16(buf + pos);
        uint32_t ttl       = get_be32(buf + pos + 4);
        uint16_t rdlength = get_be16(buf + pos + 8);
        pos += 10;

        if (pos + rdlength > buf_len) break;

        if (type == DNS_QTYPE_A && rdlength == 4) {
            printf("  A  ");
            print_ipv4(buf + pos);
            printf("  (ttl=%us)\n", ttl);
            printed++;
        }
        pos += rdlength;
    }
    return printed;
}

int md_main(long argc, char **argv) {
    const char *host = (argc >= 2) ? argv[1] : "example.com";

    int shm_handle = shm_open(NETD_IPC_SHM_NAME, O_RDWR, 0, 0);
    if (shm_handle < 0) {
        printf("dns_client: could not open netd's mailbox (is netd running?)\n");
        return 1;
    }

    netd_shm_t *shm = (netd_shm_t *)mmap(NULL, sizeof(netd_shm_t), PROT_R | PROT_W,
                                          MAP_SHARED, shm_handle);
    if (shm == MAP_FAILED) {
        printf("dns_client: mmap of netd mailbox failed\n");
        return 1;
    }

    netd_request_t req;
    memset(&req, 0, sizeof(req));
    req.cmd = NETD_CMD_UDP_SEND;
    memcpy(req.target_ip, DNS_SERVER_IP, 4);
    req.udp_dst_port = DNS_SERVER_PORT;
    req.udp_src_port = 0; // let netd pick an ephemeral port
    req.timeout_ms = DNS_TIMEOUT_MS;

    uint16_t txid = (uint16_t)(time_ms() & 0xFFFF);
    int qlen = dns_build_query(host, txid, req.udp_payload, sizeof(req.udp_payload));
    if (qlen < 0) {
        printf("dns_client: hostname '%s' too long/invalid\n", host);
        return 1;
    }
    req.udp_payload_len = (uint16_t)qlen;

    printf("dns_client: querying 8.8.8.8:53 for '%s' (txid=%u)...\n", host, txid);

    netd_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = netd_ipc_call(shm, &req, &resp, req.timeout_ms + NETD_IPC_RESPONSE_MARGIN_MS);

    int ok = 0;
    if (r != 0) {
        printf("dns_client: netd mailbox timed out (netd busy or not responding)\n");
    } else if (!resp.success) {
        printf("dns_client: failed (%s)\n", resp.message[0] ? resp.message : "no response");
    } else if (resp.udp_reply_len < sizeof(dns_hdr_t)) {
        printf("dns_client: reply too short to be a DNS message (%u bytes)\n", resp.udp_reply_len);
    } else {
        const dns_hdr_t *rhdr = (const dns_hdr_t *)resp.udp_reply;
        uint16_t rid    = get_be16((const uint8_t *)&rhdr->id);
        uint16_t rflags = get_be16((const uint8_t *)&rhdr->flags);
        uint16_t ancount = get_be16((const uint8_t *)&rhdr->ancount);
        uint16_t rcode  = rflags & 0x000F;

        if (rid != txid) {
            printf("dns_client: reply txid mismatch (got %u, expected %u) -- ignoring\n", rid, txid);
        } else if (!(rflags & DNS_FLAG_QR_REPLY)) {
            printf("dns_client: reply doesn't have the QR (response) flag set\n");
        } else if (rcode != 0) {
            printf("dns_client: server returned error, rcode=%u\n", rcode);
        } else if (ancount == 0) {
            printf("dns_client: no answers (NXDOMAIN or no A record for '%s')\n", host);
        } else {
            printf("dns_client: reply time=%ums, %u answer(s):\n", resp.rtt_ms, ancount);
            int printed = dns_print_a_records(resp.udp_reply, resp.udp_reply_len);
            if (printed == 0) printf("  (no A records among the answers -- maybe CNAME-only)\n");
            ok = 1;
        }
    }

    munmap(shm, sizeof(netd_shm_t));
    return ok ? 0 : 1;
}