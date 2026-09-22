/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * crypto.c - BLAKE2s-256 (RFC 7693) and the ChaCha20 block function
 * (RFC 8439), written from the RFCs. Checked against their test vectors
 * by kernel/cryptotest.sh, on the host.
 *
 * Both are defined on little-endian words; this is a big-endian machine,
 * so every load and store goes through le32().
 */
#include "crypto.h"

static u32 rotr(u32 x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static u32 rotl(u32 x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static u32 ld32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void st32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ---- BLAKE2s ------------------------------------------------------ */

static const u32 b2s_iv[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

static const u8 b2s_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

#define B2S_G(a, b, c, d, x, y)          \
    do {                                 \
        v[a] = v[a] + v[b] + (x);        \
        v[d] = rotr(v[d] ^ v[a], 16);    \
        v[c] = v[c] + v[d];              \
        v[b] = rotr(v[b] ^ v[c], 12);    \
        v[a] = v[a] + v[b] + (y);        \
        v[d] = rotr(v[d] ^ v[a], 8);     \
        v[c] = v[c] + v[d];              \
        v[b] = rotr(v[b] ^ v[c], 7);     \
    } while (0)

static void b2s_compress(struct blake2s *s, const u8 *block, int last)
{
    u32 m[16], v[16];
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = ld32(block + 4 * i);
    }
    for (i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = b2s_iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last) {
        v[14] = ~v[14];
    }
    for (i = 0; i < 10; i++) {
        const u8 *sg = b2s_sigma[i];

        B2S_G(0, 4, 8, 12, m[sg[0]], m[sg[1]]);
        B2S_G(1, 5, 9, 13, m[sg[2]], m[sg[3]]);
        B2S_G(2, 6, 10, 14, m[sg[4]], m[sg[5]]);
        B2S_G(3, 7, 11, 15, m[sg[6]], m[sg[7]]);
        B2S_G(0, 5, 10, 15, m[sg[8]], m[sg[9]]);
        B2S_G(1, 6, 11, 12, m[sg[10]], m[sg[11]]);
        B2S_G(2, 7, 8, 13, m[sg[12]], m[sg[13]]);
        B2S_G(3, 4, 9, 14, m[sg[14]], m[sg[15]]);
    }
    for (i = 0; i < 8; i++) {
        s->h[i] ^= v[i] ^ v[i + 8];
    }
}

void blake2s_init(struct blake2s *s)
{
    int i;

    for (i = 0; i < 8; i++) {
        s->h[i] = b2s_iv[i];
    }
    s->h[0] ^= 0x01010000UL | 32;       /* depth 1, fanout 1, no key, 32 out */
    s->t[0] = s->t[1] = 0;
    s->buflen = 0;
}

void blake2s_update(struct blake2s *s, const void *in, u32 len)
{
    const u8 *p = in;

    while (len > 0) {
        u32 n;

        /* The final block is compressed by blake2s_final, so a full
         * buffer is only compressed once more input follows it. */
        if (s->buflen == 64) {
            s->t[0] += 64;
            if (s->t[0] < 64) {
                s->t[1]++;
            }
            b2s_compress(s, s->buf, 0);
            s->buflen = 0;
        }
        n = 64 - s->buflen;
        if (n > len) {
            n = len;
        }
        {
            u32 i;

            for (i = 0; i < n; i++) {
                s->buf[s->buflen + i] = p[i];
            }
        }
        s->buflen += n;
        p += n;
        len -= n;
    }
}

void blake2s_final(struct blake2s *s, u8 out[32])
{
    u32 i;

    s->t[0] += s->buflen;
    if (s->t[0] < s->buflen) {
        s->t[1]++;
    }
    for (i = s->buflen; i < 64; i++) {
        s->buf[i] = 0;
    }
    b2s_compress(s, s->buf, 1);
    for (i = 0; i < 8; i++) {
        st32(out + 4 * i, s->h[i]);
    }
}

/* ---- ChaCha20 ----------------------------------------------------- */

#define QR(a, b, c, d)                   \
    do {                                 \
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16); \
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12); \
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);  \
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);  \
    } while (0)

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
    u32 in[16], x[16];
    int i;

    in[0] = 0x61707865UL;               /* "expand 32-byte k" */
    in[1] = 0x3320646eUL;
    in[2] = 0x79622d32UL;
    in[3] = 0x6b206574UL;
    for (i = 0; i < 8; i++) {
        in[4 + i] = ld32(key + 4 * i);
    }
    in[12] = counter;
    for (i = 0; i < 3; i++) {
        in[13 + i] = ld32(nonce + 4 * i);
    }
    for (i = 0; i < 16; i++) {
        x[i] = in[i];
    }
    for (i = 0; i < 10; i++) {
        QR(0, 4, 8, 12);
        QR(1, 5, 9, 13);
        QR(2, 6, 10, 14);
        QR(3, 7, 11, 15);
        QR(0, 5, 10, 15);
        QR(1, 6, 11, 12);
        QR(2, 7, 8, 13);
        QR(3, 4, 9, 14);
    }
    for (i = 0; i < 16; i++) {
        st32(out + 4 * i, x[i] + in[i]);
    }
}
