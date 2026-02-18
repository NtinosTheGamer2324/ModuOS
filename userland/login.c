#include "libc.h"
#include "userman.h"
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

int md_main(long argc, char **argv) {
    (void)argc; (void)argv;
    puts_raw("login: started\n");

    puts_raw("ModuOS login\n");

    char username[64];
    char password[64];

    /*
     * Flush any pending buffered keystrokes so login doesn't auto-consume previous shell input,
     * AND so the shell doesn't later replay the login keystrokes (shell reads event0).
     */
retry:
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

        int ok = 0;
    int target_uid = -1;

    // authenticate via userman devfs
    int authfd = open("$/user/users/auth", O_RDWR, 0);
    if (authfd < 0) {
        puts_raw("login: userman not available\n");
        goto retry;
    }

    char req[128];
    safe_strcpy(req, sizeof(req), username);
    safe_strcat(req, sizeof(req), ":");
    safe_strcat(req, sizeof(req), password);
    write(authfd, req, strlen(req));
    char resp[32];
    int rr = read(authfd, resp, sizeof(resp)-1);
    close(authfd);
    if (rr > 0) {
        resp[rr] = 0;
        target_uid = atoi(resp);
    }

    if (target_uid >= 0) {
        ok = 1;
    }

    if (!ok) {
        puts_raw("login failed\n");
        goto retry;
        return 2;
    }

    if (target_uid < 0) {
        puts_raw("login: kernel user not allowed\n");
        goto retry;
        return 2;
    }

    if (setuid(target_uid) != 0) {
        puts_raw("login: setuid failed (need to be mdman/root to switch)\n");
        return 3;
    }

    puts_raw("\nlogin ok\n");

    // After successful login, exec into zenith5 userland shell.
    char *argv_sh[] = { "zenith5", NULL };
    char *envp_sh[] = { "PATH=/Apps", "SHELL=ZENITH5", NULL };
    execve("zenith5", argv_sh, envp_sh);

    puts_raw("login: execve(zenith5) failed\n");
    return 0;
}