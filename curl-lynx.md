# curl and lynx: what porting them would take

This is an estimate, and nothing in it has been built. Two kinds of
statement appear below, and they are marked:

- **[checked]**: read from this tree on 2026-09-26.
- **[estimate]**: a judgement, based on how the existing ports went.

## Summary

| | Effort | What blocks it |
|--|--|--|
| curl | **done 2026-09-26** | `ports/curl` (8.22.0), `make curltest` 16/16 |
| lynx | **done 2026-09-27** | `ports/lynx` (2.9.3, wide curses), `make lynxtest` 16/16 |
| wide-character curses and a UTF-8 console | 2–4 days | `fbcon` decodes no UTF-8 |
| everything under "Extras" | ½–1 day each | nothing; all optional |

Do curl first. It forces the certificate and TLS-speed questions to be
answered, and lynx then only adds configure work and a UI test on top.

## What already exists [checked]

- **`ports/openssl`**: 3.5.4, a static library, built with `no-asm`,
  `no-dso` and `--openssldir=/etc/ssl`.
- **`ports/zlib`**. The tree also has `ports/zstd`, `ports/bzip2`,
  `ports/xz` and `ports/libiconv`.
- **`ports/ncurses`**, as a narrow-character static build with
  `--with-termlib` and `--enable-termcap`. vt102, vt100, dumb and
  unknown are compiled in as fallbacks, and the terminfo database is
  installed on the disk.
- **The libc**: sockets, `select`/`poll`, `fork`/`exec`, `sigaction`,
  `termios` and real pthreads. There is no `pthread_cancel` and no
  `__thread`.
- **picolibc is built `mb-capable`**, so `wchar.h`, `wcwidth`,
  `locale.h` and `langinfo.h` are all present.
- **A real resolver** (`libc/net/src/resolv.c`, tested by `dnstest`).
  `getaddrinfo` works through numeric names, `/etc/hosts`, a cache, and
  then DNS to the servers in `resolv.conf` or the ones DHCP supplied.
- **`ports/cross.sh`** already handles cross builds with autoconf's
  `--host=m68k-unknown-elf`.

## What is missing [checked]

- ~~**A CA bundle.**~~ **Done 2026-09-26:** `ports/ca-certs` installs
  Mozilla's roots (curl.se extract `2026-09-25`, pinned by SHA-256, 121
  CAs) at `/etc/ssl/cert.pem`, which is OpenSSL's default path.
  `make catest` checks that a real chain verifies with no `-CAfile`,
  with two negative controls.
- **UTF-8 on the framebuffer console.** `kernel/fbcon.c` has 16 colours,
  bold, underline and reverse video, but nothing decodes UTF-8. Its font
  is the VGA ROM font, which is CP437.
- **A mouse.** `i8042.c` throws away the aux port's bytes.

---

## curl: about ½ day [estimate]

1. **Add `ports/curl/build.sh` and a Makefile**, following the pattern
   of the other ports:

   ```
   --host=m68k-unknown-elf --with-openssl=<static openssl> --with-zlib
   --disable-threaded-resolver   # use the libc's blocking getaddrinfo
   --disable-ldap --disable-ldaps --without-libpsl --without-brotli
   --without-nghttp2 --without-libidn2 --disable-manual
   --with-ca-bundle=/etc/ssl/cert.pem
   ```

   Link it statically against libcurl. Nothing else here needs the
   shared library yet.

2. **The CA bundle is done** (`ports/ca-certs`). Point curl at it with
   `--with-ca-bundle=/etc/ssl/cert.pem`, or leave it unset so that curl
   uses OpenSSL's default, which is the same file.

3. **Expect a couple of `-Werror`-style fixes**, of the usual kind:
   `uint32_t` is `long unsigned int` on this target, and picolibc has
   the odd gap.

4. **Add a test suite, `curltest.sh`, on the `nettest.sh` pattern:**
   - plain http against a web server on the host, comparing the
     downloaded file byte for byte;
   - https against a local `openssl s_server` whose certificate is
     signed by a test CA that the suite installs. Add a negative control
     that swaps in the wrong CA and asserts a verification failure.
   - redirects, `-C -` resume, and a large download (more than 10 MB) to
     exercise the TCP receive path;
   - one check that decodes something on the host side, as `synopts.py`
     does, so the test is not checking the machine against itself.

The risk here is TLS speed [estimate]. The build uses `no-asm`, which
means portable C bignum code on a 25 MHz 68040. An RSA-2048 verify is
cheap, but ECDHE key exchange plus verifying the whole chain could
plausibly take several seconds per handshake. Measure it with
`openssl speed` and a timed `curl` before worrying about it.

## lynx: 1–2 days [estimate]

1. **Configure it for what exists today:**

   ```
   --with-screen=ncurses --with-ssl=<openssl> --with-zlib
   --disable-nls --disable-full-paths --without-bzlib
   --enable-default-colors --enable-color-style
   ```

   Lynx uses Dickey's old-style configure (the same author as ncurses),
   and it cross-compiles grudgingly. Several `AC_TRY_RUN` checks need
   cache overrides.

2. **Handle the host-side tool.** The build compiles and runs
   `makeuctb` to generate its character-set tables, so it needs
   `BUILD_CC` in the way `ports/ncurses/build.sh` already does it.

3. **Install its files on the disk:** `/etc/lynx.cfg` and `lynx.lss`,
   with `$HOME`, a writable `/tmp`, and `TERM` set to something in
   terminfo.

4. **Set sensible defaults in `lynx.cfg`** for this machine: a small
   cache, cookies saved to `~/.lynx_cookies`, and an external editor
   that exists here (uEmacs or vi).

5. **Tests:**
   - `lynx -dump` and `-source` against the host web server. This
     covers the fetch path and the HTML rendering without any screen.
   - an interactive session over serial driven the way `edittest.sh`
     does it (open a page, follow a link, go back, quit), checked with
     `/dev/vcsa` or a screenshot.
   - https through the same test CA that curltest uses.

Until curses is wide, lynx can only render in 8-bit charsets. UTF-8
pages will be transliterated or come out wrong, depending on the
`CHARACTER_SET` setting and what the terminal on the other end does.

## A better curses for full support: 2–4 days [estimate]

This comes in three layers. Only the first is needed for lynx over a
serial or ssh session to a modern terminal. The other two are needed
for the machine's own screen.

**1. ncursesw, about ½–1 day.** Rebuild `ports/ncurses` with
`--enable-widec` and install it alongside the narrow library, or in
place of it (`less` and uEmacs would then need relinking). picolibc is
already `mb-capable`. The work is checking that picolibc's `wcwidth`
and `mbrtowc` give correct answers in `C.UTF-8`. Test that against the
host, remembering that a test which checks a system against itself proves
little. Then build lynx with `--with-screen=ncursesw`. This
also gives UTF-8 to every other curses program. `--enable-ext-colors`
and `--enable-ext-mouse` cost nothing extra at the same time.

**2. UTF-8 in `fbcon`, about 1–2 days.** This is the real gap for the
local console:

- a UTF-8 decoder in the VT102 state machine;
- a Unicode-to-glyph table with a replacement glyph for anything
  unmapped. The VGA font covers CP437, which is enough for box drawing,
  Latin-1, some Greek and some maths.
- a vcsa cell that can hold more than a byte, or a separate code-point
  array beside it;
- a terminfo entry that tells the truth. Today that is probably a
  custom `sage` entry with `U8#1` and the right `acsc`.

A font with more than 256 glyphs, such as a Terminus or unifont subset,
would add another day or so, and it is where the memory cost appears:
at 16 bytes per glyph, 4K glyphs is 64 KB.

**3. Colours and attributes, about ½ day, optional.** fbcon has 16
colours. Adding the 256-colour SGR (`38;5;n`) and mapping it down to
the nearest of the 16 would let `TERM=xterm-256color`-style programs
work unmodified. Lynx itself is fine with 8 or 16 colours.

**Mouse, 1 day or more, optional.** A PS/2 mouse driver on the aux
port, plus xterm mouse reporting from fbcon. Only worthwhile if
something beyond lynx wants it.

## Extras that would make either or both better

In rough order of value:

1. **A shared libcurl** (`libcurl.so`), once there is a second user of
   it. Python's `pycurl`, git over http and `pkg`-style tools would all
   link against it. The dynamic loader already exists; the OpenSSL
   build notes that it avoids a shared build because of versioned
   sonames, and curl's is simpler.
2. **An `openssl speed` baseline**, checked in, so that "TLS is slow"
   becomes a number. If handshakes do hurt, the two cheap fixes are
   preferring ECDSA and X25519, and TLS session resumption, which curl
   and lynx both support. Writing m68k bignum assembly is the expensive
   fix.
3. **An entropy check.** TLS needs good randomness. `getrandom` exists,
   but the quality of the kernel's pool on a machine with no hardware
   RNG deserves a look. On QEMU it is presumably fed from timing
   [estimate; not checked].
4. ~~**HTTP/2 through nghttp2**~~ **Done 2026-09-27:** `ports/nghttp2`
   (library only). curl negotiates h2 by ALPN; curltest checks it
   against an h2-only server.
5. ~~**libidn2 and libpsl**~~ **Done 2026-09-27:** `ports/libunistring`,
   `ports/libidn2`, `ports/libpsl` (list compiled in). IDN needs
   `LANG=C.UTF-8`, as on Linux; the machine sets no LANG by default.
6. ~~**Brotli.**~~ **Done 2026-09-27:** `ports/brotli`, library and
   `brotli` command.
7. ~~**`wget`**~~ **Done 2026-09-27:** `ports/wget` (1.25.0, OpenSSL,
   PSL, IRI, zlib), `make wgettest` 22/22.
8. **Proxy through the host.** On slirp networking,
   `http_proxy`/`https_proxy` pointed at a proxy on the host would let
   test suites run without internet access. A small harness addition.
9. **A timezone database**, for correct cookie expiry and
   `If-Modified-Since`. Only needed if the machine does not run in UTC.
10. **Lynx's external viewers**, so that images and PDFs can be handed
    to something. Nothing here can display them yet, so this waits
    until there is a program that can.

## Order of work

1. ~~The CA bundle, on its own.~~ Done: `ports/ca-certs`, `make catest`.
2. ~~curl and curltest.~~ Done. TLS handshake measured at ~0.5 s locally (RSA-2048) and ~1.2–1.8 s to curl.se, which is fine. Porting it found a libc resolver crash, now fixed.
3. ~~lynx with lynxtest.~~ Done, built WIDE: ports/ncurses already builds libncursesw, so the ncursesw step below was never needed. Found and patched an upstream Lynx bug: FORCE_SSL_PROMPT:NO accepted unverifiable certificates (ports/lynx patch 02). Added ports/gzip, which Lynx needs on PATH to decode gzip.
4. ncursesw, then rebuild lynx with it.
5. UTF-8 in fbcon, if lynx on the local screen matters.
