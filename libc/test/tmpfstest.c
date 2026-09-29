/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tmpfstest - /tmp and /dev/shm, in memory.
 *
 * kernel/tmpfstest.sh plants /tmp/ondisk.txt in the DISK's /tmp before
 * booting, and afterwards checks from the host that nothing written to
 * /tmp here reached the disk: the two sides of "not on the disk", each
 * seen by something that is not tmpfs.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

static int has_line(const char *path, const char *want)
{
    char line[256];
    FILE *f = fopen(path, "r");
    int found = 0;

    while (f && fgets(line, sizeof(line), f)) {
        if (strcmp(line, want) == 0) {
            found = 1;
        }
    }
    if (f) {
        fclose(f);
    }
    return found;
}

static int listed(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    while (d && (e = readdir(d))) {
        if (strcmp(e->d_name, name) == 0) {
            found = 1;
        }
    }
    if (d) {
        closedir(d);
    }
    return found;
}

static void files(void)
{
    struct stat st;
    char buf[64];
    int fd, fd2;

    check("/proc/mounts lists /tmp and /dev/shm as tmpfs",
          has_line("/proc/mounts", "tmpfs /tmp tmpfs rw 0 0\n") &&
          has_line("/proc/mounts", "tmpfs /dev/shm tmpfs rw 0 0\n"));
    check("/tmp is a directory, mode 1777, not a disk inode",
          stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode) &&
          (st.st_mode & 07777) == 01777 && st.st_ino >= 0x70000000UL);
    check("the disk's own /tmp is hidden under it",
          stat("/tmp/ondisk.txt", &st) == -1 && errno == ENOENT);

    fd = open("/tmp/a.txt", O_CREAT | O_RDWR | O_TRUNC, 0600);
    write(fd, "hello, tmpfs", 12);
    lseek(fd, 0, SEEK_SET);
    memset(buf, 0, sizeof(buf));
    check("create, write, read back",
          fd >= 0 && read(fd, buf, sizeof(buf)) == 12 &&
          strcmp(buf, "hello, tmpfs") == 0);
    check("  with the mode asked for, and owned by its creator",
          fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0600 &&
          st.st_uid == getuid() && st.st_size == 12);

    /* A hole, then the far side of it. */
    lseek(fd, 20000, SEEK_SET);
    write(fd, "Z", 1);
    memset(buf, 1, sizeof(buf));
    check("a write past the end leaves a hole that reads zero",
          pread(fd, buf, 16, 8000) == 16 && buf[0] == 0 && buf[15] == 0 &&
          fstat(fd, &st) == 0 && st.st_size == 20001);
    check("ftruncate shrinks it, and growing again reads zero",
          ftruncate(fd, 5) == 0 && ftruncate(fd, 100) == 0 &&
          pread(fd, buf, 12, 0) == 12 && memcmp(buf, "hello\0\0\0\0\0\0\0", 12) == 0);
    close(fd);

    fd = open("/tmp/a.txt", O_WRONLY | O_APPEND);
    write(fd, "!", 1);
    close(fd);
    check("O_APPEND appends", stat("/tmp/a.txt", &st) == 0 && st.st_size == 101);

    check("mkdir, and a file inside it",
          mkdir("/tmp/d", 0755) == 0 &&
          (fd = open("/tmp/d/f", O_CREAT | O_WRONLY, 0644)) >= 0 &&
          close(fd) == 0);
    check("rmdir of a directory with something in it is ENOTEMPTY",
          rmdir("/tmp/d") == -1 && errno == ENOTEMPTY);
    check("rename within tmpfs", rename("/tmp/d/f", "/tmp/d/g") == 0 &&
          stat("/tmp/d/g", &st) == 0 && stat("/tmp/d/f", &st) == -1);
    check("rename between tmpfs and the disk is EXDEV",
          rename("/tmp/d/g", "/g") == -1 && errno == EXDEV);
    check("so is a hard link across", link("/tmp/d/g", "/g") == -1 &&
          errno == EXDEV);
    check("a hard link within, two names for one inode",
          link("/tmp/d/g", "/tmp/h") == 0 && stat("/tmp/h", &st) == 0 &&
          st.st_nlink == 2);
    check("readdir lists it", listed("/tmp/d", "g") && listed("/tmp", "h") &&
          listed("/tmp", "."));

    check("a symlink within, followed and read",
          symlink("d/g", "/tmp/s") == 0 &&
          readlink("/tmp/s", buf, sizeof(buf)) == 3 &&
          open("/tmp/s", O_RDONLY) >= 0);
    check("a symlink out of tmpfs is not followed onto the disk: EXDEV",
          symlink("/etc/passwd", "/tmp/out") == 0 &&
          open("/tmp/out", O_RDONLY) == -1 && errno == EXDEV);

    /* Unlinked while open: still there for whoever has it open. */
    fd = open("/tmp/gone", O_CREAT | O_RDWR, 0644);
    write(fd, "still", 5);
    unlink("/tmp/gone");
    fd2 = open("/tmp/gone", O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check("an unlinked file is readable through a descriptor still open",
          fd2 == -1 && pread(fd, buf, 5, 0) == 5 && strcmp(buf, "still") == 0);
    close(fd);

    check("chmod", chmod("/tmp/h", 0640) == 0 && stat("/tmp/h", &st) == 0 &&
          (st.st_mode & 0777) == 0640);
}

static void working_dir(void)
{
    char cwd[256];
    int fd;

    check("chdir /tmp, and getcwd says so",
          chdir("/tmp") == 0 && getcwd(cwd, sizeof(cwd)) &&
          strcmp(cwd, "/tmp") == 0);
    fd = open("rel.txt", O_CREAT | O_WRONLY, 0644);
    check("  a relative name there is tmpfs's",
          fd >= 0 && close(fd) == 0 && access("/tmp/rel.txt", F_OK) == 0);
    check("  and . lists it", listed(".", "rel.txt"));
    check("  and .. leads out to the disk",
          chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0 &&
          access("tmp", F_OK) == 0);
    /* From deep in the disk, into tmpfs, and "..": the disk must not be
     * asked about ".." from the directory it last stood in. */
    mkdir("/deep", 0755);
    mkdir("/deep/er", 0755);
    check("  and from /deep/er, cd /tmp then .. is /, not /deep",
          chdir("/deep/er") == 0 && chdir("/tmp") == 0 && chdir("..") == 0 &&
          getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/") == 0);
    check("  and a relative name from / into tmp/ is tmpfs's",
          access("tmp/rel.txt", F_OK) == 0);
}

static void shm(void)
{
    int fd, st;
    char *m;
    pid_t pid;

    fd = shm_open("/sage-shm", O_CREAT | O_RDWR, 0600);
    check("shm_open makes a file in /dev/shm",
          fd >= 0 && access("/dev/shm/sage-shm", F_OK) == 0);
    ftruncate(fd, 8192);
    m = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("  which maps MAP_SHARED", m != MAP_FAILED);
    if (m == MAP_FAILED) {
        return;
    }
    pid = fork();
    if (pid == 0) {
        int cfd = shm_open("/sage-shm", O_RDWR, 0);
        char *cm = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);

        if (cm == MAP_FAILED) {
            _exit(2);
        }
        strcpy(cm + 4096, "from the child");
        _exit(0);
    }
    waitpid(pid, &st, 0);
    check("  and another process that opens it by name shares the memory",
          WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
          strcmp(m + 4096, "from the child") == 0);
    check("shm_unlink removes the name",
          shm_unlink("/sage-shm") == 0 && access("/dev/shm/sage-shm", F_OK) == -1);
    check("  and the mapping lives on", strcmp(m + 4096, "from the child") == 0);
    munmap(m, 8192);
    close(fd);
    check("a name with a slash in it is EINVAL",
          shm_open("/a/b", O_CREAT | O_RDWR, 0600) == -1 && errno == EINVAL);
}

/* The sticky bit: another user may make files, and may not remove ours. */
static void sticky(void)
{
    int st, fd;
    pid_t pid;

    fd = open("/tmp/roots", O_CREAT | O_WRONLY, 0666);
    close(fd);
    pid = fork();
    if (pid == 0) {
        int bad = 0;

        if (setuid(1000) != 0) {
            _exit(100);
        }
        fd = open("/tmp/mine", O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            bad |= 1;
        }
        close(fd);
        if (unlink("/tmp/roots") != -1 || errno != EPERM) {
            bad |= 2;
        }
        if (rename("/tmp/roots", "/tmp/stolen") != -1 || errno != EPERM) {
            bad |= 4;
        }
        if (unlink("/tmp/mine") != 0) {
            bad |= 8;
        }
        _exit(bad);
    }
    waitpid(pid, &st, 0);
    check("/tmp's sticky bit: another user makes and removes their own, "
          "and not ours", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        printf("         (exit %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
}

int main(void)
{
    int fd;

    files();
    working_dir();
    shm();
    sticky();
    /* For the host to look for on the disk, and not find. */
    fd = open("/tmp/never-on-disk.txt", O_CREAT | O_WRONLY, 0644);
    write(fd, "x", 1);
    close(fd);
    sync();
    printf("tmpfstest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
