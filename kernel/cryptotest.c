/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cryptotest.c - crypto.c against the RFCs' test vectors, on the host.
 * Built and run by cryptotest.sh.
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"

static int fails;

static void hex(const u8 *p, int n, char *out)
{
    int i;

    for (i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", p[i]);
    }
}

static void check(const char *what, const u8 *got, int n, const char *want)
{
    char h[260];

    hex(got, n, h);
    printf("  %s %s\n", strcmp(h, want) == 0 ? "ok  " : "FAIL", what);
    if (strcmp(h, want) != 0) {
        printf("       got  %s\n       want %s\n", h, want);
        fails++;
    }
}

int main(void)
{
    struct blake2s s;
    u8 d[32], block[64], key[32], nonce[12];
    static u8 big[1000];
    int i;

    /* RFC 7693, appendix B: BLAKE2s-256("abc"). */
    blake2s_init(&s);
    blake2s_update(&s, "abc", 3);
    blake2s_final(&s, d);
    check("BLAKE2s-256(\"abc\"), RFC 7693 appendix B", d, 32,
          "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");

    /* The empty string: the published value everyone checks against. */
    blake2s_init(&s);
    blake2s_final(&s, d);
    check("BLAKE2s-256(\"\")", d, 32,
          "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9");

    /* Input fed in pieces must hash as fed whole: 1000 bytes, in
     * chunks that straddle the 64-byte block boundaries. */
    for (i = 0; i < 1000; i++) {
        big[i] = (u8)(i * 7);
    }
    {
        u8 whole[32], parts[32];

        blake2s_init(&s);
        blake2s_update(&s, big, 1000);
        blake2s_final(&s, whole);
        blake2s_init(&s);
        blake2s_update(&s, big, 1);
        blake2s_update(&s, big + 1, 63);
        blake2s_update(&s, big + 64, 64);
        blake2s_update(&s, big + 128, 500);
        blake2s_update(&s, big + 628, 372);
        blake2s_final(&s, parts);
        printf("  %s BLAKE2s: fed in pieces, the same as fed whole\n",
               memcmp(whole, parts, 32) == 0 ? "ok  " : "FAIL");
        if (memcmp(whole, parts, 32) != 0) {
            fails++;
        }
    }

    /* RFC 8439 2.3.2: the ChaCha20 block function test vector. */
    for (i = 0; i < 32; i++) {
        key[i] = (u8)i;
    }
    memcpy(nonce, "\x00\x00\x00\x09\x00\x00\x00\x4a\x00\x00\x00\x00", 12);
    chacha20_block(key, 1, nonce, block);
    check("ChaCha20 block, RFC 8439 section 2.3.2", block, 64,
          "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
          "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");

    printf("cryptotest: %d failed\n", fails);
    return fails != 0;
}
