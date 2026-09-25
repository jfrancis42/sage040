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
 * SERVER PASSWORD AUTHENTICATION IS ON.
 *
 * It was off, and the comment here said why: checking a password means
 * crypt(3) against a hash in /etc/shadow, and this machine had
 * neither. It has both now -- crypt is $6$ SHA-512 in
 * libc/picolibc/libos/linux/machine/m68k/crypt.c, and /etc/shadow is
 * written by passwd(1) and useradd(8) -- so ssh can ask the same
 * question the console asks.
 *
 * Dropbear reads the hash through getpwnam()'s pw_passwd, so the C
 * library has to hand it the SHADOW hash rather than the 'x' that is
 * in /etc/passwd; see the shadow patch in patches/.
 *
 * The CLIENT keeps password auth for the separate reason it always
 * did: sending a password to somebody else's server needs no crypt
 * here, and refusing to type one would make this client useless
 * against half the hosts in the world.
 */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
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

/* The client's default identity -- the private key `ssh host` uses when
 * no -i is given. It must be a ~/-anchored path (expand_homedir_path
 * only expands a leading "~/"; a bare ".ssh/..." is taken relative to the
 * current directory, so it only worked when you happened to be in your
 * home), and it must NOT be authorized_keys, which is the server's list
 * of PUBLIC keys, not a private identity. */
#define DROPBEAR_DEFAULT_CLI_AUTHKEY "~/.ssh/id_dropbear"

/*
 * One connection at a time is plenty, and each one costs a process
 * with a pty. The default is 30, on a machine with 64 MB.
 */
#define MAX_UNAUTH_CLIENTS 3
