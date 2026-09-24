/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 Jeff Francis */
/*
 * crypt.c - password hashing, SHA-512 ($6$).
 *
 * picolibc has no crypt(3), and without one there is no way to check a
 * password: login, su, sudo, passwd and dropbear's password
 * authentication all end here.
 *
 * WHY $6$ AND NOT TRADITIONAL crypt. DES crypt would be period-correct
 * for a 68k machine and is a bad idea on any machine: 8 significant
 * characters, a 12-bit salt, and fast enough to exhaust. $6$ is what
 * Linux writes today, so a hash from /etc/shadow here is the same
 * string a real Linux box would produce, and can be moved either way.
 *
 * The algorithm is Ulrich Drepper's SHA-crypt, which is a
 * specification rather than a folk recipe -- and it is intricate
 * enough that writing it from memory and believing the result would be
 * a mistake. libc/test/crypttest.c checks this against the published
 * test vectors AND against what the host's OpenSSL produces for the
 * same inputs, which is somebody else's implementation of the same
 * document. See the rule in CLAUDE.md: for anything whose job is a
 * wire format or an encoding, the port building is the start of the
 * test, not the end of it.
 *
 * The 64-bit arithmetic is plain C. The 68040 has no 64-bit registers,
 * so gcc synthesises every operation from 32-bit ones; that is slow and
 * correct, and slow is what a password hash is for.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

/* --- SHA-512 --------------------------------------------------------- */

struct sha512 {
    uint64_t state[8];
    uint64_t count;             /* bytes fed in so far */
    unsigned char buf[128];
};

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0(x) (ROTR(x, 28) ^ ROTR(x, 34) ^ ROTR(x, 39))
#define S1(x) (ROTR(x, 14) ^ ROTR(x, 18) ^ ROTR(x, 41))
#define s0(x) (ROTR(x, 1)  ^ ROTR(x, 8)  ^ ((x) >> 7))
#define s1(x) (ROTR(x, 19) ^ ROTR(x, 61) ^ ((x) >> 6))

static void sha512_block(struct sha512 *c, const unsigned char *p)
{
    uint64_t w[80], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint64_t)p[i * 8] << 56) | ((uint64_t)p[i * 8 + 1] << 48) |
               ((uint64_t)p[i * 8 + 2] << 40) | ((uint64_t)p[i * 8 + 3] << 32) |
               ((uint64_t)p[i * 8 + 4] << 24) | ((uint64_t)p[i * 8 + 5] << 16) |
               ((uint64_t)p[i * 8 + 6] << 8) | (uint64_t)p[i * 8 + 7];
    }
    for (i = 16; i < 80; i++) {
        w[i] = s1(w[i - 2]) + w[i - 7] + s0(w[i - 15]) + w[i - 16];
    }
    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];
    for (i = 0; i < 80; i++) {
        t1 = h + S1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = S0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha512_init(struct sha512 *c)
{
    c->state[0] = 0x6a09e667f3bcc908ULL; c->state[1] = 0xbb67ae8584caa73bULL;
    c->state[2] = 0x3c6ef372fe94f82bULL; c->state[3] = 0xa54ff53a5f1d36f1ULL;
    c->state[4] = 0x510e527fade682d1ULL; c->state[5] = 0x9b05688c2b3e6c1fULL;
    c->state[6] = 0x1f83d9abfb41bd6bULL; c->state[7] = 0x5be0cd19137e2179ULL;
    c->count = 0;
}

static void sha512_update(struct sha512 *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t have = (size_t)(c->count % 128), need;

    c->count += len;
    if (have) {
        need = 128 - have;
        if (len < need) {
            memcpy(c->buf + have, p, len);
            return;
        }
        memcpy(c->buf + have, p, need);
        sha512_block(c, c->buf);
        p += need;
        len -= need;
    }
    while (len >= 128) {
        sha512_block(c, p);
        p += 128;
        len -= 128;
    }
    if (len) {
        memcpy(c->buf, p, len);
    }
}

static void sha512_final(struct sha512 *c, unsigned char out[64])
{
    uint64_t bits = c->count * 8;
    size_t have = (size_t)(c->count % 128);
    unsigned char pad[256];
    size_t padlen;
    int i;

    /* 0x80, then zeroes, then the length in the last 16 bytes. The
     * length field is 128 bits; nothing here is long enough to need the
     * top half, which is written as zero rather than left out. */
    padlen = (have < 112) ? (112 - have) : (240 - have);
    memset(pad, 0, padlen + 16);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + 8 + i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha512_update(c, pad, padlen + 16);
    for (i = 0; i < 8; i++) {
        out[i * 8]     = (unsigned char)(c->state[i] >> 56);
        out[i * 8 + 1] = (unsigned char)(c->state[i] >> 48);
        out[i * 8 + 2] = (unsigned char)(c->state[i] >> 40);
        out[i * 8 + 3] = (unsigned char)(c->state[i] >> 32);
        out[i * 8 + 4] = (unsigned char)(c->state[i] >> 24);
        out[i * 8 + 5] = (unsigned char)(c->state[i] >> 16);
        out[i * 8 + 6] = (unsigned char)(c->state[i] >> 8);
        out[i * 8 + 7] = (unsigned char)(c->state[i]);
    }
}

/* --- SHA-512-crypt --------------------------------------------------- */

static const char b64chars[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN     1000
#define ROUNDS_MAX     999999999
#define SALT_MAX       16

static char *b64_from_24bit(char *p, unsigned int b2, unsigned int b1,
                            unsigned int b0, int n)
{
    unsigned int w = (b2 << 16) | (b1 << 8) | b0;
    int i;

    for (i = 0; i < n; i++) {
        *p++ = b64chars[w & 0x3f];
        w >>= 6;
    }
    return p;
}

/*
 * The algorithm, in the order the specification gives it. The names A,
 * B, DP and DS are its names, kept so that this can be read beside it.
 */
static char *sha512_crypt_r(const char *key, const char *setting,
                            char *out, size_t outlen)
{
    struct sha512 ctx, alt;
    unsigned char A[64], B[64], DP[64], DS[64];
    unsigned char *P, *S;
    char pbuf[64], sbuf[64];
    const char *salt;
    size_t keylen, saltlen, i;
    unsigned long rounds = ROUNDS_DEFAULT;
    int rounds_given = 0;
    char *p;

    if (strncmp(setting, "$6$", 3) != 0) {
        return 0;
    }
    salt = setting + 3;
    if (strncmp(salt, "rounds=", 7) == 0) {
        const char *num = salt + 7;
        char *end;
        unsigned long r = strtoul(num, &end, 10);

        if (*end == '$') {
            salt = end + 1;
            rounds_given = 1;
            rounds = r < ROUNDS_MIN ? ROUNDS_MIN
                   : (r > ROUNDS_MAX ? ROUNDS_MAX : r);
        }
    }
    for (saltlen = 0; saltlen < SALT_MAX && salt[saltlen] &&
                      salt[saltlen] != '$'; saltlen++) {
    }
    keylen = strlen(key);
    if (keylen > sizeof(pbuf) || saltlen > sizeof(sbuf)) {
        return 0;
    }

    /* B = SHA512(key . salt . key) */
    sha512_init(&alt);
    sha512_update(&alt, key, keylen);
    sha512_update(&alt, salt, saltlen);
    sha512_update(&alt, key, keylen);
    sha512_final(&alt, B);

    /* A = SHA512(key . salt . B-to-keylen . <bits of keylen>) */
    sha512_init(&ctx);
    sha512_update(&ctx, key, keylen);
    sha512_update(&ctx, salt, saltlen);
    for (i = keylen; i > 64; i -= 64) {
        sha512_update(&ctx, B, 64);
    }
    sha512_update(&ctx, B, i);
    for (i = keylen; i > 0; i >>= 1) {
        if (i & 1) {
            sha512_update(&ctx, B, 64);
        } else {
            sha512_update(&ctx, key, keylen);
        }
    }
    sha512_final(&ctx, A);

    /* DP = SHA512(key repeated keylen times), P = DP stretched to keylen */
    sha512_init(&ctx);
    for (i = 0; i < keylen; i++) {
        sha512_update(&ctx, key, keylen);
    }
    sha512_final(&ctx, DP);
    P = (unsigned char *)pbuf;
    for (i = keylen; i > 64; i -= 64) {
        memcpy(P + keylen - i, DP, 64);
    }
    memcpy(P + keylen - i, DP, i);

    /* DS = SHA512(salt repeated 16 + A[0] times), S stretched to saltlen */
    sha512_init(&ctx);
    for (i = 0; i < (size_t)(16 + A[0]); i++) {
        sha512_update(&ctx, salt, saltlen);
    }
    sha512_final(&ctx, DS);
    S = (unsigned char *)sbuf;
    for (i = saltlen; i > 64; i -= 64) {
        memcpy(S + saltlen - i, DS, 64);
    }
    memcpy(S + saltlen - i, DS, i);

    /* The rounds. This is the whole cost, and the point of it. */
    for (i = 0; i < rounds; i++) {
        sha512_init(&ctx);
        if (i & 1) {
            sha512_update(&ctx, P, keylen);
        } else {
            sha512_update(&ctx, A, 64);
        }
        if (i % 3) {
            sha512_update(&ctx, S, saltlen);
        }
        if (i % 7) {
            sha512_update(&ctx, P, keylen);
        }
        if (i & 1) {
            sha512_update(&ctx, A, 64);
        } else {
            sha512_update(&ctx, P, keylen);
        }
        sha512_final(&ctx, A);
    }

    /* "$6$" [ "rounds=N$" ] salt "$" 86 characters of base64. */
    p = out;
    if (outlen < 128) {
        return 0;
    }
    memcpy(p, "$6$", 3);
    p += 3;
    if (rounds_given) {
        int n = snprintf(p, 32, "rounds=%lu$", rounds);

        p += n;
    }
    memcpy(p, salt, saltlen);
    p += saltlen;
    *p++ = '$';

    /* The permutation is the specification's, and it is not a pattern
     * that can be reconstructed by guessing -- it is checked against
     * the published vectors in libc/test/crypttest.c. */
    p = b64_from_24bit(p, A[0],  A[21], A[42], 4);
    p = b64_from_24bit(p, A[22], A[43], A[1],  4);
    p = b64_from_24bit(p, A[44], A[2],  A[23], 4);
    p = b64_from_24bit(p, A[3],  A[24], A[45], 4);
    p = b64_from_24bit(p, A[25], A[46], A[4],  4);
    p = b64_from_24bit(p, A[47], A[5],  A[26], 4);
    p = b64_from_24bit(p, A[6],  A[27], A[48], 4);
    p = b64_from_24bit(p, A[28], A[49], A[7],  4);
    p = b64_from_24bit(p, A[50], A[8],  A[29], 4);
    p = b64_from_24bit(p, A[9],  A[30], A[51], 4);
    p = b64_from_24bit(p, A[31], A[52], A[10], 4);
    p = b64_from_24bit(p, A[53], A[11], A[32], 4);
    p = b64_from_24bit(p, A[12], A[33], A[54], 4);
    p = b64_from_24bit(p, A[34], A[55], A[13], 4);
    p = b64_from_24bit(p, A[56], A[14], A[35], 4);
    p = b64_from_24bit(p, A[15], A[36], A[57], 4);
    p = b64_from_24bit(p, A[37], A[58], A[16], 4);
    p = b64_from_24bit(p, A[59], A[17], A[38], 4);
    p = b64_from_24bit(p, A[18], A[39], A[60], 4);
    p = b64_from_24bit(p, A[40], A[61], A[19], 4);
    p = b64_from_24bit(p, A[62], A[20], A[41], 4);
    p = b64_from_24bit(p, 0,     0,     A[63], 2);
    *p = '\0';

    /* Not left lying about in memory. */
    memset(&ctx, 0, sizeof(ctx));
    memset(&alt, 0, sizeof(alt));
    memset(B, 0, sizeof(B));
    memset(DP, 0, sizeof(DP));
    memset(DS, 0, sizeof(DS));
    memset(pbuf, 0, sizeof(pbuf));
    memset(sbuf, 0, sizeof(sbuf));
    return out;
}

static char crypt_result[192];

char *crypt(const char *key, const char *salt);
char *crypt_r(const char *key, const char *salt, void *data);

char *crypt(const char *key, const char *salt)
{
    if (!key || !salt) {
        errno = EINVAL;
        return 0;
    }
    if (strncmp(salt, "$6$", 3) == 0) {
        char *r = sha512_crypt_r(key, salt, crypt_result,
                                 sizeof(crypt_result));

        if (!r) {
            errno = EINVAL;
        }
        return r;
    }
    /*
     * Any other scheme is REFUSED, not approximated. Returning
     * something that is not the hash the caller asked for would make
     * every password comparison fail in a way that looks like a wrong
     * password; glibc answers a NULL and errno for the same reason.
     * If a $1$ or a DES hash ever has to be read here, implement it --
     * do not let this fall through.
     */
    errno = EINVAL;
    return 0;
}

char *crypt_r(const char *key, const char *salt, void *data)
{
    /* The caller's buffer is glibc's `struct crypt_data`, whose first
     * member is a char array of at least 184 bytes. Writing the result
     * there is what makes this re-entrant. */
    if (!key || !salt || !data) {
        errno = EINVAL;
        return 0;
    }
    if (strncmp(salt, "$6$", 3) != 0) {
        errno = EINVAL;
        return 0;
    }
    return sha512_crypt_r(key, salt, (char *)data, 184);
}
