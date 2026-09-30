# Perl on SuckOS

**Perl 5.44.0**, cross-built against picolibc with
[perl-cross](https://github.com/arsv/perl-cross) 1.6.5, 64-bit integers,
and every XS extension a shared object that DynaLoader `dlopen()`s.

```bash
make -C ports/perl            # fetch, patch, cross-configure, build
make -C ports/perl install    # onto hd.img: /usr/bin/perl, its programs, the library
make perltest                 # the suite, on the machine
```

```
/$ perl -MDigest::SHA=sha256_hex -le 'print sha256_hex("abc")'
ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
```

## The source is not here

`build.sh` fetches Perl from cpan.org and perl-cross from GitHub, checks
both against pinned SHA-256s, lays perl-cross over the Perl tree, applies
`patches/`, and builds in `~/m68k/src/build-perl-sage040`.

Perl's own Configure cannot cross-compile: it answers its questions by
running test programs. perl-cross answers them by compiling only, and
builds a host miniperl to run the build's Perl scripts. Where its
answers were wrong for this system they are given on the command line,
and `build.sh` says why for each.

## Patches

- **01** Errno finds `errno.h` through `$Config{usrinc}` before the
  host's `/usr/include` -- which, cross-compiling, is glibc's.
- **02** POSIX: picolibc declares `tzname` as `char * const[2]`, and
  has `termios.h` without `ctermid()`.

## XS modules built on the machine

The installed `Config.pm` describes the machine, not the build host:
`cc` is `gcc`, `lddlflags` is `-shared`, `usrinc` is `/usr/include`.
MakeMaker's Makefiles run their recipes with `/bin/sh`, which is bash
(ports/bash installs the link; the system's own shell is `/bin/msh`,
and is not POSIX). With the
native toolchain (`make toolchain`) and GNU make (`ports/make`),
`perl Makefile.PL && make` builds an XS module on the machine; the suite
does exactly that.

## Size

58 MB installed, most of it the library: 11 MB of it is `pod/`, which
`perldoc` reads. No manual pages -- there is no `man`.
