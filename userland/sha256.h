#pragma once
#include <stdint.h>
#include <stddef.h>
#include "string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE 32

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx;

static inline uint32_t __sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t __sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline void __sha256_transform(sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]     << 24) |
               ((uint32_t)block[i*4 + 1] << 16) |
               ((uint32_t)block[i*4 + 2] << 8)  |
               ((uint32_t)block[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = __sha256_rotr(w[i-15], 7) ^ __sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = __sha256_rotr(w[i-2], 17) ^ __sha256_rotr(w[i-2], 19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = __sha256_rotr(e, 6) ^ __sha256_rotr(e, 11) ^ __sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + __sha256_k[i] + w[i];
        uint32_t S0 = __sha256_rotr(a, 2) ^ __sha256_rotr(a, 13) ^ __sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static inline void sha256_init(sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static inline void sha256_update(sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t*)data;
    size_t off = 0;

    while (off < len) {
        size_t space = 64 - ctx->datalen;
        size_t take = (len - off < space) ? (len - off) : space;

        memcpy(ctx->data + ctx->datalen, bytes + off, take);
        ctx->datalen += (uint32_t)take;
        off += take;

        if (ctx->datalen == 64) {
            __sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static inline void sha256_final(sha256_ctx *ctx, uint8_t hash_out[32]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        memset(ctx->data + i, 0, 56 - i);
    } else {
        ctx->data[i++] = 0x80;
        memset(ctx->data + i, 0, 64 - i);
        __sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (int j = 0; j < 8; j++) {
        ctx->data[63 - j] = (uint8_t)(ctx->bitlen >> (j * 8));
    }
    __sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            hash_out[i + j*4] = (uint8_t)(ctx->state[j] >> (24 - i*8));
        }
    }
}

static inline void sha256(const void *data, size_t len, uint8_t hash_out[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash_out);
}

static inline void sha256_to_hex(const uint8_t hash[32], char out_hex[65]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i*2]     = digits[(hash[i] >> 4) & 0xF];
        out_hex[i*2 + 1] = digits[hash[i] & 0xF];
    }
    out_hex[64] = 0;
}

#ifdef __cplusplus
}
#endif