// stwp_server.c -- STWP server for ModuOS (port of the Python server).
//
// Needs: patched netd (TCP_LISTEN/ACCEPT) and the updated sockets.h.
// Usage: stwpserver [document_root]     (default: PUBLIC_ROOT)
//
// Protocol (every reply ends with 0x03):
//   INFO            -> "001; <STWP/1> <127.0.0.1:8080>"
//   GET <path>      -> "001 OK;<Content:...file bytes...>;"  | "002 Not Found;"
//   STAT <path>     -> "001 OK;<Content-Length:N>;<CheckSum:sha256>;" | "002 Not Found;"
//   missing path    -> "003"
//   anything else   -> "004 Invalid Request;"
#include "sockets.h"

#define PORT               8080
#define END_BYTE           0x03
#define PUBLIC_ROOT        "/public"
#define MAX_FILE           (4u * 1024u * 1024u)   // larger files are reported as Not Found
#define STWP_PATH_MAX      256
#define CLIENT_TIMEOUT_MS  5000
// What INFO reports. Kept identical to the Python server so Windows-side
// clients see the same text (the host reaches us via QEMU's port forward).
#define ADVERTISED_HOST    "127.0.0.1"

// ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t sha_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha_rotr(w[i-15], 7) ^ sha_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sha_rotr(w[i-2], 17) ^ sha_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, data + i);

    uint8_t tail[128];
    size_t rem = len - i;
    if (rem) memcpy(tail, data + i, rem);
    tail[rem++] = 0x80;
    size_t tlen = (rem <= 56) ? 64 : 128;
    memset(tail + rem, 0, tlen - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) tail[tlen - 1 - j] = (uint8_t)(bits >> (8 * j));
    sha256_block(h, tail);
    if (tlen == 128) sha256_block(h, tail + 64);

    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(h[j] >> 24);
        out[j*4+1] = (uint8_t)(h[j] >> 16);
        out[j*4+2] = (uint8_t)(h[j] >> 8);
        out[j*4+3] = (uint8_t)h[j];
    }
}

static void to_hex(const uint8_t *d, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = digits[d[i] >> 4];
        out[i*2+1] = digits[d[i] & 15];
    }
    out[n*2] = 0;
}

// ── Helpers ──────────────────────────────────────────────────────────

// Sends msg followed by the END byte in a single sendall.
static void send_msg(int c, const char *msg) {
    uint8_t buf[160];
    size_t n = strlen(msg);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, msg, n);
    buf[n++] = END_BYTE;
    sock_sendall(c, buf, n);
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Like Python's str.split(): returns the next whitespace-delimited token
// (NUL-terminated in place) or NULL.
static char *next_token(char **pp) {
    char *p = *pp;
    while (*p && is_ws(*p)) p++;
    if (!*p) { *pp = p; return NULL; }
    char *start = p;
    while (*p && !is_ws(*p)) p++;
    if (*p) { *p = 0; p++; }
    *pp = p;
    return start;
}

// root + request path ("/" means "/index.html"). Rejects anything that
// isn't absolute or that contains ".." (the Python version let
// "GET /../server.py" escape the public folder).
static int build_path(const char *root, const char *req, char *out, size_t cap) {
    if (strcmp(req, "/") == 0) req = "/index.html";
    if (req[0] != '/') return -1;
    for (const char *p = req; *p; p++) {
        if (p[0] == '.' && p[1] == '.') return -1;
        if (*p == '\\') return -1;
    }
    size_t rl = strlen(root), ql = strlen(req);
    if (rl + ql + 1 > cap) return -1;
    memcpy(out, root, rl);
    memcpy(out + rl, req, ql + 1);
    return 0;
}

// Reads a whole file into a malloc'd buffer (caller frees). NULL if it can't
// be opened, is too big, or memory runs out.
static uint8_t *load_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    size_t cap = 4096, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        if (len == cap) {
            if (cap >= MAX_FILE) { free(buf); close(fd); return NULL; }
            uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    *out_len = len;
    return buf;
}

// ── Request handling ─────────────────────────────────────────────────

static void handle_get(int c, const char *root, const char *arg) {
    char path[STWP_PATH_MAX];
    size_t flen = 0;
    uint8_t *data = NULL;
    if (build_path(root, arg, path, sizeof(path)) == 0) data = load_file(path, &flen);
    if (!data) { send_msg(c, "002 Not Found;"); return; }

    static const char HDR[] = "001 OK;<Content:";
    static const char TRL[] = ">;";
    size_t hl = sizeof(HDR) - 1, tl = sizeof(TRL) - 1;
    uint8_t *out = (uint8_t *)malloc(hl + flen + tl + 1);
    if (!out) { free(data); send_msg(c, "002 Not Found;"); return; }
    memcpy(out, HDR, hl);
    if (flen) memcpy(out + hl, data, flen);
    memcpy(out + hl + flen, TRL, tl);
    out[hl + flen + tl] = END_BYTE;

    sock_sendall(c, out, hl + flen + tl + 1);
    free(out);
    free(data);
}

static void handle_stat(int c, const char *root, const char *arg) {
    char path[STWP_PATH_MAX];
    size_t flen = 0;
    uint8_t *data = NULL;
    if (build_path(root, arg, path, sizeof(path)) == 0) data = load_file(path, &flen);
    if (!data) { send_msg(c, "002 Not Found;"); return; }

    uint8_t digest[32];
    char hex[65], msg[128];
    sha256(data, flen, digest);
    to_hex(digest, 32, hex);
    free(data);
    snprintf(msg, sizeof(msg), "001 OK;<Content-Length:%d>;<CheckSum:%s>;", (int)flen, hex);
    send_msg(c, msg);
}

static void handle_client(int c, const char *root) {
    char req[1025];
    ssize_t n = sock_recv(c, req, sizeof(req) - 1);
    if (n <= 0) return;                              // closed or timed out: say nothing
    req[n] = 0;

    char *p = req;
    char *cmd = next_token(&p);
    if (!cmd) return;                                // blank request
    char *arg = next_token(&p);

    printf("stwp: %s %s\n", cmd, arg ? arg : "");

    if (strcmp(cmd, "INFO") == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "001; <STWP/1> <%s:%d>", ADVERTISED_HOST, PORT);
        send_msg(c, msg);
    } else if (strcmp(cmd, "GET") == 0) {
        if (!arg) send_msg(c, "003");
        else handle_get(c, root, arg);
    } else if (strcmp(cmd, "STAT") == 0) {
        if (!arg) send_msg(c, "003");
        else handle_stat(c, root, arg);
    } else {
        send_msg(c, "004 Invalid Request;");
    }
}

int md_main(long argc, char **argv) {
    const char *root = (argc > 1 && argv[1]) ? argv[1] : PUBLIC_ROOT;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0 || sock_bind(ls, "0.0.0.0", PORT) != 0 || sock_listen(ls, 4) != 0) {
        printf("stwp: cannot listen on port %d (errno=%d, %s)\n", PORT, errno, sock_last_message());
        printf("stwp: is netd running, and is it the version with TCP_LISTEN?\n");
        return 1;
    }
    printf("STWP Server started on port %d (root %s)...\n", PORT, root);

    for (;;) {
        sock_addr_t peer;
        int c = sock_accept(ls, &peer);              // blocks (1 s slices inside)
        if (c < 0) {
            printf("stwp: accept failed (errno=%d, %s)\n", errno, sock_last_message());
            sleep(1);
            continue;
        }
        printf("stwp: connection from %s:%d\n", inet_ntoa(peer.ip), (int)peer.port);
        sock_settimeout(c, CLIENT_TIMEOUT_MS);       // don't let one silent client hang us
        handle_client(c, root);
        sock_close(c);                               // graceful: netd flushes, then FIN
    }
    return 0;
}