/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * localoptions.h - what Dropbear may assume about this machine.
 *
 * Dropbear reads this instead of being configured by a hundred
 * --enable flags. Every line here is a fact about SuckOS, not a
 * preference.
 */

/*
 * NO SERVER PASSWORD AUTHENTICATION. Checking a password means
 * crypt(3) against a hash in /etc/shadow, and picolibc has no crypt
 * and this system has no /etc/shadow. Public keys are what is left,
 * and are what should be used anyway.
 *
 * The CLIENT keeps password auth: sending a password to somebody
 * else's server needs no crypt here, and refusing to type one at a
 * machine that asks would make this client useless against half the
 * hosts in the world.
 */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_CLI_PASSWORD_AUTH 1
#define DROPBEAR_CLI_PUBKEY_AUTH 1

/* No pluggable authentication modules, and nothing to plug in. */
#define DROPBEAR_SVR_PAM_AUTH 0

/*
 * Ed25519 only, of the key types, plus the key exchange that goes with
 * it. It is the fastest to verify on a 25 MHz CPU with no multiplier
 * worth the name, the keys are 32 bytes, and it needs no parameter
 * generation. RSA is left in for talking to older servers -- a client
 * that cannot reach an existing host is not much of a client.
 *
 * DSS is off: it is withdrawn, and nothing should accept it.
 */
#define DROPBEAR_ED25519 1
#define DROPBEAR_RSA 1
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0

/* Diffie-Hellman group14 and curve25519. Group exchange wants big
 * primes generated on the fly, which this CPU would take minutes over. */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_DH_GROUP14_SHA256 1
#define DROPBEAR_DH_GROUP16 0

/*
 * ChaCha20-Poly1305 first: no lookup tables, so it is both faster and
 * safer than AES on a machine with no AES instructions. AES-CTR stays
 * for interoperability.
 */
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_ENABLE_CBC_MODE 0
#define DROPBEAR_3DES 0
#define DROPBEAR_TWOFISH 0

/* No compression: zlib is here, but ssh compression is a CPU cost on
 * a machine whose CPU is the scarce thing and whose link is a LAN. */
#define DROPBEAR_ZLIB 0

/*
 * No X11 forwarding (there is no X), no agent forwarding (no agent).
 * Local and remote port forwarding stay: they are the useful half of
 * ssh on a machine like this.
 */
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 1
#define DROPBEAR_CLI_REMOTETCPFWD 1
#define DROPBEAR_SVR_LOCALTCPFWD 1
#define DROPBEAR_SVR_REMOTETCPFWD 1

/* Where things live on this machine. */
#define DROPBEAR_DEFAULT_CLI_AUTHKEY ".ssh/authorized_keys"

/*
 * One connection at a time is plenty, and each one costs a process
 * with a pty. The default is 30, on a machine with 64 MB.
 */
#define MAX_UNAUTH_CLIENTS 3
