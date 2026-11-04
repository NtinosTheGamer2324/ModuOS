// mcurl.c -- A lightweight HTTP/1.0 + HTTPS (TLS 1.2) client for ModuOS
#include "sockets.h"
#include "tls.h"

static tls_t g_tls;      /* ~140 KB: keep it off the stack */

static void print_usage(void) {
    printf("mcurl - Lightweight HTTP/1.0 + HTTPS client for ModuOS\n");
    printf("Usage: mcurl <url>\n");
    printf("       mcurl <host> [port] [path]     (port 443 means HTTPS)\n");
    printf("Example: mcurl https://example.com/\n");
    printf("Example: mcurl 127.0.0.1 8080 /\n");
    printf("Example: mcurl example.com 443 /\n");
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

int md_main(long argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    sock_set_dns(8, 8, 8, 8);

    static char host_buf[256];
    static char path_buf[512];
    const char *host = argv[1];
    uint16_t port = 80;
    const char *path = "/";
    int use_tls = 0;

    if (starts_with(argv[1], "http://") || starts_with(argv[1], "https://")) {
        if (parse_url(argv[1], &use_tls, host_buf, sizeof(host_buf), &port,
                      path_buf, sizeof(path_buf)) != 0) {
            printf("mcurl: invalid URL\n");
            return 1;
        }
        host = host_buf;
        path = path_buf;
    } else {
        if (argc > 2 && argv[2] && argv[2][0] != '/') {
            port = (uint16_t)atoi(argv[2]);
            if (argc > 3) {
                path = argv[3];
            }
        } else if (argc > 2) {
            path = argv[2];
        }
        use_tls = (port == 443);
    }

    char req[1024];
    int default_port = use_tls ? 443 : 80;
    int req_len;
    if (port == default_port) {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: ModuOS-mcurl/1.0\r\n"
            "Connection: close\r\n\r\n",
            path, host);
    } else {
        req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: ModuOS-mcurl/1.0\r\n"
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

    char buf[512];
    ssize_t n;

    if (use_tls) {
        if (tls_connect_to(&g_tls, resolved_ip_str, host, port, 5000, 1) != 0) {
            printf("mcurl: TLS connection failed to %s:%u: %s\n", host, port, tls_error(&g_tls));
            return 1;
        }
        printf("mcurl: TLS 1.2 established (%s, %s)\n", tls_cipher_name(&g_tls), g_tls.curve);
        printf("mcurl: WARNING: the certificate chain is NOT validated; the server\n"
               "mcurl: identity is unauthenticated and a man-in-the-middle is possible\n");
        printf("mcurl: sending request for %s...\n\n", path);

        if (tls_sendall(&g_tls, req, (size_t)req_len) != 0) {
            printf("mcurl: failed to send request: %s\n", tls_error(&g_tls));
            tls_close(&g_tls);
            return 1;
        }
        while ((n = tls_recv(&g_tls, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
        }
        if (n < 0) {
            printf("\nmcurl: receive error or timeout: %s\n", tls_error(&g_tls));
        } else if (!tls_clean_close(&g_tls)) {
            printf("\nmcurl: warning: stream ended without close_notify; "
                   "the response may be truncated\n");
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
        while ((n = sock_recv(fd, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
        }
        if (n < 0) {
            printf("\nmcurl: receive error or timeout (errno=%d, %s)\n", errno, sock_last_message());
        }
        sock_close(fd);
    }
    printf("\nmcurl: disconnected.\n");
    return 0;
}