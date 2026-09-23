#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fsimg - put files on the machine's disk image, and read them back,
# from the host.
#
# One place where the host knows how to reach the filesystem inside a
# disk image, so that Makefiles and test suites do not each carry their
# own spelling of it.  Every operation works on the plain image file:
# nothing here needs root and nothing here needs a loop device, which is
# what makes `make write` a one-liner and what lets a test verify the
# kernel's writes with somebody else's code rather than with the code
# that did the writing.
#
# The filesystem is ext2 and the tools are e2fsprogs.  e2fsprogs reaches
# a filesystem inside a partitioned image through the "?offset=" suffix
# on the device name, which every one of its tools understands; that is
# the whole trick, and it is why no partition ever has to be extracted
# with dd first.
#
# Usage:
#   fsimg.sh IMG mkfs [LABEL]        make the filesystem (destroys it)
#   fsimg.sh IMG put SRC... DST      copy host files in, overwriting
#                                    (DST may be a directory)
#   fsimg.sh IMG put -m MODE SRC DST ... and set its permissions
#   fsimg.sh IMG put -r DIR DST      copy a host directory tree in
#   fsimg.sh IMG get SRC DST         copy a file out
#   fsimg.sh IMG get -r DIR DEST     copy a directory tree out
#   fsimg.sh IMG cat PATH            write a file to stdout
#   fsimg.sh IMG ls DIR              names in DIR, one per line
#   fsimg.sh IMG ls-l DIR            names with mode, size and time
#   fsimg.sh IMG exists PATH         exit 0 if it is there
#   fsimg.sh IMG isdir PATH          exit 0 if it is a directory
#   fsimg.sh IMG size PATH           its size in bytes, on stdout
#   fsimg.sh IMG mkdir DIR           make DIR and any parent of it
#   fsimg.sh IMG rm PATH...          delete files
#   fsimg.sh IMG rmdir DIR           delete an empty directory
#   fsimg.sh IMG mv FROM TO          rename
#   fsimg.sh IMG alloc PATH MB       a file of MB megabytes with every
#                                    block really allocated (no holes)
#   fsimg.sh IMG df                  total, used, free and AVAILABLE bytes,
#                                    and the inode counts
#   fsimg.sh IMG fsck [-p]           check it; -p repairs
#   fsimg.sh IMG batch               debugfs commands on stdin, one session
#
# The offset comes from PART_OFFSET in the environment, or from the
# partition table in the image, or is zero for an image that is nothing
# but a filesystem.

set -u

IMG=${1:?usage: fsimg.sh IMG COMMAND [ARGS]}
CMD=${2:?usage: fsimg.sh IMG COMMAND [ARGS]}
shift 2

TOPDIR=${TOPDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}

# Where the filesystem starts inside the image.  Asked of the image
# itself rather than assumed, so that a caller which knows nothing about
# the layout still works; PART_OFFSET overrides it for speed.
fsimg_offset() {
    if [ -n "${PART_OFFSET:-}" ]; then
        echo "$PART_OFFSET"
        return
    fi
    local start
    start=$(sfdisk -J "$IMG" 2>/dev/null |
            sed -n 's/.*"start": *\([0-9]*\).*/\1/p' | head -1)
    if [ -n "$start" ]; then
        echo $((start * 512))
    else
        echo 0
    fi
}

OFF=$(fsimg_offset)
if [ "$OFF" = 0 ]; then
    DEV="$IMG"
else
    DEV="$IMG?offset=$OFF"
fi

# debugfs says what it is doing on stderr and echoes each command; only
# the payload is wanted.  It also exits 0 on a failed command, so
# failure has to be read out of the text -- which is why every wrapper
# below checks rather than trusting the status.
dbg() {                         # dbg COMMANDS...  (one per argument)
    local out
    out=$(printf '%s\n' "$@" quit | debugfs -w "$DEV" 2>&1)
    printf '%s\n' "$out"
}

dbg_ro() {
    local out
    out=$(printf '%s\n' "$@" quit | debugfs "$DEV" 2>&1)
    printf '%s\n' "$out"
}

# Strip debugfs's banner and its echo of each command.  Whole lines,
# not just the prefix: stripping "debugfs:  " off the echo of a command
# leaves the command itself looking like output, which is how `ls-l`
# came to report a file called "quit".
dbg_clean() {
    sed -e '/^debugfs/d'
}

die() { echo "fsimg: $*" >&2; exit 1; }

# debugfs splits its command line on whitespace, so every path handed to
# it is quoted -- otherwise "A Long Name.txt" arrives as three arguments
# and the command fails with a usage message that looks like a bug in
# the caller. (A name containing a double quote cannot be expressed this
# way; nothing here makes one.)
q() { printf '"%s"' "$1"; }

# Does a path exist?  `stat` on a missing file says "File not found".
fs_exists() {
    ! dbg_ro "stat $(q "$1")" 2>/dev/null | grep -q "File not found"
}

# Copy a host directory's CONTENTS into a directory on the image.
fs_put_tree() {                 # fs_put_tree SRCDIR DSTDIR
    local src=$1 dst=${2%/}
    fs_mkdir_p "$dst"
    {
        (cd "$src" && find . -type d ! -name .) |
            sed "s|^\./|mkdir \"$dst/|; s|$|\"|"
        (cd "$src" && find . -type f) | while read -r f; do
            echo "rm \"$dst/${f#./}\""
            echo "write \"$src/${f#./}\" \"$dst/${f#./}\""
        done
    } | debugfs -w -f - "$DEV" >/dev/null 2>&1
}

fs_mkdir_p() {                  # every component, parents first
    local path=$1 acc=""
    local IFS=/
    local comp
    for comp in $path; do
        [ -z "$comp" ] && continue
        acc="$acc/$comp"
        if ! fs_exists "$acc"; then
            dbg "mkdir $(q "$acc")" >/dev/null
        fi
    done
}

case "$CMD" in
mkfs)
    LABEL=${1:-SAGE040}
    BS=${FS_BLOCK_SIZE:-4096}
    # -O ^dir_index: the kernel's driver refuses a hashed directory
    #    rather than write into one it cannot maintain.
    # -O ^resize_inode: nothing here ever resizes the volume, and the
    #    reserved descriptor blocks would just be metadata the driver
    #    has to step over.
    # -I 256: room for the inode's "extra" word, which is what carries
    #    a date past 2038.
    SIZE=$(stat -c %s "$IMG")
    BLOCKS=$(( (SIZE - OFF) / BS ))
    mke2fs -q -t ext2 -b "$BS" -I 256 -O ^dir_index,^resize_inode \
           -L "$LABEL" -E offset="$OFF" -F "$IMG" "$BLOCKS" ||
        die "mke2fs failed"
    ;;
put)
    MODE=""
    if [ "${1:-}" = "-r" ]; then
        shift
        SRC=${1:?put -r: need a source directory}
        DST=${2:?put -r: need a destination directory}
        [ -d "$SRC" ] || die "not a directory: $SRC"
        # One debugfs session for the whole tree: a header directory of
        # 300 files is 300 process starts otherwise, and `make install`
        # does several of them.
        fs_put_tree "$SRC" "$DST"
        exit 0
    fi
    if [ "${1:-}" = "-m" ]; then MODE=$2; shift 2; fi
    [ $# -ge 2 ] || die "put: need a source and a destination"
    # The last argument is the destination.  When it names a directory
    # -- it ends in "/", or it is one already -- every source goes into
    # it under its own basename, which is what lets a caller pass a
    # glob: `put bin/* /bin/`.
    DST=${@: -1}
    set -- "${@:1:$#-1}"
    DSTDIR=""
    case "$DST" in
    */) DSTDIR=${DST%/} ;;
    *)  if [ $# -gt 1 ] || "$0" "$IMG" isdir "$DST" 2>/dev/null; then
            DSTDIR=$DST
        fi ;;
    esac

    cmds=()
    modes=()
    for SRC in "$@"; do
        #
        # A DIRECTORY AMONG THE SOURCES IS COPIED, not refused.
        #
        # `put DIR/* /X/` is how a caller stages a directory's contents,
        # and a tree of test data has subdirectories in it. Dying on the
        # first one aborted the whole staging, and what that looked like
        # afterwards was every one of grep's thirty cases failing with
        # "No such file or directory" -- as though grep were broken.
        #
        if [ -d "$SRC" ]; then
            if [ -n "$DSTDIR" ]; then
                fs_put_tree "$SRC" "$DSTDIR/$(basename "$SRC")"
            else
                fs_put_tree "$SRC" "$DST"
            fi
            continue
        fi
        [ -f "$SRC" ] || die "no such file: $SRC"
        if [ -n "$DSTDIR" ]; then
            TARGET="$DSTDIR/$(basename "$SRC")"
        else
            TARGET=$DST
        fi
        DIR=$(dirname "$TARGET")
        if [ "$DIR" != "/" ] && [ "$DIR" != "." ]; then
            fs_mkdir_p "$DIR"
        fi
        # write refuses an existing name, so the old one goes first.
        cmds+=("rm $(q "$TARGET")" "write $(q "$SRC") $(q "$TARGET")")
        # sif sets the WHOLE of i_mode, so the file-type bits have to be
        # part of it: a mode of plain 0755 leaves an inode that is not a
        # regular file, which e2fsck reports and the kernel will not run.
        if [ -n "$MODE" ]; then
            modes+=("sif $(q "$TARGET") mode 0100${MODE#0}")
        fi
    done
    out=$(dbg "${cmds[@]}" ${modes[@]+"${modes[@]}"})
    if echo "$out" | grep -qiE "error|could not"; then
        echo "$out" | dbg_clean >&2
        die "could not write into $DST"
    fi
    ;;
get)
    if [ "${1:-}" = "-r" ]; then
        shift
        SRC=${1:?get -r: need a source directory}
        DEST=${2:?get -r: need a destination directory}
        mkdir -p "$DEST"
        # rdump warns that it cannot set ownership when it is not root.
        # That is expected and does not mean the files are missing.
        dbg_ro "rdump $(q "$SRC") $(q "$(cd "$DEST" && pwd)")" |
            grep -v "while changing ownership" | dbg_clean >&2
        exit 0
    fi
    SRC=${1:?get: need a source}
    DST=${2:?get: need a destination}
    fs_exists "$SRC" || die "no such file: $SRC"
    rm -f "$DST"
    dbg_ro "dump $(q "$SRC") $(q "$DST")" >/dev/null
    [ -e "$DST" ] || die "could not read $SRC"
    ;;
cat)
    SRC=${1:?cat: need a path}
    fs_exists "$SRC" || die "no such file: $SRC"
    TMP=$(mktemp)
    dbg_ro "dump $(q "$SRC") $(q "$TMP")" >/dev/null
    cat "$TMP"
    rm -f "$TMP"
    ;;
ls)
    DIR=${1:-/}
    # `ls -p` prints /inode/mode/uid/gid/name/size/ -- a name cannot
    # contain a slash, so splitting on one is exact.
    dbg_ro "ls -p $(q "$DIR")" | dbg_clean |
        awk -F/ '/^\// { if ($6 != "" && $6 != "." && $6 != "..") print $6 }'
    ;;
ls-l)
    DIR=${1:-/}
    dbg_ro "ls -l $(q "$DIR")" | dbg_clean | grep -v '^$'
    ;;
exists)
    fs_exists "${1:?exists: need a path}"
    ;;
isdir)
    P=${1:?isdir: need a path}
    dbg_ro "stat $(q "$P")" | grep -q "Type: directory"
    ;;
size)
    P=${1:?size: need a path}
    fs_exists "$P" || die "no such file: $P"
    dbg_ro "stat $(q "$P")" | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1
    ;;
mkdir)
    fs_mkdir_p "${1:?mkdir: need a path}"
    ;;
rm)
    [ $# -gt 0 ] || die "rm: need a path"
    args=()
    for p in "$@"; do args+=("rm $(q "$p")"); done
    dbg "${args[@]}" >/dev/null
    ;;
rmdir)
    D=${1:?rmdir: need a path}
    dbg "rmdir $(q "$D")" >/dev/null
    ;;
mv)
    FROM=${1:?mv: need a source}
    TO=${2:?mv: need a destination}
    # A directory carries its parent in its own ".." entry, and nothing
    # here rewrites that, so a directory may only be renamed within the
    # directory it is already in. Moving one elsewhere would leave ".."
    # pointing at the old parent, which e2fsck reports and a path walk
    # believes.
    if [ "$(dirname "$FROM")" != "$(dirname "$TO")" ] &&
       "$0" "$IMG" isdir "$FROM" 2>/dev/null; then
        die "mv: cannot move a directory to a different parent ($FROM -> $TO)"
    fi
    out=$(dbg "rm $(q "$TO")" "ln $(q "$FROM") $(q "$TO")" "unlink $(q "$FROM")")
    # An `if`, not `cmd && { ... }`: the && form leaves the status of the
    # grep as the script's own, so a rename that worked exited 1 and
    # every caller chaining on && silently stopped there.
    if echo "$out" | grep -qi "error"; then
        echo "$out" | dbg_clean >&2
        die "could not rename $FROM"
    fi
    ;;
alloc)
    #
    # A FILE WITH NO HOLES, which is not what `put` of a file of zeroes
    # gives you.
    #
    # debugfs writes an all-zero input as a fully sparse file -- size
    # right, Blockcount 0, no blocks at all -- which is correct ext2 and
    # useless as a swap file: swapon maps every page through the
    # filesystem's bmap once, and a hole has no block to name. Linux
    # refuses a swap file with holes for the same reason ("it appears to
    # have holes"), and so does this kernel.
    #
    # Filling with a non-zero byte is what makes the blocks real.
    #
    P=${1:?alloc: need a path}
    MB=${2:?alloc: need a size in megabytes}
    TMP=$(mktemp)
    head -c $((MB * 1024 * 1024)) /dev/zero | tr '\0' 'S' > "$TMP"
    "$0" "$IMG" put "$TMP" "$P"
    rm -f "$TMP"
    ;;
df)
    # From e2fsck's own summary line, which is the host counting the
    # bitmaps rather than believing the superblock:
    #   "LABEL: 33/3840 files (0.0% non-contiguous), 335/3840 blocks"
    # dumpe2fs is not used here: it does not accept the "?offset=" form.
    line=$(e2fsck -fn "$DEV" 2>&1 | tail -1)
    super=$(dbg_ro "show_super_stats -h")
    bs=$(printf '%s\n' "$super" | sed -n 's/^Block size: *//p' | head -1)
    [ -n "$bs" ] || bs=4096
    # FREE is not AVAILABLE. ext2 keeps a reserved percentage that only
    # root may dip into, and the machine's `df` reports the available
    # figure in its "avail" column, as Linux's does. Comparing one with
    # the other is a 5%-of-the-disk discrepancy that looks like a bug in
    # whichever of them you trust less.
    resv=$(printf '%s\n' "$super" | sed -n 's/^Reserved block count: *//p' | head -1)
    [ -n "$resv" ] || resv=0
    iu=${line#*: }; iu=${iu%%/*}
    it=$(echo "$line" | sed -n 's|.*files.*, *[0-9]*/\([0-9]*\) blocks.*|\1|p')
    bu=$(echo "$line" | sed -n 's|.*[, ]\([0-9]*\)/[0-9]* blocks.*|\1|p')
    itot=$(echo "$line" | sed -n 's|.*: *[0-9]*/\([0-9]*\) files.*|\1|p')
    echo "blocksize $bs"
    echo "blocks_total $it"
    echo "blocks_used $bu"
    echo "blocks_free $((it - bu))"
    echo "blocks_reserved $resv"
    echo "blocks_avail $((it - bu - resv < 0 ? 0 : it - bu - resv))"
    echo "bytes_total $((it * bs))"
    echo "bytes_free $(((it - bu) * bs))"
    echo "bytes_avail $(((it - bu - resv < 0 ? 0 : it - bu - resv) * bs))"
    echo "inodes_total $itot"
    echo "inodes_used $iu"
    echo "inodes_free $((itot - iu))"
    ;;
fsck)
    if [ "${1:-}" = "-p" ]; then
        e2fsck -fy "$DEV"
        exit $?
    fi
    #
    # THE EXIT STATUS OF `e2fsck -fn` IS NOT THE WHOLE ANSWER.
    #
    # A superblock whose free-block or free-inode count disagrees with
    # the bitmaps is reported -- "Free blocks count wrong (3903,
    # counted=3826)" -- and e2fsck still exits 0. A test that judges by
    # the status alone therefore calls that filesystem clean, which is
    # exactly the fault a driver that miscounts would leave behind.
    #
    # So: clean means status 0 AND nothing said.
    #
    out=$(e2fsck -fn "$DEV" 2>&1)
    st=$?
    printf '%s\n' "$out"
    [ "$st" = 0 ] || exit "$st"
    if printf '%s\n' "$out" |
       grep -qE "wrong|differences|Fix\?|Clear\?|Unattached|invalid|illegal|WARNING"; then
        exit 4
    fi
    ;;
batch)
    debugfs -w -f - "$DEV" 2>&1 | dbg_clean
    ;;
offset)
    echo "$OFF"
    ;;
*)
    die "unknown command: $CMD"
    ;;
esac
