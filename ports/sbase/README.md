# sbase on SuckOS

[sbase](https://core.suckless.org/sbase/), suckless's POSIX utilities --
98 of them, from `basename` to `yes`, `sort`, `find`, `xargs`, `tar`,
`make`, `ed`, `bc`, `dc` and the checksums among them -- built against
picolibc and installed in `/bin`.

```bash
make libc                    # once: picolibc
make -C ports/sbase          # clone, build ./bin
make -C ports/sbase install  # onto hd.img, in /BIN
make sbasetest               # its tests, on the machine
```

`grep` and `sed` are left out: GNU's are installed (`ports/grep`,
`ports/sed`). Where a utility has the same name as a builtin of the
shell -- `ls`, `cp`, `mv`, `rm`, `mkdir`, `cat`, `echo`, `date`, `test`
and a few more -- the builtin is what runs at the prompt, and the
program is what `/bin/NAME`, scripts, `xargs`, `find -exec` and awk's
`system()` reach.

## The source is not here

MIT-licensed, cloned at a fixed commit into `~/m68k/src` and built from
there. sbase's own Makefile builds its `make` first and runs it, which
cannot work for another machine, so `build.sh` makes sbase's two
libraries with it and compiles each utility itself.

`patches/` fixes two upstream bugs, each described at its head: `rev`
printed every line unreversed and `tail -m` counted the wrong bytes
(a macro substituted with the opposite meaning), and `od -t x1` -- the
POSIX form -- printed its usage.

## Tests

`kernel/sbasetest.sh` runs 65 command lines (`tests/cases.txt`) on the
machine and the same lines on the host with GNU's tools, and compares
them: sort orders, cut and tr, head and tail, od dumps, cmp and comm,
the checksums and `cksum`'s CRC, `bc` to twenty places of pi, find and
xargs. Then what the disk records: a tar archive the host's tar reads, a
file `dd` wrote, a date `touch` set, `make` rebuilding only when its
source is newer, `ed` editing in place, a directory tree copied, moved
and removed, `ln` refused (FAT has no links), and a uuencode round trip.
