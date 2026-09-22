/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * crypto.h - the two primitives the random number generator is built
 * from: BLAKE2s-256 (RFC 7693) to mix what goes into the pool, and the
 * ChaCha20 block function (RFC 8439) to produce what comes out.
 *
 * Plain C with no kernel dependencies, so kernel/cryptotest.sh can build
 * it for the host and check it against the RFCs' test vectors.
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include "../types.h"

struct blake2s {
    u32 h[8];
    u32 t[2];                   /* bytes hashed so far                */
    u8  buf[64];
    u32 buflen;
};

void blake2s_init(struct blake2s *s);           /* unkeyed, 32-byte digest */
void blake2s_update(struct blake2s *s, const void *in, u32 len);
void blake2s_final(struct blake2s *s, u8 out[32]);

/* One 64-byte block of keystream: 32-byte key, block counter, 12-byte
 * nonce, as RFC 8439 lays them out. */
void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]);

#endif
