// mcurl.c -- A lightweight HTTP/1.1 + HTTPS (TLS 1.2) client for ModuOS
#include "sockets.h"
#include "tls.h"

static tls_t g_tls;      /* ~150 KB: keep it off the stack */

static void print_usage(void) {
    printf("mcurl - Lightweight HTTP/1.1 + HTTPS client for ModuOS\n");
    printf("Usage: mcurl [options] <url>\n");
    printf("       mcurl [options] <host> [port] [path]     (port 443 means HTTPS)\n");
    printf("Options:\n");
    printf("  -k, --insecure     do not verify the server certificate (not recommended)\n");
    printf("  --cacert <file>    CA bundle in PEM format (default: %s)\n", TLS_CA_PATH);
    printf("  -h, --help         show this help\n");
    printf("Example: mcurl https://example.com/\n");
    printf("Example: mcurl 127.0.0.1 8080 /\n");
    printf("Example: mcurl example.com 443 /\n");
}
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} rtc_time_t;

static int is_leap_year(uint32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int64_t unix_now(void) {
    rtc_time_t rtc;

    int fd = open("$/dev/rtc", O_RDONLY);
    if (fd < 0)
        return 0;

    ssize_t n = read(fd, &rtc, sizeof(rtc));
    close(fd);

    if (n != sizeof(rtc))
        return 0;

    /* Validate RTC data */
    if (rtc.second > 59 || rtc.minute > 59 ||
        rtc.hour > 23 || rtc.month < 1 || rtc.month > 12 ||
        rtc.day < 1 || rtc.year < 1970)
        return 0;

    static const uint8_t days_per_month[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    uint32_t max_day = days_per_month[rtc.month - 1];

    if (rtc.month == 2 && is_leap_year(rtc.year))
        max_day = 29;

    if (rtc.day > max_day)
        return 0;

    /* Calculate days since 1970-01-01 */
    int64_t days = 0;

    for (uint32_t y = 1970; y < rtc.year; y++)
        days += is_leap_year(y) ? 366 : 365;

    for (uint32_t m = 1; m < rtc.month; m++) {
        days += days_per_month[m - 1];

        if (m == 2 && is_leap_year(rtc.year))
            days++;
    }

    days += rtc.day - 1;

    return days * 86400LL
         + rtc.hour * 3600LL
         + rtc.minute * 60LL
         + rtc.second;
}

/*
 * Resolves `domain` to an IPv4 address using our own hand-built DNS query
 * sent to Google DNS (8.8.8.8) over the sockets.h UDP API.
 *
 * Returns the address with its bytes in network order in memory (byte 0 of
 * the int is the first octet on a little-endian CPU), or 0 on failure.
 *
 * Note on sockets.h UDP: sock_sendto() performs the whole exchange (it
 * blocks until the reply arrives or the timeout expires) and buffers the
 * reply; sock_recvfrom() then just returns that buffered reply.
 */
static int domain_to_host(const char *domain) {
    // 1. Check if domain is already an IPv4 address
    int is_ip = 1;
    const char *p = domain;
    int dots = 0;
    while (*p) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') { is_ip = 0; break; }
        p++;
    }
    if (is_ip && dots == 3) {
        unsigned int b[4];
        if (sscanf(domain, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4) {
            return (int)((b[3] << 24) | (b[2] << 16) | (b[1] << 8) | b[0]);
        }
    }

    // 2. Create UDP socket for DNS query
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;
    sock_settimeout(sock, 3000);

    // 3. Construct DNS Request Packet
    unsigned char packet[512];
    int pkt_len = 0;

    uint16_t tx_id = 0x1337;
    packet[0] = (tx_id >> 8) & 0xFF;
    packet[1] = tx_id & 0xFF;
    packet[2] = 0x01; // Flags: Recursion Desired (RD=1)
    packet[3] = 0x00;
    packet[4] = 0x00;
    packet[5] = 0x01; // QDCOUNT = 1
    packet[6] = 0x00;
    packet[7] = 0x00; // ANCOUNT = 0
    packet[8] = 0x00;
    packet[9] = 0x00; // NSCOUNT = 0
    packet[10] = 0x00;
    packet[11] = 0x00;
    pkt_len = 12;

    const char *src = domain;
    while (*src) {
        const char *dot = strchr(src, '.');
        int len = dot ? (int)(dot - src) : (int)strlen(src);
        if (len <= 0 || len > 63 || pkt_len + 1 + len >= (int)sizeof(packet)) {
            sock_close(sock);
            return 0;
        }
        packet[pkt_len++] = (unsigned char)len;
        memcpy(&packet[pkt_len], src, len);
        pkt_len += len;
        src += len;
        if (*src == '.') src++;
    }
    packet[pkt_len++] = 0x00;

    if (pkt_len + 4 > (int)sizeof(packet)) {
        sock_close(sock);
        return 0;
    }
    packet[pkt_len++] = 0x00;
    packet[pkt_len++] = 0x01; // QTYPE  = A
    packet[pkt_len++] = 0x00;
    packet[pkt_len++] = 0x01; // QCLASS = IN

    // 4. Send to Google DNS and receive the response (sockets.h API)
    unsigned char resp[512];
    sock_addr_t from;
    ssize_t r = -1;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (sock_sendto(sock, packet, (size_t)pkt_len, "8.8.8.8", 53) < 0) {
            sock_close(sock);
            return 0;
        }
        r = sock_recvfrom(sock, resp, sizeof(resp), &from);
        if (r >= 0) break;              // got a reply; otherwise retry once
    }
    sock_close(sock);

    if (r < 12) return 0;
    if (resp[0] != ((tx_id >> 8) & 0xFF) || resp[1] != (tx_id & 0xFF)) return 0;
    if ((resp[3] & 0x0F) != 0) return 0; // RCODE error check

    int ancount = (resp[6] << 8) | resp[7];
    if (ancount == 0) return 0;

    // Skip Question Section
    int idx = 12;
    while (idx < r && resp[idx] != 0) {
        idx += resp[idx] + 1;
    }
    idx++; // skip root 0
    idx += 4; // skip QTYPE and QCLASS

    // Parse Answer Section for A Record (Type 1)
    for (int i = 0; i < ancount && idx < r; i++) {
        if ((resp[idx] & 0xC0) == 0xC0) {
            idx += 2; // Compressed name pointer
        } else {
            while (idx < r && resp[idx] != 0) {
                idx += resp[idx] + 1;
            }
            idx++;
        }

        if (idx + 10 > r) return 0;

        uint16_t type = (resp[idx] << 8) | resp[idx+1];
        uint16_t rdlength = (resp[idx+8] << 8) | resp[idx+9];
        idx += 10;

        if (type == 1 && rdlength == 4 && idx + 4 <= r) {
            uint32_t ip;
            memcpy(&ip, &resp[idx], 4);   // avoid unaligned access
            return (int)ip;
        }
        idx += rdlength;
    }

    return 0;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

/* Parses "scheme://host[:port][/path]". Returns 0 on success. */
static int parse_url(const char *url, int *use_tls, char *host, size_t host_cap,
                     uint16_t *port, char *path, size_t path_cap) {
    const char *p;
    if (starts_with(url, "https://"))      { *use_tls = 1; *port = 443; p = url + 8; }
    else if (starts_with(url, "http://"))  { *use_tls = 0; *port = 80;  p = url + 7; }
    else return -1;

    size_t hl = 0;
    while (p[hl] && p[hl] != ':' && p[hl] != '/') hl++;
    if (hl == 0 || hl >= host_cap) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;

    if (*p == ':') {
        *port = (uint16_t)atoi(p + 1);
        while (*p && *p != '/') p++;
    }
    if (*p == 0) p = "/";
    size_t pl = strlen(p);
    if (pl >= path_cap) return -1;
    memcpy(path, p, pl + 1);
    return 0;
}


/* ------------------------------------------------------------------ HTTP/1.1 response handling
 * The raw status line + headers are echoed to stdout, followed by the body.
 * The body is decoded: Transfer-Encoding: chunked is undone, Content-Length is
 * honoured (so we stop as soon as the message is complete), and otherwise the
 * body runs until the server closes the connection. */
enum { H_HEADERS, H_BODY_LEN, H_BODY_CHUNKED, H_BODY_CLOSE, H_DONE };
enum { C_SIZE, C_EXT, C_DATA, C_DATA_END, C_TRAILER };

typedef struct {
    int       state;
    int       chunk_state;
    int       error;                 /* malformed chunked stream */
    int       status;
    char      hdr[8192];
    size_t    hdr_len;
    long long remaining;             /* bytes left in the Content-Length body or in the current chunk */
    int       size_digits;
    int       trailer_len;           /* non-CR characters on the current trailer line */
} http_t;

static http_t g_http;

static void out_write(const void *p, size_t n) {
    if (n) write(STDOUT_FILENO, p, n);
}

static int ci_prefix(const char *s, const char *prefix) {      /* case-insensitive prefix test */
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static int ci_contains(const char *s, size_t n, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= n; i++)
        if (ci_prefix(s + i, needle)) return 1;
    return 0;
}

static void http_init(http_t *h) {
    memset(h, 0, sizeof(*h));
    h->state = H_HEADERS;
}

/* Called once the blank line ending a header block has been seen. */
static void http_headers_done(http_t *h) {
    out_write(h->hdr, h->hdr_len);
    h->hdr[h->hdr_len] = 0;

    int status = 0;
    const char *p = h->hdr;
    while (*p && *p != ' ' && *p != '\n') p++;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') status = status * 10 + (*p++ - '0');
    h->status = status;

    int chunked = 0, have_len = 0, bad_len = 0;
    long long len = 0;
    const char *line = h->hdr;
    while (*line) {
        const char *eol = line;
        while (*eol && *eol != '\n') eol++;
        size_t ll = (size_t)(eol - line);
        if (ci_prefix(line, "transfer-encoding:")) {
            if (ci_contains(line, ll, "chunked")) chunked = 1;
        } else if (ci_prefix(line, "content-length:")) {
            const char *v = line + 15;
            while (*v == ' ' || *v == '\t') v++;
            long long val = 0;
            int digits = 0;
            while (*v >= '0' && *v <= '9' && digits < 18) { val = val * 10 + (*v++ - '0'); digits++; }
            if (digits == 0) bad_len = 1;
            else if (have_len && val != len) bad_len = 1;      /* conflicting duplicates */
            have_len = 1;
            len = val;
        }
        line = *eol ? eol + 1 : eol;
    }

    h->hdr_len = 0;
    if ((status >= 100 && status < 200 && status != 101) ) { h->state = H_HEADERS; return; }   /* interim response */
    if (status == 204 || status == 304 || status == 101)     { h->state = H_DONE; return; }
    if (chunked)                      { h->state = H_BODY_CHUNKED; h->chunk_state = C_SIZE; h->remaining = 0; h->size_digits = 0; return; }
    if (have_len && !bad_len)         { h->remaining = len; h->state = len == 0 ? H_DONE : H_BODY_LEN; return; }
    h->state = H_BODY_CLOSE;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Feed received bytes. Returns 1 when the message is complete (or broken) and
 * no more data should be read, 0 to keep reading. */
static int http_feed(http_t *h, const unsigned char *d, size_t n) {
    size_t i = 0;
    while (i < n && h->state != H_DONE && !h->error) {
        if (h->state == H_HEADERS) {
            if (h->hdr_len + 1 >= sizeof(h->hdr)) {                 /* absurdly long headers: stop parsing */
                out_write(h->hdr, h->hdr_len);
                h->hdr_len = 0;
                h->state = H_BODY_CLOSE;
                continue;
            }
            h->hdr[h->hdr_len++] = (char)d[i++];
            if (h->hdr_len >= 2 && h->hdr[h->hdr_len - 1] == '\n' &&
                (h->hdr[h->hdr_len - 2] == '\n' ||
                 (h->hdr_len >= 4 && h->hdr[h->hdr_len - 2] == '\r' && h->hdr[h->hdr_len - 3] == '\n')))
                http_headers_done(h);
        } else if (h->state == H_BODY_CLOSE) {
            out_write(d + i, n - i);
            i = n;
        } else if (h->state == H_BODY_LEN) {
            size_t take = n - i;
            if ((long long)take > h->remaining) take = (size_t)h->remaining;
            out_write(d + i, take);
            i += take;
            h->remaining -= (long long)take;
            if (h->remaining == 0) h->state = H_DONE;
        } else { /* H_BODY_CHUNKED */
            unsigned char c = d[i];
            switch (h->chunk_state) {
            case C_SIZE:
                if (c == '\r') { i++; break; }
                if (c == '\n') {
                    i++;
                    if (h->size_digits == 0) { h->error = 1; break; }
                    if (h->remaining == 0) { h->chunk_state = C_TRAILER; h->trailer_len = 0; }
                    else h->chunk_state = C_DATA;
                    break;
                }
                if (c == ';' || c == ' ' || c == '\t') { i++; h->chunk_state = C_EXT; break; }
                {
                    int v = hex_val(c);
                    if (v < 0 || h->remaining > (1LL << 40)) { h->error = 1; break; }
                    h->remaining = h->remaining * 16 + v;
                    h->size_digits++;
                    i++;
                }
                break;
            case C_EXT:                                             /* skip chunk extensions up to end of line */
                i++;
                if (c == '\n') {
                    if (h->size_digits == 0) { h->error = 1; break; }
                    if (h->remaining == 0) { h->chunk_state = C_TRAILER; h->trailer_len = 0; }
                    else h->chunk_state = C_DATA;
                }
                break;
            case C_DATA: {
                size_t take = n - i;
                if ((long long)take > h->remaining) take = (size_t)h->remaining;
                out_write(d + i, take);
                i += take;
                h->remaining -= (long long)take;
                if (h->remaining == 0) h->chunk_state = C_DATA_END;
                break;
            }
            case C_DATA_END:                                        /* CRLF after the chunk data */
                i++;
                if (c == '\n') { h->chunk_state = C_SIZE; h->remaining = 0; h->size_digits = 0; }
                else if (c != '\r') h->error = 1;
                break;
            case C_TRAILER:                                         /* trailers are dropped; blank line ends the message */
                i++;
                if (c == '\n') {
                    if (h->trailer_len == 0) h->state = H_DONE;
                    h->trailer_len = 0;
                } else if (c != '\r') h->trailer_len++;
                break;
            }
        }
    }
    return h->state == H_DONE || h->error;
}

/* Called when the connection ended (or reading stopped). `clean_tls` = peer sent close_notify. */
static void http_finish(const http_t *h, int is_tls, int clean_tls) {
    if (h->error) {
        printf("\nmcurl: warning: malformed chunked encoding in the response\n");
    } else if (h->state == H_HEADERS) {
        printf("\nmcurl: warning: connection closed before the response headers were complete\n");
    } else if (h->state == H_BODY_LEN) {
        printf("\nmcurl: warning: response truncated (%lu bytes of the body are missing)\n", (unsigned long)h->remaining);
    } else if (h->state == H_BODY_CHUNKED) {
        printf("\nmcurl: warning: response truncated (chunked body did not finish)\n");
    } else if (h->state == H_BODY_CLOSE && is_tls && !clean_tls) {
        printf("\nmcurl: warning: stream ended without close_notify; the response may be truncated\n");
    }
}

int md_main(long argc, char **argv) {
    int insecure = 0;
    const char *cacert = NULL;
    const char *pos[8];
    int npos = 0;

    for (long i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "-k") == 0 || strcmp(a, "--insecure") == 0) {
            insecure = 1;
        } else if (strcmp(a, "--cacert") == 0) {
            if (i + 1 >= argc) { printf("mcurl: --cacert needs a file name\n"); return 1; }
            cacert = argv[++i];
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage();
            return 0;
        } else if (a[0] == '-' && a[1] != 0) {
            printf("mcurl: unknown option '%s'\n", a);
            return 1;
        } else if (npos < 8) {
            pos[npos++] = a;
        }
    }
    if (npos < 1) {
        print_usage();
        return 1;
    }

    sock_set_dns(8, 8, 8, 8);

    static char host_buf[256];
    static char path_buf[512];
    const char *host = pos[0];
    uint16_t port = 80;
    const char *path = "/";
    int use_tls = 0;

    if (starts_with(pos[0], "http://") || starts_with(pos[0], "https://")) {
        if (parse_url(pos[0], &use_tls, host_buf, sizeof(host_buf), &port,
                      path_buf, sizeof(path_buf)) != 0) {
            printf("mcurl: invalid URL\n");
            return 1;
        }
        host = host_buf;
        path = path_buf;
    } else {
        if (npos > 1 && pos[1][0] != '/') {
            port = (uint16_t)atoi(pos[1]);
            if (npos > 2) {
                path = pos[2];
            }
        } else if (npos > 1) {
            path = pos[1];
        }
        use_tls = (port == 443);
    }

    char req[1024];
    int default_port = use_tls ? 443 : 80;
    int req_len;
    /* HTTP/1.1: Host is mandatory. "Connection: close" keeps the exchange to a
     * single request; identity encoding avoids having to inflate anything. */
    if (port == default_port) {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: ModuOS-mcurl/1.1\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            path, host);
    } else {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: ModuOS-mcurl/1.1\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            path, host, (unsigned)port);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        printf("mcurl: request string too long\n");
        return 1;
    }

    // Resolve domain name to IP using domain_to_host via Google DNS UDP query
    static char resolved_ip_str[32];
    int ip_val = domain_to_host(host);
    if (ip_val != 0) {
        unsigned char *b = (unsigned char *)&ip_val;
        snprintf(resolved_ip_str, sizeof(resolved_ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    } else {
        // Fall back to original host string if resolution fails
        strcpy(resolved_ip_str, host);
    }

    printf("mcurl: resolving %s via Google DNS (8.8.8.8) and connecting to port %u (%s)...\n",
            host, port, use_tls ? "TLS 1.2" : "plain HTTP");

    unsigned char buf[2048];
    ssize_t n;
    http_init(&g_http);

    if (use_tls) {
        const char *ca_path = cacert ? cacert : TLS_CA_PATH;
        tls_set_ca_path(ca_path);
        tls_set_time(unix_now());

        if (tls_connect_to(&g_tls, resolved_ip_str, host, port, 5000,
                           insecure ? TLS_VERIFY_NONE : TLS_VERIFY_FULL) != 0) {
            printf("mcurl: TLS connection failed to %s:%u: %s\n", host, port, tls_error(&g_tls));
            if (!insecure)
                printf("mcurl: (-k / --insecure skips the certificate checks; --cacert <file> selects another CA bundle)\n");
            return 1;
        }
        printf("mcurl: TLS 1.2 established (%s, %s)\n", tls_cipher_name(&g_tls), g_tls.curve);
        if (insecure) {
            printf("mcurl: WARNING: certificate verification is DISABLED (-k); the server\n"
                   "mcurl: identity is unauthenticated and a man-in-the-middle is possible\n");
        } else {
            printf("mcurl: server certificate verified for %s (chain of %d checked against %s)\n",
                   host, g_tls.chain_len, ca_path);
            if (!tls_dates_checked(&g_tls))
                printf("mcurl: NOTE: no system clock available, certificate validity dates were NOT checked\n");
        }
        printf("mcurl: sending request for %s...\n\n", path);

        if (tls_sendall(&g_tls, req, (size_t)req_len) != 0) {
            printf("mcurl: failed to send request: %s\n", tls_error(&g_tls));
            tls_close(&g_tls);
            return 1;
        }
        int done = 0;
        while (!done && (n = tls_recv(&g_tls, buf, sizeof(buf))) > 0) {
            done = http_feed(&g_http, buf, (size_t)n);
        }
        if (!done && n < 0) {
            printf("\nmcurl: receive error or timeout: %s\n", tls_error(&g_tls));
        } else {
            http_finish(&g_http, 1, tls_clean_close(&g_tls));
        }
        tls_close(&g_tls);
    } else {
        int fd = create_connection(resolved_ip_str, port, 5000);
        if (fd < 0) {
            printf("mcurl: connection failed to %s:%u (errno=%d, %s)\n",
                    host, port, errno, sock_last_message());
            return 1;
        }
        printf("mcurl: sending request for %s...\n\n", path);
        if (sock_sendall(fd, req, (size_t)req_len) != 0) {
            printf("mcurl: failed to send request (errno=%d, %s)\n", errno, sock_last_message());
            sock_close(fd);
            return 1;
        }
        int done = 0;
        while (!done && (n = sock_recv(fd, buf, sizeof(buf))) > 0) {
            done = http_feed(&g_http, buf, (size_t)n);
        }
        if (!done && n < 0) {
            printf("\nmcurl: receive error or timeout (errno=%d, %s)\n", errno, sock_last_message());
        } else {
            http_finish(&g_http, 0, 0);
        }
        sock_close(fd);
    }
    printf("\nmcurl: disconnected.\n");
    return 0;
}