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


typedef struct {
    char user[32];
    char pass[64];
    int uid;
} user_entry_t;

/* ---------------- SHA-256 (small, self-contained) ---------------- */
typedef struct {
    uint32_t h[8];
    uint64_t len_bits;
    uint8_t  buf[64];
    uint32_t buf_len;
} sha256_t;

static uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t bsig0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static uint32_t bsig1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static uint32_t ssig0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
static uint32_t ssig1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }

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
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void sha256_init(sha256_t *s) {
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len_bits = 0;
    s->buf_len = 0;
}

static void sha256_block(sha256_t *s, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i=0;i<16;i++) w[i] = rd32be(block + i*4);
    for (int i=16;i<64;i++) w[i] = ssig1(w[i-2]) + w[i-7] + ssig0(w[i-15]) + w[i-16];

    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i=0;i<64;i++) {
        uint32_t t1 = h + bsig1(e) + ch(e,f,g) + k256[i] + w[i];
        uint32_t t2 = bsig0(a) + maj(a,b,c);
        h=g; g=f; f=e; e=d + t1;
        d=c; c=b; b=a; a=t1 + t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha256_update(sha256_t *s, const uint8_t *data, uint32_t len) {
    s->len_bits += (uint64_t)len * 8u;
    while (len) {
        uint32_t take = 64 - s->buf_len;
        if (take > len) take = len;
        memcpy(s->buf + s->buf_len, data, take);
        s->buf_len += take;
        data += take;
        len -= take;
        if (s->buf_len == 64) {
            sha256_block(s, s->buf);
            s->buf_len = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        while (s->buf_len < 64) s->buf[s->buf_len++] = 0;
        sha256_block(s, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 56) s->buf[s->buf_len++] = 0;

    uint64_t L = s->len_bits;
    for (int i=0;i<8;i++) s->buf[63 - i] = (uint8_t)(L >> (i*8));
    sha256_block(s, s->buf);

    for (int i=0;i<8;i++) wr32be(out + i*4, s->h[i]);
}

static void to_hex_lower(const uint8_t *in, uint32_t n, char *out) {
    static const char *hex = "0123456789abcdef";
    for (uint32_t i=0;i<n;i++) {
        out[i*2] = hex[(in[i] >> 4) & 0xF];
        out[i*2+1] = hex[in[i] & 0xF];
    }
    out[n*2] = 0;
}

static int parse_line(const char *line, user_entry_t *out) {
    if (!line || !out) return -1;
    char buf[MAX_LINE];
    safe_strcpy(buf, sizeof(buf), line);
    // format: username:uid:sha256_hex(password)
    char *p1 = strchr(buf, ':');
    if (!p1) return -1;
    *p1++ = 0;
    char *p2 = strchr(p1, ':');
    if (!p2) return -1;
    *p2++ = 0;
    safe_strcpy(out->user, sizeof(out->user), buf);
    out->uid = atoi(p1);
    safe_strcpy(out->pass, sizeof(out->pass), p2);
    return 0;
}

static int user_lookup(const char *name, user_entry_t *out) {
    int fd = open(USER_DB_PATH, O_RDONLY, 0);
    if (fd < 0) return -1;
    char line[MAX_LINE];
    int r = 0;
    while ((r = read(fd, line, sizeof(line)-1)) > 0) {
        line[r] = 0;
        char *start = line;
        char *nl = NULL;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = 0;
            user_entry_t e;
            if (parse_line(start, &e) == 0 && strcmp(e.user, name) == 0) {
                if (out) *out = e;
                close(fd);
                return 0;
            }
            start = nl + 1;
        }
    }
    close(fd);
    return -1;
}

static void hash_password_hex(const char *password, char *hex_out, size_t hex_sz) {
    if (!password || !hex_out || hex_sz < 65) return;
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, (const uint8_t*)password, (uint32_t)strlen(password));
    uint8_t digest[32];
    sha256_final(&s, digest);
    to_hex_lower(digest, 32, hex_out);
    hex_out[64] = 0;
}

static int user_auth(const char *user, const char *pass, int *uid_out) {
    user_entry_t e;
    if (user_lookup(user, &e) != 0) return -1;
    char hex[65];
    hash_password_hex(pass, hex, sizeof(hex));
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
    int total = 0;
    int r = 0;
    while ((r = read(fd, out + total, (int)(out_sz - total - 1))) > 0) {
        total += r;
        if ((size_t)total >= out_sz - 1) break;
    }
    out[total] = 0;
    close(fd);
    return total;
}

static int save_users(const char *buf) {
    int fd = open(USER_DB_PATH, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) return -1;
    int len = (int)strlen(buf);
    int rc = write_all(fd, buf, len);
    close(fd);
    return rc;
}

static int handle_adduser(int fd) {
    char req[128];
    int r = read(fd, req, sizeof(req)-1);
    if (r <= 0) return -1;
    req[r] = 0;
    // format: user:password:uid
    char *p1 = strchr(req, ':');
    if (!p1) return -1;
    *p1++ = 0;
    char *p2 = strchr(p1, ':');
    if (!p2) return -1;
    *p2++ = 0;
    int uid = atoi(p2);
    if (uid < 0) return -1;

    if (user_lookup(req, NULL) == 0) return -2; // exists

    char hex[65];
    hash_password_hex(p1, hex, sizeof(hex));

    char buf[4096];
    int n = load_users(buf, sizeof(buf));
    if (n < 0) return -1;
    char line[128];
    line[0] = 0;
    append_str(line, sizeof(line), "\n");
    append_str(line, sizeof(line), req);
    append_str(line, sizeof(line), ":");
    char uidbuf[16];
    itoa(uid, uidbuf, 10);
    append_str(line, sizeof(line), uidbuf);
    append_str(line, sizeof(line), ":");
    append_str(line, sizeof(line), hex);
    append_str(buf, sizeof(buf), line);
    if (save_users(buf) != 0) return -1;
    write(fd, "0", 1);
    return 0;
}

static int handle_rmuser(int fd) {
    char req[64];
    int r = read(fd, req, sizeof(req)-1);
    if (r <= 0) return -1;
    req[r] = 0;

    char buf[4096];
    int n = load_users(buf, sizeof(buf));
    if (n < 0) return -1;

    char out[4096];
    out[0] = 0;
    char *line = buf;
    char *nl = NULL;
    int removed = 0;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = 0;
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
        line = nl + 1;
    }
    if (!removed) return -2;
    if (save_users(out) != 0) return -1;
    write(fd, "0", 1);
    return 0;
}

static int handle_passwd(int fd) {
    char req[128];
    int r = read(fd, req, sizeof(req)-1);
    if (r <= 0) return -1;
    req[r] = 0;
    // format: user:newpass
    char *p1 = strchr(req, ':');
    if (!p1) return -1;
    *p1++ = 0;

    char hex[65];
    hash_password_hex(p1, hex, sizeof(hex));

    char buf[4096];
    int n = load_users(buf, sizeof(buf));
    if (n < 0) return -1;

    char out[4096];
    out[0] = 0;
    char *line = buf;
    char *nl = NULL;
    int updated = 0;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = 0;
        if (line[0] && line[0] != '#') {
            user_entry_t e;
            if (parse_line(line, &e) == 0 && strcmp(e.user, req) == 0) {
                char newline[128];
                newline[0] = 0;
                append_str(newline, sizeof(newline), e.user);
                append_str(newline, sizeof(newline), ":");
                char uidbuf2[16];
                itoa(e.uid, uidbuf2, 10);
                append_str(newline, sizeof(newline), uidbuf2);
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
        line = nl + 1;
    }
    if (!updated) return -2;
    if (save_users(out) != 0) return -1;
    write(fd, "0", 1);
    return 0;
}

static int register_node(const char *path) {
    userfs_user_node_t node;
    memset(&node, 0, sizeof(node));
    // userfs expects full $/userland/... path for userland nodes
    node.path = path;
    node.owner_id = "userman";
    return userfs_register(&node);
}

static void handle_auth(int fd) {
    char req[128];
    int r = read(fd, req, sizeof(req)-1);
    if (r <= 0) return;
    req[r] = 0;
    // format: user:pass
    char *p = strchr(req, ':');
    if (!p) return;
    *p++ = 0;
    int uid = -1;
    int rc = user_auth(req, p, &uid);
    char out[32];
    if (rc == 0) {
        itoa(uid, out, 10);
    } else {
        safe_strcpy(out, sizeof(out), "-1");
    }
    write(fd, out, strlen(out));
}

int md_main(long argc, char** argv) {
    (void)argc; (void)argv;
    puts_raw("userman: start\n");

    int rc_auth = register_node(USERMAN_NODE_AUTH);
    int rc_add = register_node(USERMAN_NODE_ADD);
    int rc_rm = register_node(USERMAN_NODE_RM);
    int rc_pw = register_node(USERMAN_NODE_PASSWD);

    puts_raw("userman: userfs nodes created\n");

    if (rc_auth != 0 || rc_add != 0 || rc_rm != 0 || rc_pw != 0) {
        puts_raw("userman: register failed\n");
        for (;;) sleep(1000);
    }

    sleep(50);

    int fd_auth = open(USERMAN_DEV_AUTH, O_RDWR | O_NONBLOCK, 0);
    int fd_add = open(USERMAN_DEV_ADD, O_RDWR | O_NONBLOCK, 0);
    int fd_rm = open(USERMAN_DEV_RM, O_RDWR | O_NONBLOCK, 0);
    int fd_pw = open(USERMAN_DEV_PASSWD, O_RDWR | O_NONBLOCK, 0);

    if (fd_auth < 0 || fd_add < 0 || fd_rm < 0 || fd_pw < 0) {
        puts_raw("userman: open failed\n");
        for (;;) sleep(1000);
    }

    for (;;) {
        handle_auth(fd_auth);
        handle_adduser(fd_add);
        handle_rmuser(fd_rm);
        handle_passwd(fd_pw);
        sleep(10);
        yield();
    }

    return 0;
}
