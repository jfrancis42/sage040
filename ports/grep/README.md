# GNU grep on SuckOS

GNU grep 3.12, built against picolibc and running as `/bin/grep`.

```bash
make libc                   # once: picolibc
make -C ports/grep          # fetch, verify, cross-configure, build ./grep
make -C ports/grep install  # onto hd.img as /BIN/GREP
make greptest               # its tests, on the machine
```

The release is fetched from ftp.gnu.org, checked against a pinned
SHA-256 (its GPG signature was checked against the GNU keyring when it
was pinned) and built out of tree in `~/m68k/src` with
`ports/cross.sh`. No patches to grep. Without PCRE: `-P` is not there.

## Tests

`kernel/greptest.sh`:

- **grep's own pattern tables** -- `bre.tests`, `ere.tests`,
  `spencer1.tests`, 329 patterns with an input and the exit status
  upstream requires -- run on the machine as one shell script and graded
  against the tables themselves. Rows upstream marks as known
  non-conformance are skipped, as its own test skips them.
- **32 tests of the options** in `tests/` -- case folding, inversion,
  counts, context, only-matching, fixed strings, pattern files, binary
  files, NUL-separated records, UTF-8, missing files, and `-r` over a
  directory tree -- compared with the same grep source built for the
  host. `-r` output is compared sorted: it lists files in directory
  order, which is the filesystem's.
