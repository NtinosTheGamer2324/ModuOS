#include "libc.h"
#include "userman.h"
#include "string.h"

#define USER_DB_PATH "/ModuOS/System64/users.db"
#define MAX_LINE 256

static void append_str(char *dst, size_t dst_sz, const char *src) {
    if (!dst || !src || dst_sz == 0) return;
    size_t len = strlen(dst);
    if (len >= dst_sz - 1) return;
    size_t copy = strlen(src);
    if (copy > dst_sz - 1 - len) copy = dst_sz - 1 - len;
    memcpy(dst + len, src, copy);
    dst[len + copy] = 0;
}

static void to_hex_lower(const uint8_t *in, uint32_t n, char *out) {
    static const char *hex = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i*2]   = hex[(in[i] >> 4) & 0xF];
        out[i*2+1] = hex[in[i] & 0xF];
    }
    out[n*2] = 0;
}

typedef struct {
    char user[32];
    char pass[64];
    char salt[33]; /* 16 bytes as 32 hex chars + null */
    int  uid;
} user_entry_t;

/* ---------------- RDRAND for secure random salt ---------------- */
static int rdrand64(uint64_t *result) {
    unsigned char ok;
    __asm__ __volatile__(
        "rdrand %0; setc %1"
        : "=r" (*result), "=qm" (ok)
    );
    return ok;
}

static void generate_salt_hex(char *salt_hex, size_t salt_hex_sz) {
    if (!salt_hex || salt_hex_sz < 33) return;
    uint8_t salt_bytes[16];

    for (int i = 0; i < 2; i++) {
        uint64_t rand_val;
        int attempts = 0;
        while (!rdrand64(&rand_val) && attempts++ < 10);
        if (attempts >= 10)
            rand_val = (uint64_t)__builtin_ia32_rdtsc();
        memcpy(salt_bytes + i * 8, &rand_val, 8);
    }

    to_hex_lower(salt_bytes, 16, salt_hex);
    salt_hex[32] = 0;
}

/* ---------------- SHA-256 (small, self-contained) ---------------- */
typedef struct {
    uint32_t h[8];
    uint64_t len_bits;
    uint8_t  buf[64];
    uint32_t buf_len;
} sha256_t;

static uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static uint32_t ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t bsig0(uint32_t x) { return rotr32(x,2)  ^ rotr32(x,13) ^ rotr32(x,22); }
static uint32_t bsig1(uint32_t x) { return rotr32(x,6)  ^ rotr32(x,11) ^ rotr32(x,25); }
static uint32_t ssig0(uint32_t x) { return rotr32(x,7)  ^ rotr32(x,18) ^ (x >> 3); }
static uint32_t ssig1(uint32_t x) { return rotr32(x,17) ^ rotr32(x,19) ^ (x >> 10); }

static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

static void sha256_init(sha256_t *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len_bits = 0; s->buf_len = 0;
}

static void sha256_block(sha256_t *s, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i=0;i<16;i++) w[i]=rd32be(block+i*4);
    for (int i=16;i<64;i++) w[i]=ssig1(w[i-2])+w[i-7]+ssig0(w[i-15])+w[i-16];
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],
             e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i=0;i<64;i++) {
        uint32_t t1=h+bsig1(e)+ch(e,f,g)+k256[i]+w[i];
        uint32_t t2=bsig0(a)+maj(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;
    s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

static void sha256_update(sha256_t *s, const uint8_t *data, uint32_t len) {
    s->len_bits += (uint64_t)len * 8u;
    while (len) {
        uint32_t take = 64 - s->buf_len;
        if (take > len) take = len;
        memcpy(s->buf + s->buf_len, data, take);
        s->buf_len += take; data += take; len -= take;
        if (s->buf_len == 64) { sha256_block(s, s->buf); s->buf_len = 0; }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        while (s->buf_len < 64) s->buf[s->buf_len++] = 0;
        sha256_block(s, s->buf); s->buf_len = 0;
    }
    while (s->buf_len < 56) s->buf[s->buf_len++] = 0;
    uint64_t L = s->len_bits;
    for (int i=0;i<8;i++) s->buf[63-i]=(uint8_t)(L>>(i*8));
    sha256_block(s, s->buf);
    for (int i=0;i<8;i++) wr32be(out+i*4, s->h[i]);
}

/* ---------------- DB helpers (unchanged) ---------------- */

static int parse_line(const char *line, user_entry_t *out) {
    if (!line || !out) return -1;
    char buf[MAX_LINE];
    safe_strcpy(buf, sizeof(buf), line);
    char *p1 = strchr(buf, ':'); if (!p1) return -1; *p1++ = 0;
    char *p2 = strchr(p1,  ':'); if (!p2) return -1; *p2++ = 0;
    char *p3 = strchr(p2,  ':');
    if (p3) {
        *p3++ = 0;
        safe_strcpy(out->salt, sizeof(out->salt), p2);
        safe_strcpy(out->pass, sizeof(out->pass), p3);
    } else {
        // Old format: username:uid:hash (no salt)
        safe_strcpy(out->salt, sizeof(out->salt), "");
        safe_strcpy(out->pass, sizeof(out->pass), p2);
    }
    safe_strcpy(out->user, sizeof(out->user), buf);
    out->uid = atoi(p1);
    return 0;
}

static int user_lookup(const char *name, user_entry_t *out) {
    int fd = open(USER_DB_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;
    char line[MAX_LINE];
    int r = 0;
    while ((r = read(fd, line, sizeof(line)-1)) > 0) {
        line[r] = 0;
        char *start = line, *nl = NULL;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            user_entry_t e;
            if (parse_line(start, &e) == 0 && strcmp(e.user, name) == 0) {
                if (out) *out = e;
                close(fd); return 0;
            }
            start = nl + 1;
        }
    }
    close(fd); return -1;
}

static void hash_password_hex(const char *password, const char *salt_hex,
                               char *hex_out, size_t hex_sz) {
    if (!password || !hex_out || hex_sz < 65) return;
    sha256_t s; sha256_init(&s);
    if (salt_hex && *salt_hex) {
        sha256_update(&s, (const uint8_t*)salt_hex,  (uint32_t)strlen(salt_hex));
    }
    sha256_update(&s, (const uint8_t*)password,  (uint32_t)strlen(password));
    uint8_t digest[32]; sha256_final(&s, digest);
    to_hex_lower(digest, 32, hex_out); hex_out[64] = 0;
}

static int user_auth(const char *user, const char *pass, int *uid_out) {
    user_entry_t e;
    if (user_lookup(user, &e) != 0) return -1;
    char hex[65];
    hash_password_hex(pass, e.salt, hex, sizeof(hex));
    if (strcmp(e.pass, hex) != 0) return -2;
    if (uid_out) *uid_out = e.uid;
    return 0;
}

static int write_all(int fd, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int r = write(fd, buf + off, len - off);
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
}

static int load_users(char *out, size_t out_sz) {
    int fd = open(USER_DB_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;
    int total = 0, r = 0;
    while ((r = read(fd, out + total, (int)(out_sz - total - 1))) > 0) {
        total += r;
        if ((size_t)total >= out_sz - 1) break;
    }
    out[total] = 0; close(fd); return total;
}

static int save_users(const char *buf) {
    int fd = open(USER_DB_PATH, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) return -1;
    int rc = write_all(fd, buf, (int)strlen(buf));
    close(fd); return rc;
}

/* ================================================================
 * Per-node request/response ring buffers.
 *
 * The kernel calls our write callback when a client sends a request,
 * and calls our read callback when the client reads back a response.
 * We keep one small req/resp pair per node so concurrent callers get
 * their own answer.  (Single-threaded daemon; one in-flight per node.)
 * ================================================================ */

#define NODE_BUF_SZ 256

typedef struct {
    /* request: written by client via write callback */
    uint8_t  req[NODE_BUF_SZ];
    uint32_t req_len;

    /* response: filled by us, drained by client via read callback */
    uint8_t  resp[NODE_BUF_SZ];
    uint32_t resp_len;
    uint32_t resp_pos;

    /* set to 1 when a new request is waiting to be processed */
    int      pending;
} node_state_t;

static node_state_t ns_auth;
static node_state_t ns_add;
static node_state_t ns_rm;
static node_state_t ns_pw;

/* ---- generic callbacks ----------------------------------------- */

static ssize_t node_write_cb(void *ctx, const void *buf, size_t count) {
    node_state_t *ns = (node_state_t *)ctx;
    if (!ns) return -1;
    if (count > NODE_BUF_SZ - 1) count = NODE_BUF_SZ - 1;
    memcpy(ns->req, buf, count);
    ns->req[count] = 0;
    ns->req_len  = (uint32_t)count;
    ns->resp_len = 0;
    ns->resp_pos = 0;
    ns->pending  = 1;
    return (ssize_t)count;
}

static ssize_t node_read_cb(void *ctx, void *buf, size_t count) {
    node_state_t *ns = (node_state_t *)ctx;
    if (!ns) return 0;
    uint32_t avail = ns->resp_len - ns->resp_pos;
    if (avail == 0) return 0;
    if ((uint32_t)count > avail) count = avail;
    memcpy(buf, ns->resp + ns->resp_pos, count);
    ns->resp_pos += (uint32_t)count;
    return (ssize_t)count;
}

/* helper: store a string as the response for a node */
static void set_response(node_state_t *ns, const char *str) {
    size_t len = strlen(str);
    if (len >= NODE_BUF_SZ) len = NODE_BUF_SZ - 1;
    memcpy(ns->resp, str, len);
    ns->resp[len] = 0;
    ns->resp_len = (uint32_t)len;
    ns->resp_pos = 0;
}

/* ================================================================
 * Request processors — called from the main loop when pending == 1
 * ================================================================ */

static void process_auth(node_state_t *ns) {
    /* format: user:pass */
    char req[NODE_BUF_SZ];
    memcpy(req, ns->req, ns->req_len + 1);

    char *p = strchr(req, ':');
    if (!p) { set_response(ns, "-1"); return; }
    *p++ = 0;

    int uid = -1;
    int rc  = user_auth(req, p, &uid);
    char out[32];
    if (rc == 0) itoa(uid, out, 10);
    else         safe_strcpy(out, sizeof(out), "-1");
    set_response(ns, out);
}

static void process_adduser(node_state_t *ns) {
    /* format: user:password:uid */
    char req[NODE_BUF_SZ];
    memcpy(req, ns->req, ns->req_len + 1);

    char *p1 = strchr(req, ':');
    if (!p1) { set_response(ns, "-1"); return; }
    *p1++ = 0;
    char *p2 = strchr(p1, ':');
    if (!p2) { set_response(ns, "-1"); return; }
    *p2++ = 0;

    int uid = atoi(p2);
    if (uid < 0) { set_response(ns, "-1"); return; }

    if (user_lookup(req, NULL) == 0) { set_response(ns, "-2"); return; } /* exists */

    char salt_hex[33];
    generate_salt_hex(salt_hex, sizeof(salt_hex));

    char hex[65];
    hash_password_hex(p1, salt_hex, hex, sizeof(hex));

    char buf[4096];
    if (load_users(buf, sizeof(buf)) < 0) { set_response(ns, "-1"); return; }

    char line[128]; line[0] = 0;
    char uidbuf[16]; itoa(uid, uidbuf, 10);
    append_str(line, sizeof(line), "\n");
    append_str(line, sizeof(line), req);
    append_str(line, sizeof(line), ":"); append_str(line, sizeof(line), uidbuf);
    append_str(line, sizeof(line), ":"); append_str(line, sizeof(line), salt_hex);
    append_str(line, sizeof(line), ":"); append_str(line, sizeof(line), hex);
    append_str(buf, sizeof(buf), line);

    if (save_users(buf) != 0) { set_response(ns, "-1"); return; }
    set_response(ns, "0");
}

static void process_rmuser(node_state_t *ns) {
    /* format: username */
    char req[NODE_BUF_SZ];
    memcpy(req, ns->req, ns->req_len + 1);
    /* trim trailing newline if any */
    char *nl = strchr(req, '\n'); if (nl) *nl = 0;

    char buf[4096];
    if (load_users(buf, sizeof(buf)) < 0) { set_response(ns, "-1"); return; }

    char out[4096]; out[0] = 0;
    char *line = buf, *nlp = NULL;
    int removed = 0;

    while ((nlp = strchr(line, '\n')) != NULL) {
        *nlp = 0;
        if (line[0] && line[0] != '#') {
            user_entry_t e;
            if (parse_line(line, &e) == 0 && strcmp(e.user, req) == 0) {
                removed = 1;
            } else {
                if (out[0]) append_str(out, sizeof(out), "\n");
                append_str(out, sizeof(out), line);
            }
        } else {
            if (out[0]) append_str(out, sizeof(out), "\n");
            append_str(out, sizeof(out), line);
        }
        line = nlp + 1;
    }

    if (!removed)              { set_response(ns, "-2"); return; }
    if (save_users(out) != 0)  { set_response(ns, "-1"); return; }
    set_response(ns, "0");
}

static void process_passwd(node_state_t *ns) {
    /* format: user:newpass */
    char req[NODE_BUF_SZ];
    memcpy(req, ns->req, ns->req_len + 1);

    char *p1 = strchr(req, ':');
    if (!p1) { set_response(ns, "-1"); return; }
    *p1++ = 0;

    user_entry_t existing;
    if (user_lookup(req, &existing) != 0) { set_response(ns, "-3"); return; }

    char hex[65];
    hash_password_hex(p1, existing.salt, hex, sizeof(hex));

    char buf[4096];
    if (load_users(buf, sizeof(buf)) < 0) { set_response(ns, "-1"); return; }

    char out[4096]; out[0] = 0;
    char *line = buf, *nlp = NULL;
    int updated = 0;

    while ((nlp = strchr(line, '\n')) != NULL) {
        *nlp = 0;
        if (line[0] && line[0] != '#') {
            user_entry_t e;
            if (parse_line(line, &e) == 0 && strcmp(e.user, req) == 0) {
                char newline[128]; newline[0] = 0;
                char uidbuf[16]; itoa(e.uid, uidbuf, 10);
                append_str(newline, sizeof(newline), e.user);
                append_str(newline, sizeof(newline), ":");
                append_str(newline, sizeof(newline), uidbuf);
                append_str(newline, sizeof(newline), ":");
                append_str(newline, sizeof(newline), e.salt);
                append_str(newline, sizeof(newline), ":");
                append_str(newline, sizeof(newline), hex);
                if (out[0]) append_str(out, sizeof(out), "\n");
                append_str(out, sizeof(out), newline);
                updated = 1;
            } else {
                if (out[0]) append_str(out, sizeof(out), "\n");
                append_str(out, sizeof(out), line);
            }
        } else {
            if (out[0]) append_str(out, sizeof(out), "\n");
            append_str(out, sizeof(out), line);
        }
        line = nlp + 1;
    }

    if (!updated)              { set_response(ns, "-2"); return; }
    if (save_users(out) != 0)  { set_response(ns, "-1"); return; }
    set_response(ns, "0");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    puts_raw("userman: start\n");

    memset(&ns_auth, 0, sizeof(ns_auth));
    memset(&ns_add,  0, sizeof(ns_add));
    memset(&ns_rm,   0, sizeof(ns_rm));
    memset(&ns_pw,   0, sizeof(ns_pw));

    /* Register auth node — clients write "user:pass", read back uid string */
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = USERMAN_NODE_AUTH;
        node.owner_id = "userman";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.read  = node_read_cb;
        node.ops.write = node_write_cb;
        node.ctx       = &ns_auth;
        if (userfs_register(&node) < 0) {
            puts_raw("userman: failed to register auth node\n");
            goto fail;
        }
    }

    /* Register adduser node — clients write "user:pass:uid", read back "0"/"-N" */
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = USERMAN_NODE_ADD;
        node.owner_id = "userman";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.read  = node_read_cb;
        node.ops.write = node_write_cb;
        node.ctx       = &ns_add;
        if (userfs_register(&node) < 0) {
            puts_raw("userman: failed to register add node\n");
            goto fail;
        }
    }

    /* Register rmuser node — clients write "username", read back "0"/"-N" */
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = USERMAN_NODE_RM;
        node.owner_id = "userman";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.read  = node_read_cb;
        node.ops.write = node_write_cb;
        node.ctx       = &ns_rm;
        if (userfs_register(&node) < 0) {
            puts_raw("userman: failed to register rm node\n");
            goto fail;
        }
    }

    /* Register passwd node — clients write "user:newpass", read back "0"/"-N" */
    {
        userfs_user_node_t node;
        memset(&node, 0, sizeof(node));
        node.path     = USERMAN_NODE_PASSWD;
        node.owner_id = "userman";
        node.perms    = USERFS_PERM_READ_WRITE;
        node.ops.read  = node_read_cb;
        node.ops.write = node_write_cb;
        node.ctx       = &ns_pw;
        if (userfs_register(&node) < 0) {
            puts_raw("userman: failed to register passwd node\n");
            goto fail;
        }
    }

    puts_raw("userman: nodes registered, entering main loop\n");

    /*
     * Main loop.
     * The kernel invokes our callbacks directly when clients read/write.
     * All we need to do here is process any pending requests that were
     * queued by node_write_cb and fill the response buffer so that the
     * next node_read_cb call returns the right answer.
     *
     * We must NEVER exit — the kernel deletes all nodes on process exit.
     */
    for (;;) {
        if (ns_auth.pending) { process_auth(&ns_auth);    ns_auth.pending = 0; }
        if (ns_add.pending)  { process_adduser(&ns_add);  ns_add.pending  = 0; }
        if (ns_rm.pending)   { process_rmuser(&ns_rm);    ns_rm.pending   = 0; }
        if (ns_pw.pending)   { process_passwd(&ns_pw);    ns_pw.pending   = 0; }
        yield();
    }

    /* unreachable */
    return 0;

fail:
    puts_raw("userman: fatal error, sleeping forever to keep nodes alive\n");
    for (;;) sleep(1000);
    return -1;
}