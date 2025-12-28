#include "libc.h"
#include "string.h"

/*
 * login.sqr
 *
 * Multi-user login program.
 * File format: /ModuOS/System64/users.db
 * Lines:
 *   username:uid:sha256_hex
 * where sha256_hex is SHA-256(password) in lowercase hex.
 *
 * NOTE: password input is currently visible (no tty echo control yet).
 */

#define USERS_DB "/ModuOS/System64/users.db"

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
    /* pad */
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        while (s->buf_len < 64) s->buf[s->buf_len++] = 0;
        sha256_block(s, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 56) s->buf[s->buf_len++] = 0;

    /* length */
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

static int parse_users_db_line(const char *line, char *user, int user_sz, int *uid, char *hex, int hex_sz) {
    /* username:uid:hex */
    const char *p1 = strchr(line, ':');
    if (!p1) return -1;
    const char *p2 = strchr(p1+1, ':');
    if (!p2) return -1;

    int ulen = (int)(p1 - line);
    if (ulen <= 0 || ulen >= user_sz) return -1;
    memcpy(user, line, (size_t)ulen);
    user[ulen] = 0;

    char uidbuf[16];
    int uidlen = (int)(p2 - (p1+1));
    if (uidlen <= 0 || uidlen >= (int)sizeof(uidbuf)) return -1;
    memcpy(uidbuf, p1+1, (size_t)uidlen);
    uidbuf[uidlen] = 0;

    *uid = 0;
    for (int i=0; uidbuf[i]; i++) {
        if (uidbuf[i] < '0' || uidbuf[i] > '9') return -1;
        *uid = (*uid)*10 + (uidbuf[i]-'0');
    }

    /* strip newline */
    const char *hstart = p2+1;
    int hlen = 0;
    while (hstart[hlen] && hstart[hlen] != '\n' && hstart[hlen] != '\r') hlen++;
    if (hlen != 64) return -1;
    if (hlen+1 > hex_sz) return -1;
    memcpy(hex, hstart, (size_t)hlen);
    hex[hlen] = 0;

    return 0;
}

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;

    puts_raw("ModuOS login\n");

    char username[64];
    char password[64];

    /*
     * Flush any pending buffered keystrokes so login doesn't auto-consume previous shell input,
     * AND so the shell doesn't later replay the login keystrokes (shell reads event0).
     */
    input_flush();

    puts_raw("Username: ");
    {
        const char *in = input();
        strncpy(username, in ? in : "", sizeof(username) - 1);
        username[sizeof(username) - 1] = 0;
    }
    /* Zenith shell uses event0; drain any structured events generated while typing. */
    input_flush();

    puts_raw("\nPassword: ");
    {
        const char *in = input();
        strncpy(password, in ? in : "", sizeof(password) - 1);
        password[sizeof(password) - 1] = 0;
    }
    /* Drain again so password keystrokes aren't replayed as shell input. */
    input_flush();

    /* hash password */
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, (const uint8_t*)password, (uint32_t)strlen(password));
    uint8_t digest[32];
    sha256_final(&s, digest);
    char hex[65];
    to_hex_lower(digest, 32, hex);

    int fd = open(USERS_DB, O_RDONLY, 0);
    if (fd < 0) {
        puts_raw("login: cannot open users.db\n");
        return 1;
    }

    char line[256];
    int li = 0;
    int ok = 0;
    int target_uid = -1;

    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n != 1) break;
        if (li < (int)sizeof(line)-1) line[li++] = c;
        if (c == '\n') {
            line[li] = 0;
            li = 0;

            char u2[64];
            char h2[65];
            int uid;
            if (parse_users_db_line(line, u2, sizeof(u2), &uid, h2, sizeof(h2)) == 0) {
                if (strcmp(u2, username) == 0 && strcmp(h2, hex) == 0) {
                    ok = 1;
                    target_uid = uid;
                    break;
                }
            }
        }
    }
    close(fd);

    if (!ok) {
        puts_raw("login failed\n");
        return 2;
    }

    if (setuid(target_uid) != 0) {
        puts_raw("login: setuid failed (need to be mdman/root to switch)\n");
        return 3;
    }

    puts_raw("login ok\n");

    return 0;
}
