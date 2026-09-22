/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * random.c - the random number generator.
 *
 * The design is Linux's since 5.17, cut down to what this machine has:
 *
 *   INPUT. Everything goes into one BLAKE2s-256 hash state, the pool,
 *   which never outputs directly. At boot: the real-time clock, the
 *   ethernet address, the tick count, where the kernel landed. After
 *   that: the timing of every interrupt -- which one it was, the tick,
 *   and how far the MFP's timer had counted into the tick when it came
 *   (12288 Hz, so ~80 us) -- and anything written to /dev/random.
 *
 *   OUTPUT. ChaCha20, keyed from the pool, with FAST KEY ERASURE: every
 *   request first draws a new key from the stream and throws the old one
 *   away, so what was output before cannot be recomputed from the state
 *   after. The key is re-derived from the pool when enough new entropy
 *   has come in.
 *
 * HOW MUCH IS CREDITED, which decides when getrandom() stops waiting.
 * Measured under QEMU: a timer tick's own counter reading is the same
 * value 85% of the time -- under a quarter of a bit of min-entropy -- so
 * a tick is credited an EIGHTH of a bit. Any other interrupt (disk, key,
 * serial, network) arrives at an unrelated point in the tick and is
 * credited ONE bit, though it spans seven. Nothing written to the device
 * is credited. The pool is READY at 128 bits: about ten seconds after
 * boot on an idle machine, much sooner with anything happening.
 *
 * There is no cycle counter on a 68040, and under QEMU the MFP's counter
 * changes on every read, so the usual busy-loop "jitter" source gives
 * nothing here: measured, and left out.
 */
#include "random.h"
#include "crypto.h"
#include "timer.h"
#include "dev.h"
#include "wait.h"
#include "task.h"
#include "signal.h"
#include "errno.h"
#include "string.h"

#define READY_BITS      128
#define RESEED_BITS     64      /* new entropy that earns a reseed    */

static struct blake2s pool;
static u8  key[32];             /* the ChaCha20 key output comes from */
static u32 credit_x8;           /* entropy credited, in eighths       */
static u32 since_reseed_x8;
static int ready;
static struct waitq ready_wait;

/* Interrupt samples waiting to be stirred in: 16 of 4 words each. */
static u32 fast[16][4];
static u32 nfast;

/* Take the key from the pool's current state, keeping the old key in
 * the input too: a reseed can only add unpredictability. */
static void reseed(void)
{
    struct blake2s copy;
    u8 fresh[32];
    u16 sr = irq_save();

    copy = pool;
    irq_restore(sr);
    blake2s_update(&copy, key, sizeof(key));
    blake2s_final(&copy, fresh);
    sr = irq_save();
    memcpy(key, fresh, sizeof(key));
    since_reseed_x8 = 0;
    irq_restore(sr);
    memset(fresh, 0, sizeof(fresh));
    memset(&copy, 0, sizeof(copy));
}

void random_init(void)
{
    struct rtcdev *r = dev_rtc();
    struct netdev *n = dev_first_net();
    time_t now = 0;
    u32 v;

    blake2s_init(&pool);
    /* Known to anybody at the machine, unknown to anybody across a
     * wire: worth mixing in, worth no credit. */
    if (r && r->get(r, &now) == 0) {
        blake2s_update(&pool, &now, sizeof(now));
    }
    v = timer_jiffies();
    blake2s_update(&pool, &v, sizeof(v));
    if (n) {
        blake2s_update(&pool, n->mac, 6);
    }
    v = (u32)(unsigned long)&pool;
    blake2s_update(&pool, &v, sizeof(v));
    reseed();
}

void random_interrupt(u32 where, u32 what, u32 bits_x8)
{
    fast[nfast][0] = where;
    fast[nfast][1] = what;
    fast[nfast][2] = timer_jiffies();
    fast[nfast][3] = credit_x8;
    if (++nfast == 16) {
        blake2s_update(&pool, fast, sizeof(fast));
        nfast = 0;
    }
    credit_x8 += bits_x8;
    since_reseed_x8 += bits_x8;
    if (!ready && credit_x8 >= READY_BITS * 8) {
        ready = 1;
        wake_all(&ready_wait);
    }
}

void random_write(const void *buf, u32 len)
{
    const u8 *p = buf;

    while (len) {
        u32 n = len < 256 ? len : 256;
        u16 sr = irq_save();

        blake2s_update(&pool, p, n);
        irq_restore(sr);
        p += n;
        len -= n;
    }
}

int random_ready(void)
{
    return ready;
}

/*
 * Fast key erasure: the first 32 bytes of the stream become the next
 * key before anything is output, and the old key is gone.
 */
void random_get(void *buf, u32 len)
{
    static const u8 nonce[12];
    u8 k[32], block[64];
    u8 *out = buf;
    u32 counter = 0;
    u16 sr;

    if (since_reseed_x8 >= RESEED_BITS * 8) {
        reseed();
    }
    sr = irq_save();
    memcpy(k, key, sizeof(k));
    chacha20_block(k, counter++, nonce, block);
    memcpy(key, block, 32);             /* the next key, now */
    irq_restore(sr);

    {
        u32 n = len < 32 ? len : 32;

        memcpy(out, block + 32, n);
        out += n;
        len -= n;
    }
    while (len) {
        u32 n = len < 64 ? len : 64;

        chacha20_block(k, counter++, nonce, block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    memset(k, 0, sizeof(k));
    memset(block, 0, sizeof(block));
}

int random_wait(int nonblock)
{
    while (!ready) {
        if (nonblock) {
            return -EAGAIN;
        }
        if (current && signal_pending(current)) {
            return -EINTR;
        }
        sleep_on_timeout(&ready_wait, 100);
    }
    return 0;
}

/* Words for the kernel's own uses -- TCP's sequence numbers -- taken 16
 * at a time so that each does not cost a ChaCha20 block. */
u32 random_u32(void)
{
    static u32 buf[16];
    static u32 left;
    u32 v;
    u16 sr = irq_save();

    if (!left) {
        irq_restore(sr);
        random_get(buf, sizeof(buf));
        sr = irq_save();
        left = 16;
    }
    v = buf[--left];
    buf[left] = 0;
    irq_restore(sr);
    return v;
}
