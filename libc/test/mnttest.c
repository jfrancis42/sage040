/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mnttest - mount(2) and umount2(2), run by kernel/mounttest.sh.
 *
 * The disk it expects: hda1 the root, with directories /mnt and /ro and
 * a file /mnt/shadow.txt; hda2 an ext2 volume holding /hello.txt; hda3
 * another holding /ro.txt. What it checks is that a mounted volume is a
 * place of its own -- its own device number, its own inode numbers, its
 * own free space -- that paths cross into it and back out of it, that
 * nothing can be linked or moved across the boundary, that a volume in
 * use cannot be taken away, and that read-only means it.
 *
 * Every number compared here comes from two different calls (stat
 * against statfs, the mounted volume against the root), never from one
 * call checked against itself.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#ifndef MS_RDONLY
#define MS_RDONLY 1
#endif

static int checks, failures;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failures++;
    }
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

/* The raw calls, with errno: picolibc has no mount(). */
#define MS_REMOUNT_ 32           /* Linux's MS_REMOUNT */

/* A volume's superblock as the DISK has it: s_state in the low half
 * (1: clean), the needs_recovery bit (incompat 4) shifted to 0x40000. */
static unsigned sb_state(const char *dev)
{
    unsigned char sb[1024];
    int fd = open(dev, O_RDONLY);
    unsigned st, inc;

    if (fd < 0 || lseek(fd, 1024, SEEK_SET) != 1024 ||
        read(fd, sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
        if (fd >= 0) {
            close(fd);
        }
        return 0xdead;
    }
    close(fd);
    st = sb[58] | (sb[59] << 8);
    inc = sb[96] | (sb[97] << 8) | ((unsigned)sb[98] << 16) | ((unsigned)sb[99] << 24);
    return st | ((inc & 4) << 16);
}

static int do_mount(const char *src, const char *dir, const char *type,
                    unsigned long flags)
{
    return (int)syscall(21, src, dir, type, flags, 0);
}

static int do_umount(const char *dir)
{
    return (int)syscall(52, dir, 0);
}

/* Did the call fail with exactly `e`? */
static int fails(int r, int e)
{
    return r < 0 && errno == e;
}

static int has_line(const char *path, const char *want)
{
    char buf[2048];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    return strstr(buf, want) != 0;
}

static int file_is(const char *path, const char *want)
{
    char buf[256];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    return close(fd) == 0 && ok;
}

static int listed(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, name) == 0) {
            found = 1;
        }
    }
    closedir(d);
    return found;
}

int main(void)
{
    struct stat root, mnt, st, st2;
    struct statvfs rs, ms;
    char cwd[256];
    int fd, r;
    pid_t p;

    printf("mnttest: start\n");

    /* --- before: the mount point is an ordinary directory ------------ */
    check("before mounting, /mnt holds the root volume's shadow.txt",
          file_is("/mnt/shadow.txt", "under the mount\n"));
    check("/proc/mounts names the root as /dev/hda1 on /",
          has_line("/proc/mounts", "/dev/hda1 / "));

    /* The disks as nodes in /dev: block devices, Linux's numbers, and
     * readable -- checked against bytes known another way: the MBR's
     * signature at 510 of the disk, ext2's magic at 1080 of a volume. */
    {
        struct stat st;
        unsigned char b2[2] = { 0, 0 }, m2[2] = { 0, 0 };
        int fd;

        check("/dev/hda2 is a block device, 3:2",
              stat("/dev/hda2", &st) == 0 && S_ISBLK(st.st_mode) &&
              major(st.st_rdev) == 3 && minor(st.st_rdev) == 2);
        check("  and /dev/hda, the whole disk, is 3:0",
              stat("/dev/hda", &st) == 0 && S_ISBLK(st.st_mode) &&
              major(st.st_rdev) == 3 && minor(st.st_rdev) == 0);
        check("  and ls /dev lists them", listed("/dev", "hda2") &&
              listed("/dev", "hda"));
        fd = open("/dev/hda2", O_RDONLY);
        check("reading /dev/hda2 at 1080 gives ext2's magic, 53 ef",
              fd >= 0 && lseek(fd, 1080, SEEK_SET) == 1080 &&
              read(fd, b2, 2) == 2 && b2[0] == 0x53 && b2[1] == 0xef);
        if (fd >= 0) {
            close(fd);
        }
        fd = open("/dev/hda", O_RDONLY);
        check("reading /dev/hda at 510 gives the MBR's signature, 55 aa",
              fd >= 0 && lseek(fd, 510, SEEK_SET) == 510 &&
              read(fd, m2, 2) == 2 && m2[0] == 0x55 && m2[1] == 0xaa);
        if (fd >= 0) {
            close(fd);
        }
        fd = open("/dev/hda2", O_WRONLY);
        check("writing to one is refused: EROFS",
              fd >= 0 && write(fd, "x", 1) < 0 && errno == EROFS);
        if (fd >= 0) {
            close(fd);
        }
    }

    /* --- refusals that need nothing mounted ------------------------- */
    check("mounting a device that does not exist: ENOENT",
          fails(do_mount("/dev/hdz9", "/mnt", "ext2", 0), ENOENT));
    check("mounting an unknown type: ENODEV",
          fails(do_mount("/dev/hda2", "/mnt", "vfat", 0), ENODEV));
    check("mounting on a file: ENOTDIR",
          fails(do_mount("/dev/hda2", "/mnt/shadow.txt", "ext2", 0), ENOTDIR));
    check("mounting on a directory that does not exist: ENOENT",
          fails(do_mount("/dev/hda2", "/nowhere", "ext2", 0), ENOENT));
    check("mounting on /tmp, which is not on the disk: EINVAL",
          fails(do_mount("/dev/hda2", "/tmp", "ext2", 0), EINVAL));
    check("mounting on /: EBUSY",
          fails(do_mount("/dev/hda2", "/", "ext2", 0), EBUSY));
    check("mounting the root's own partition again: EBUSY",
          fails(do_mount("/dev/hda1", "/ro", "ext2", 0), EBUSY));
    check("  and by the whole disk's name, which reaches it too: EBUSY",
          fails(do_mount("/dev/hda", "/ro", "ext2", 0), EBUSY));
    check("unmounting something that is not a mount point: EINVAL",
          fails(do_umount("/mnt"), EINVAL));
    check("unmounting the root: EINVAL", fails(do_umount("/"), EINVAL));

    /* Only root may: a child that is not. */
    p = fork();
    if (p == 0) {
        if (setuid(1000) != 0) {
            _exit(2);
        }
        _exit(fails(do_mount("/dev/hda2", "/mnt", "ext2", 0), EPERM) ? 0 : 1);
    }
    waitpid(p, &r, 0);
    check("a user who is not root cannot mount: EPERM",
          WIFEXITED(r) && WEXITSTATUS(r) == 0);

    /* --- mount -------------------------------------------------------- */
    r = do_mount("/dev/hda2", "/mnt", "ext2", 0);
    check("mount /dev/hda2 on /mnt", r == 0);
    if (r != 0) {
        printf("mnttest: mount failed: %s\n", strerror(errno));
        printf("mnttest: %d checks, %d failed\n", checks, failures + 1);
        return 1;
    }
    check("/proc/mounts lists it", has_line("/proc/mounts", "/dev/hda2 /mnt ext3 rw"));   /* journaled */
    check("its file is there", file_is("/mnt/hello.txt", "hello from hda2\n"));
    check("and the directory's own file is hidden while it is",
          access("/mnt/shadow.txt", F_OK) != 0 && errno == ENOENT);
    check("readdir of /mnt lists the volume's names",
          listed("/mnt", "hello.txt") && !listed("/mnt", "shadow.txt"));

    check("mounting the same volume twice: EBUSY",
          fails(do_mount("/dev/hda2", "/ro", "ext2", 0), EBUSY));

    /* A place of its own. */
    stat("/", &root);
    stat("/mnt", &mnt);
    check("stat: /mnt is on a different device from /", root.st_dev != mnt.st_dev);
    check("  and is that volume's root directory, inode 2",
          mnt.st_ino == 2 && S_ISDIR(mnt.st_mode));
    lstat("/mnt", &st);
    check("  lstat agrees", st.st_dev == mnt.st_dev && st.st_ino == 2);
    statvfs("/", &rs);
    statvfs("/mnt", &ms);
    check("statvfs: a different size, and a different fsid",
          rs.f_blocks != ms.f_blocks && rs.f_fsid != ms.f_fsid);
    {
        struct statvfs fs2;
        int fd2 = open("/mnt/hello.txt", O_RDONLY);

        check("  statvfs of a file on it is the same volume",
              statvfs("/mnt/hello.txt", &fs2) == 0 &&
              fs2.f_blocks == ms.f_blocks && fs2.f_fsid == ms.f_fsid);
        check("  and so is fstatvfs of one open there",
              fd2 >= 0 && fstatvfs(fd2, &fs2) == 0 && fs2.f_fsid == ms.f_fsid);
        if (fd2 >= 0) {
            close(fd2);
        }
    }

    {
        struct statvfs ts;

        check("statvfs of /tmp is tmpfs's: its own fsid, and room in it",
              statvfs("/tmp", &ts) == 0 && ts.f_fsid != rs.f_fsid &&
              ts.f_fsid != ms.f_fsid && ts.f_blocks > 0 &&
              ts.f_bfree <= ts.f_blocks);
    }

    /* Writing on it. */
    check("a file is created on it", put("/mnt/new.txt", "new on hda2\n"));
    check("  and reads back", file_is("/mnt/new.txt", "new on hda2\n"));
    stat("/mnt/new.txt", &st);
    check("  on its device", st.st_dev == mnt.st_dev);
    check("a hard link within the volume works",
          link("/mnt/new.txt", "/mnt/new2.txt") == 0);
    stat("/mnt/new2.txt", &st2);
    check("  the same inode, link count 2",
          st2.st_ino == st.st_ino && st2.st_nlink == 2);
    check("a hard link ACROSS volumes: EXDEV",
          fails(link("/mnt/new.txt", "/crosslink"), EXDEV));
    check("  and the other way: EXDEV",
          put("/onroot.txt", "on the root\n") &&
          fails(link("/onroot.txt", "/mnt/onroot.txt"), EXDEV));
    check("rename across volumes: EXDEV",
          fails(rename("/mnt/new2.txt", "/moved.txt"), EXDEV));
    check("rename within the volume works",
          rename("/mnt/new2.txt", "/mnt/renamed.txt") == 0 &&
          access("/mnt/renamed.txt", F_OK) == 0);

    /* Paths across the boundary, both ways. */
    check("mkdir on it", mkdir("/mnt/d", 0755) == 0);
    check("chdir into it, a directory down",
          chdir("/mnt/d") == 0 && getcwd(cwd, sizeof(cwd)) &&
          strcmp(cwd, "/mnt/d") == 0);
    check("  a relative name works from there",
          put("rel.txt", "relative\n") && file_is("/mnt/d/rel.txt", "relative\n"));
    check("  .. twice comes back out to /",
          chdir("../..") == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
    stat("/mnt/..", &st);
    check("stat /mnt/.. is the root's root directory",
          st.st_dev == root.st_dev && st.st_ino == root.st_ino);
    stat("/mnt/d/..", &st);
    check("stat /mnt/d/.. is the mounted volume's root",
          st.st_dev == mnt.st_dev && st.st_ino == 2);
    check("a path that goes in and out again resolves",
          file_is("/mnt/../mnt/d/../hello.txt", "hello from hda2\n"));
    check("a symlink on the root pointing into it is followed",
          symlink("/mnt/hello.txt", "/tomnt") == 0 &&
          file_is("/tomnt", "hello from hda2\n"));
    check("a relative symlink on it pointing out of it is followed",
          symlink("../shadow-link-target", "/mnt/out") == 0 &&
          put("/shadow-link-target", "outside\n") &&
          file_is("/mnt/out", "outside\n"));

    /* REMOUNT: read-only and back, the volume staying where it is. */
    {
        int w = open("/mnt/hello.txt", O_WRONLY | O_APPEND);

        check("remount read-only with a file open for writing: EBUSY",
              w >= 0 && fails(do_mount("none", "/mnt", 0, MS_REMOUNT_ | 1), EBUSY));
        if (w >= 0) {
            close(w);
        }
        fd = open("/mnt/hello.txt", O_RDONLY);
        check("  but a file open for reading does not stop it",
              fd >= 0 && do_mount("none", "/mnt", 0, MS_REMOUNT_ | 1) == 0);
        check("  and then it is read-only: EROFS, and /proc/mounts says ro",
              fails(open("/mnt/new-ro.txt", O_WRONLY | O_CREAT, 0644), EROFS) &&
              has_line("/proc/mounts", "/dev/hda2 /mnt ext3 ro"));
        /* On the DISK, read past the block cache through /dev/hda2:
         * marked clean (s_state 1) and nothing for the journal to
         * replay (needs_recovery, incompat bit 4, clear) -- a machine
         * stopped now leaves a volume that needs neither. */
        check("  and on the disk it is clean, with nothing to replay",
              sb_state("/dev/hda2") == 0x1);
        check("  its files still read, through the descriptor that was open",
              fd >= 0 && read(fd, cwd, 5) == 5 && memcmp(cwd, "hello", 5) == 0);
        if (fd >= 0) {
            close(fd);
        }
        check("remount it writable again",
              do_mount("none", "/mnt", 0, MS_REMOUNT_) == 0 &&
              has_line("/proc/mounts", "/dev/hda2 /mnt ext3 rw"));
        check("  in use on the disk again, the journal open",
              sb_state("/dev/hda2") == 0x40000);
        check("  and it is: a new file, written and read back",
              put("/mnt/after-rw.txt", "back\n") &&
              file_is("/mnt/after-rw.txt", "back\n") &&
              unlink("/mnt/after-rw.txt") == 0);
        check("remount of a directory that is not a volume's root: EINVAL",
              fails(do_mount("none", "/mnt/d", 0, MS_REMOUNT_ | 1), EINVAL));
        check("remount of the root read-only, and back",
              do_mount("none", "/", 0, MS_REMOUNT_ | 1) == 0 &&
              fails(open("/root-ro.txt", O_WRONLY | O_CREAT, 0644), EROFS) &&
              do_mount("none", "/", 0, MS_REMOUNT_) == 0 &&
              put("/root-rw.txt", "rw\n") && unlink("/root-rw.txt") == 0);
    }

    /* The mount point cannot be taken away. */
    check("rmdir of the mount point: EBUSY", fails(rmdir("/mnt"), EBUSY));
    check("rename of the mount point: EBUSY",
          fails(rename("/mnt", "/mnt2"), EBUSY));

    /* In use. */
    fd = open("/mnt/hello.txt", O_RDONLY);
    check("umount with a file open on it: EBUSY",
          fd >= 0 && fails(do_umount("/mnt"), EBUSY));
    close(fd);
    chdir("/mnt/d");
    check("umount with the working directory on it: EBUSY",
          fails(do_umount("/mnt"), EBUSY));
    chdir("/");
    p = fork();
    if (p == 0) {
        chdir("/mnt");
        pause();
        _exit(0);
    }
    usleep(200000);
    check("umount while ANOTHER process is in it: EBUSY",
          fails(do_umount("/mnt"), EBUSY));
    kill(p, SIGKILL);
    waitpid(p, &r, 0);

    /* --- a second volume on the first, read-only --------------------- */
    check("mkdir /mnt/sub", mkdir("/mnt/sub", 0755) == 0);
    check("mount /dev/hda3 read-only on /mnt/sub",
          do_mount("/dev/hda3", "/mnt/sub", "ext2", MS_RDONLY) == 0);
    check("/proc/mounts lists it, ro",
          has_line("/proc/mounts", "/dev/hda3 /mnt/sub ext2 ro"));
    check("its file reads", file_is("/mnt/sub/ro.txt", "read only\n"));
    stat("/mnt/sub", &st);
    check("  a third device", st.st_dev != mnt.st_dev && st.st_dev != root.st_dev);
    statvfs("/mnt/sub", &ms);
    check("  statvfs says ST_RDONLY", (ms.f_flag & ST_RDONLY) != 0);
    check("umount of the volume UNDER it: EBUSY", fails(do_umount("/mnt"), EBUSY));
    check("read-only: create EROFS",
          fails(open("/mnt/sub/x", O_WRONLY | O_CREAT, 0644), EROFS));
    check("read-only: open an existing file for writing EROFS",
          fails(open("/mnt/sub/ro.txt", O_WRONLY), EROFS));
    check("read-only: O_TRUNC EROFS",
          fails(open("/mnt/sub/ro.txt", O_RDONLY | O_TRUNC), EROFS));
    check("read-only: unlink EROFS", fails(unlink("/mnt/sub/ro.txt"), EROFS));
    check("read-only: mkdir EROFS", fails(mkdir("/mnt/sub/dd", 0755), EROFS));
    check("read-only: chmod EROFS", fails(chmod("/mnt/sub/ro.txt", 0600), EROFS));
    check("read-only: utime EROFS", fails(utime("/mnt/sub/ro.txt", 0), EROFS));
    check("read-only: rename EROFS",
          fails(rename("/mnt/sub/ro.txt", "/mnt/sub/r2"), EROFS));
    check("read-only: symlink EROFS", fails(symlink("x", "/mnt/sub/l"), EROFS));
    check("  and it still reads", file_is("/mnt/sub/ro.txt", "read only\n"));
    check("umount /mnt/sub", do_umount("/mnt/sub") == 0);
    check("  and /mnt/sub is the empty directory again",
          access("/mnt/sub/ro.txt", F_OK) != 0 && listed("/mnt/sub", "."));

    /* --- umount -------------------------------------------------------- */
    check("umount /mnt", do_umount("/mnt") == 0);
    check("the root volume's file is back", file_is("/mnt/shadow.txt", "under the mount\n"));
    check("/proc/mounts no longer lists it", !has_line("/proc/mounts", "/dev/hda2"));
    check("a second umount: EINVAL", fails(do_umount("/mnt"), EINVAL));

    /* Mounted again, what was written is still there -- and stays
     * mounted, for the halt to unmount and the host to read. */
    check("mount it again", do_mount("hda2", "/mnt", 0, 0) == 0);
    check("  what was written is still there",
          file_is("/mnt/new.txt", "new on hda2\n") &&
          file_is("/mnt/d/rel.txt", "relative\n") &&
          file_is("/mnt/renamed.txt", "new on hda2\n"));
    check("  and a last file for the host to find",
          put("/mnt/final.txt", "written before halt\n"));

    printf("mnttest: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
