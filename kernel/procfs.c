/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * procfs.c - /proc.
 *
 * Linux's /proc, the part that ported software actually reads: what a
 * process is running, with what arguments, what it has open, where its
 * memory is, and how the machine is doing. There is nothing on the disk
 * behind any of it. Every file is written out of the kernel's own
 * tables at the moment it is opened, the way /dev is made out of the
 * device registry, and read-only: nothing here can be changed by
 * writing to it.
 *
 *   /proc/cpuinfo /proc/loadavg /proc/meminfo /proc/mounts /proc/stat
 *   /proc/uptime  /proc/self -> <pid>
 *   /proc/<pid>/  cmdline comm environ maps stat statm status
 *                 cwd -> exe -> root -> fd/ (N -> what descriptor N is)
 *
 * The formats are Linux's, field for field, because the only reason to
 * have /proc at all is that programs written for Linux parse it. Where
 * this kernel does not keep a figure Linux reports, the field is still
 * there -- a parser counts fields -- and holds 0; each one is said
 * below.
 *
 * A SNAPSHOT AT OPEN. The whole file is generated when it is opened
 * and read from memory after that, so a program that reads it in small
 * pieces sees one consistent answer rather than a line of one moment
 * and a line of the next. Linux's seq_file regenerates as it goes;
 * reading from the start again means opening again, here as there.
 *
 * WHO MAY SEE WHAT. Everything is readable by everybody except what
 * names another user's files or memory -- a process's links, its fd
 * directory and its environment -- which only its own user and root may
 * follow or read. That is the rule Linux applies (ptrace_may_access,
 * simplified to "same real user"), and it is what stops /proc/<pid>/cwd
 * being a way into a directory the caller could not otherwise reach.
 *
 * STANDING IN /proc. A working directory there is kept as its path
 * alone -- the filesystem's own number for the working directory is
 * left as it was, and never consulted while the path is under /proc,
 * because every relative name is then /proc's (proc_owns). sbase's ls,
 * like fts and so find and du, changes into each directory it lists,
 * so this is not a nicety.
 *
 * NOT HERE: /proc/<pid>/task, /proc/sys and the rest of Linux's
 * hundred-odd files; this is the part that software was found to need.
 *
 * /dev's DIRECTORIES ARE HERE TOO. The devices themselves are names in
 * the device registry (dev.c), opened through resolve_dev() in vfs.c --
 * but a registry cannot be listed, so `ls /dev` showed nothing. So /dev
 * and /dev/pts are directories of this tree, listing the registry, and
 * /dev/fd and /dev/stdin, stdout and stderr are links into
 * /proc/self/fd, exactly as Linux makes them. Everything else under
 * /dev -- the devices, /dev/pts/N, and /dev/shm (tmpfs) -- is not
 * procfs's, and proc_lookup() hands it back as "elsewhere".
 */
#include "procfs.h"
#include "vfs.h"
#include "dev.h"
#include "task.h"
#include "vm.h"
#include "pmm.h"
#include "uaccess.h"
#include "timer.h"
#include "loadavg.h"
#include "swap.h"
#include "textcache.h"
#include "errno.h"
#include "string.h"

/* ---------------------------------------------------------------- */
/* The names                                                         */
/* ---------------------------------------------------------------- */

enum {
    N_ROOT, N_FILE, N_LINK, N_PID, N_FD, N_DEVROOT, N_DEVPTS
};

enum {
    /* /proc */
    F_CPUINFO, F_LOADAVG, F_MEMINFO, F_MOUNTS, F_STAT, F_UPTIME, L_SELF,
    /* /proc/<pid> */
    P_CMDLINE, P_COMM, P_ENVIRON, P_MAPS, P_STAT, P_STATM, P_STATUS,
    L_CWD, L_EXE, L_ROOT, L_FD,
    /* /dev */
    L_DEVFD, L_STDIN, L_STDOUT, L_STDERR
};

struct entry {
    const char *name;
    u8 type;                    /* N_FILE, N_LINK, or N_FD for "fd"   */
    u8 what;
    u16 mode;
};

static const struct entry root_ents[] = {
    { "cpuinfo", N_FILE, F_CPUINFO, 0444 },
    { "loadavg", N_FILE, F_LOADAVG, 0444 },
    { "meminfo", N_FILE, F_MEMINFO, 0444 },
    { "mounts",  N_FILE, F_MOUNTS,  0444 },
    { "self",    N_LINK, L_SELF,    0777 },
    { "stat",    N_FILE, F_STAT,    0444 },
    { "uptime",  N_FILE, F_UPTIME,  0444 },
};
#define ROOT_ENTS   (sizeof(root_ents) / sizeof(root_ents[0]))

static const struct entry pid_ents[] = {
    { "cmdline", N_FILE, P_CMDLINE, 0444 },
    { "comm",    N_FILE, P_COMM,    0444 },
    { "cwd",     N_LINK, L_CWD,     0777 },
    { "environ", N_FILE, P_ENVIRON, 0400 },
    { "exe",     N_LINK, L_EXE,     0777 },
    { "fd",      N_FD,   0,         0500 },
    { "maps",    N_FILE, P_MAPS,    0444 },
    { "root",    N_LINK, L_ROOT,    0777 },
    { "stat",    N_FILE, P_STAT,    0444 },
    { "statm",   N_FILE, P_STATM,   0444 },
    { "status",  N_FILE, P_STATUS,  0444 },
};
#define PID_ENTS    (sizeof(pid_ents) / sizeof(pid_ents[0]))

static const struct entry dev_ents[] = {
    { "fd",      N_LINK, L_DEVFD,   0777 },
    { "stderr",  N_LINK, L_STDERR,  0777 },
    { "stdin",   N_LINK, L_STDIN,   0777 },
    { "stdout",  N_LINK, L_STDOUT,  0777 },
};
#define DEV_ENTS    (sizeof(dev_ents) / sizeof(dev_ents[0]))

/* What /proc/<pid>/fd/<n> is. */
static const struct entry fd_link = { "", N_LINK, L_FD, 0700 };

/* One place in /proc: what it is, and whose. */
struct pnode {
    int type;                   /* N_*                                  */
    const struct entry *e;      /* for N_FILE and N_LINK                */
    int pid;                    /* for everything under /proc/<pid>     */
    int fd;                     /* for /proc/<pid>/fd/<fd>              */
};

/* Is `path` exactly `name`, but for trailing slashes? */
static int is_exactly(const char *path, const char *name)
{
    u32 n = (u32)strlen(name);

    if (strncmp(path, name, n) != 0) {
        return 0;
    }
    for (path += n; *path == '/'; path++) {
    }
    return *path == '\0';
}

/* The part of /dev that is this tree's: see the head of the file. */
static int under_dev(const char *path)
{
    if (strncmp(path, "/dev", 4) != 0 || (path[4] && path[4] != '/')) {
        return 0;
    }
    return is_exactly(path, "/dev") || is_exactly(path, "/dev/pts") ||
           is_exactly(path, "/dev/stdin") || is_exactly(path, "/dev/stdout") ||
           is_exactly(path, "/dev/stderr") ||
           (strncmp(path, "/dev/fd", 7) == 0 &&
            (path[7] == '\0' || path[7] == '/'));
}

static int under_proc(const char *path)
{
    return (strncmp(path, "/proc", 5) == 0 &&
            (path[5] == '\0' || path[5] == '/')) || under_dev(path);
}

/*
 * A path is /proc's if it is absolute and under /proc -- or relative,
 * from a working directory that is. There is no directory on the
 * volume a relative name could mean then; the name is resolved against
 * the working directory's path, here, and the filesystem never sees it.
 */
int proc_owns(const char *path)
{
    if (path[0] == '/') {
        return under_proc(path);
    }
    return current && under_proc(current->cwd_path);
}

int proc_cwd(void)
{
    return current && under_proc(current->cwd_path);
}

/* A decimal number and nothing else, as a pid or a descriptor is
 * named; -1 for anything that is not. */
static int number(const char *s, u32 len)
{
    u32 i, v = 0;

    if (len == 0 || len > 9 || (s[0] == '0' && len > 1)) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (u32)(s[i] - '0');
    }
    return (int)v;
}

static const struct entry *find(const struct entry *tab, u32 n,
                                const char *name, u32 len)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (strlen(tab[i].name) == len &&
            strncmp(tab[i].name, name, len) == 0) {
            return &tab[i];
        }
    }
    return 0;
}

static struct file *task_file(struct task *t, int fd)
{
    if (!t || !t->files || fd < 0 || fd >= OPEN_MAX) {
        return 0;
    }
    return t->files->fd[fd];
}

/*
 * May the caller see into this task -- follow its links, list its
 * descriptors, read its environment? Its own user and root may.
 */
static int may_inspect(struct task *t)
{
    return current && (current->euid == 0 || current->euid == t->uid ||
                       current->uid == t->uid);
}

/*
 * Walk `path` from /proc as far as the first symbolic link that has to
 * be followed -- one with more path after it, or the last component
 * when `follow` is set -- or to the end. *used says how much of the
 * path that took; the node says where it got. Returns 1 when it stopped
 * at a link to follow, 0 at the end, or -errno.
 */
static int walk(const char *path, int follow, struct pnode *n, u32 *used)
{
    u32 i = 5;                  /* past "/proc" */

    memset(n, 0, sizeof(*n));
    n->type = N_ROOT;
    n->pid = -1;
    n->fd = -1;
    if (path[1] == 'd') {
        i = 4;                  /* past "/dev" */
        n->type = N_DEVROOT;
    }

    for (;;) {
        const char *c;
        u32 len, next;
        int num;

        while (path[i] == '/') {
            i++;
        }
        if (!path[i]) {
            *used = i;
            return 0;
        }
        c = path + i;
        len = 0;
        while (c[len] && c[len] != '/') {
            len++;
        }
        next = i + len;

        if (len == 1 && c[0] == '.') {
            i = next;
            continue;
        }
        switch (n->type) {
        case N_ROOT:
            num = number(c, len);
            if (num > 0) {
                if (!task_find(num)) {
                    return -ENOENT;
                }
                n->type = N_PID;
                n->pid = num;
                break;
            }
            n->e = find(root_ents, ROOT_ENTS, c, len);
            if (!n->e) {
                return -ENOENT;
            }
            n->type = n->e->type;
            break;
        case N_DEVROOT:
            if (len == 3 && strncmp(c, "pts", 3) == 0) {
                n->type = N_DEVPTS;
                break;
            }
            n->e = find(dev_ents, DEV_ENTS, c, len);
            if (!n->e) {
                return -ENOENT;
            }
            n->type = n->e->type;
            break;
        case N_PID:
            n->e = find(pid_ents, PID_ENTS, c, len);
            if (!n->e) {
                return -ENOENT;
            }
            n->type = n->e->type;
            break;
        case N_FD:
            /* Into another user's descriptors: the directory is 0500
             * and theirs, and this is that check made for root too --
             * Linux makes it with ptrace's rules, which root passes. */
            if (!may_inspect(task_find(n->pid))) {
                return -EACCES;
            }
            num = number(c, len);
            if (num < 0 || !task_file(task_find(n->pid), num)) {
                return -ENOENT;
            }
            n->type = N_LINK;
            n->e = &fd_link;
            n->fd = num;
            break;
        default:
            return -ENOTDIR;            /* a file has nothing under it */
        }
        if (n->type == N_LINK) {
            int last;
            const char *rest = path + next;

            while (*rest == '/') {
                rest++;
            }
            last = (*rest == '\0');
            if (!last || follow) {
                *used = next;
                return 1;
            }
            *used = next;
            return 0;
        }
        i = next;
    }
}

/* The target of a link node: a path, or an open file with none. */
static int link_target(const struct pnode *n, char *out, u32 size,
                       struct file **anon)
{
    struct task *t;

    *anon = 0;
    switch (n->e->what) {
    case L_DEVFD:
        strcpy(out, "/proc/self/fd");
        return 0;
    case L_STDIN:
    case L_STDOUT:
    case L_STDERR:
        strcpy(out, "/proc/self/fd/0");
        out[14] = (char)('0' + n->e->what - L_STDIN);
        return 0;
    }
    if (n->e->what == L_SELF) {
        struct task *me = current;
        char tmp[12];
        int k = 0, v = me ? me->tgid : 0, j = 0;

        if (!me) {
            return -ENOENT;
        }
        do {
            tmp[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (k) {
            out[j++] = tmp[--k];
        }
        out[j] = '\0';
        return 0;
    }
    t = task_find(n->pid);
    if (!t) {
        return -ENOENT;
    }
    if (!may_inspect(t)) {
        return -EACCES;
    }
    if (n->fd >= 0) {
        struct file *f = task_file(t, n->fd);
        int r;

        if (!f) {
            return -ENOENT;
        }
        r = vfs_file_name(f, out, size);
        if (r < 0) {
            return r;
        }
        if (out[0] != '/') {
            file_get(f);
            *anon = f;
        }
        return 0;
    }
    switch (n->e->what) {
    case L_CWD:
        if (strlen(t->cwd_path) >= size) {
            return -ENAMETOOLONG;
        }
        strcpy(out, t->cwd_path);
        return 0;
    case L_ROOT:
        strcpy(out, "/");
        return 0;
    case L_EXE:
        if (!t->as || !t->as->img.exe[0]) {
            return -ENOENT;     /* a kernel task runs no file */
        }
        if (strlen(t->as->img.exe) >= size) {
            return -ENAMETOOLONG;
        }
        strcpy(out, t->as->img.exe);
        return 0;
    }
    return -EINVAL;
}

/* A path for the node, the way /proc names it: self as a number. */
static void canonical(const char *path, u32 used, char *out)
{
    memcpy(out, path, used);
    out[used] = '\0';
}

int proc_lookup(const char *path, int follow, char *out, struct file **anon)
{
    char cur[PATH_MAX], tgt[PATH_MAX];
    u32 len = (u32)strlen(path);
    int hops;

    *anon = 0;
    /*
     * Absolute, and with "." and ".." taken out, before anything else:
     * a relative name is from a working directory in /proc, and ".." is
     * taken lexically -- /proc/self/cwd/.. is /proc/self -- where Linux
     * would go to the parent of what cwd leads to. A trailing slash
     * means the last name is a directory, so a link there is followed
     * (POSIX), whatever the caller asked.
     */
    if (len && path[len - 1] == '/') {
        follow = 1;
    }
    if (vfs_abspath(path, cur, sizeof(cur)) < 0) {
        return -ENAMETOOLONG;
    }
    if (!under_proc(cur)) {
        strcpy(out, cur);       /* "/proc/.." and the like */
        return PROC_ELSEWHERE;
    }

    for (hops = 0; hops < 8; hops++) {
        struct pnode n;
        struct file *f;
        u32 used, tl;
        int r = walk(cur, follow, &n, &used);

        if (r < 0) {
            return r;
        }
        if (r == 0) {
            canonical(cur, used, out);
            return PROC_HERE;
        }
        r = link_target(&n, tgt, sizeof(tgt), &f);
        if (r < 0) {
            return r;
        }
        if (f) {
            while (cur[used] == '/') {
                used++;
            }
            if (cur[used]) {
                file_put(f);
                return -ENOTDIR;
            }
            *anon = f;
            return PROC_ANON;
        }
        /* self is relative -- "12" -- and means /proc/12. */
        if (tgt[0] != '/') {
            char num[16];

            strcpy(num, tgt);
            strcpy(tgt, "/proc/");
            strcpy(tgt + 6, num);
        }
        tl = (u32)strlen(tgt);
        if (tl + strlen(cur + used) >= PATH_MAX) {
            return -ENAMETOOLONG;
        }
        strcpy(tgt + tl, cur + used);
        if (vfs_abspath(tgt, cur, sizeof(cur)) < 0) {
            return -ENAMETOOLONG;
        }
        if (!under_proc(cur)) {
            strcpy(out, cur);
            return PROC_ELSEWHERE;
        }
    }
    return -ELOOP;
}

/* ---------------------------------------------------------------- */
/* stat, readlink                                                    */
/* ---------------------------------------------------------------- */

static int resolve(const char *path, struct pnode *n)
{
    u32 used;
    int r = walk(path, 0, n, &used);

    return r < 0 ? r : 0;
}

static u32 now_seconds(void)
{
    struct timeval tv;

    clock_get(&tv);
    return (u32)tv.tv_sec;
}

int proc_stat(const char *path, struct stat *st)
{
    struct pnode n;
    struct task *t;
    int r = resolve(path, &n);

    if (r < 0) {
        return r;
    }
    memset(st, 0, sizeof(*st));
    st->st_mtime = now_seconds();
    st->st_nlink = 1;
    t = n.pid >= 0 ? task_find(n.pid) : 0;
    if (t) {
        st->st_uid = t->euid;
        st->st_gid = t->egid;
    }
    /* A number no file on the volume has, different for every name. */
    st->st_ino = 0x80000000UL | ((u32)(n.pid & 0x3ffff) << 12) |
                 ((u32)(n.e ? n.e->what + 1 : 32 + n.type) << 6) |
                 (u32)(n.fd >= 0 ? n.fd : 0);

    switch (n.type) {
    case N_ROOT:
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_ino = 1 | 0x80000000UL;
        break;
    case N_PID:
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        break;
    case N_DEVROOT:
    case N_DEVPTS:
        /* Searchable and readable by everybody, writable by nobody:
         * what is in them is the registry. */
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        break;
    case N_FD:
        st->st_mode = S_IFDIR | 0500;
        st->st_nlink = 2;
        break;
    case N_FILE:
        st->st_mode = S_IFREG | n.e->mode;
        break;
    case N_LINK:
        st->st_mode = S_IFLNK | 0777;
        if (n.fd >= 0) {
            /* As Linux has it: the owner's bits say how the descriptor
             * was opened, read and write, x with either. */
            struct file *f = task_file(t, n.fd);
            int acc = f ? (f->flags & O_ACCMODE) : O_RDONLY;

            st->st_mode = S_IFLNK;
            if (acc == O_RDONLY || acc == O_RDWR) {
                st->st_mode |= 0500;
            }
            if (acc == O_WRONLY || acc == O_RDWR) {
                st->st_mode |= 0300;
            }
        }
        break;
    }
    return 0;
}

int proc_readlink(const char *path, char *out, u32 size)
{
    struct pnode n;
    struct file *anon;
    int r = resolve(path, &n);

    if (r < 0) {
        return r;
    }
    if (n.type != N_LINK) {
        return -EINVAL;
    }
    r = link_target(&n, out, size, &anon);
    if (anon) {
        file_put(anon);         /* the name was all that was wanted */
    }
    return r;
}

/* ---------------------------------------------------------------- */
/* Writing a file out                                                */
/* ---------------------------------------------------------------- */

/*
 * Up to 64 KB, a page at a time, from the page allocator: enough for
 * the longest command line exec will build (EXEC_MAX_ARGS strings) and
 * any maps this address space can hold. Past that the file is cut off,
 * which is better than refusing to open it.
 */
#define PBUF_PAGES  16

struct pbuf {
    u32 page[PBUF_PAGES];
    u32 len;
    int err;
};

static void putc_(struct pbuf *b, char c)
{
    u32 p = b->len / PAGE_SIZE;

    if (b->err || p >= PBUF_PAGES) {
        return;
    }
    if (!b->page[p]) {
        b->page[p] = pmm_alloc();
        if (!b->page[p]) {
            b->err = -ENOMEM;
            return;
        }
    }
    ((char *)b->page[p])[b->len % PAGE_SIZE] = c;
    b->len++;
}

static void puts_(struct pbuf *b, const char *s)
{
    while (*s) {
        putc_(b, *s++);
    }
}

/* Unsigned decimal, right-aligned in `width` (0 for none). */
static void putu(struct pbuf *b, u32 v, int width)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (width-- > n) {
        putc_(b, ' ');
    }
    while (n) {
        putc_(b, tmp[--n]);
    }
}

static void puti(struct pbuf *b, s32 v)
{
    if (v < 0) {
        putc_(b, '-');
        putu(b, (u32)-v, 0);
    } else {
        putu(b, (u32)v, 0);
    }
}

/* Lower-case hex, zero-padded to `digits`. */
static void putx(struct pbuf *b, u32 v, int digits)
{
    int i;

    for (i = digits - 1; i >= 0; i--) {
        putc_(b, "0123456789abcdef"[(v >> (i * 4)) & 15]);
    }
}

/* Hundredths as "12.34". */
static void putcenti(struct pbuf *b, u32 centi)
{
    putu(b, centi / 100, 0);
    putc_(b, '.');
    putc_(b, (char)('0' + (centi / 10) % 10));
    putc_(b, (char)('0' + centi % 10));
}

/* Ticks as hundredths of a second: USER_HZ is 100, and so is HZ, but
 * the arithmetic says so rather than assuming it. */
static u32 ticks_to_centi(u32 ticks)
{
    return (u32)((ticks / HZ) * 100 + (ticks % HZ) * 100 / HZ);
}

/* A field, then a space: /proc/<pid>/stat is fifty-two of these. */
static void field(struct pbuf *b, u32 v)
{
    putu(b, v, 0);
    putc_(b, ' ');
}

/* The name a task goes by: the last part of what it was started as,
 * cut to fifteen characters, which is Linux's comm. */
static void comm(struct pbuf *b, struct task *t)
{
    const char *s = t->name, *p;
    int n;

    for (p = s; *p; p++) {
        if (*p == '/' && p[1]) {
            s = p + 1;
        }
    }
    for (n = 0; s[n] && n < 15; n++) {
        putc_(b, s[n]);
    }
}

static char state_letter(struct task *t)
{
    switch (t->state) {
    case TASK_RUNNING:
    case TASK_READY:   return 'R';
    case TASK_STOPPED: return 'T';
    case TASK_ZOMBIE:  return 'Z';
    default:           return 'S';
    }
}

/* --- the machine ---------------------------------------------------- */

/*
 * Linux/m68k's own layout (arch/m68k/kernel/setup_mm.c). "Clocking",
 * "BogoMips" and "Calibration" are left out rather than invented: the
 * clock speed is not something this kernel has measured.
 */
static void gen_cpuinfo(struct pbuf *b)
{
    puts_(b, "CPU:\t\t68040\n");
    puts_(b, "MMU:\t\t68040\n");
    puts_(b, "FPU:\t\t68040\n");
    puts_(b, "Model:\t\tSage040\n");
}

static void gen_loadavg(struct pbuf *b)
{
    u32 l[3];
    int i;

    loadavg_get(l);
    for (i = 0; i < 3; i++) {
        /* SI_LOAD_SHIFT fixed point, to hundredths, rounded. */
        putcenti(b, (u32)((l[i] * 100 + (1UL << (SI_LOAD_SHIFT - 1)))
                          >> SI_LOAD_SHIFT));
        putc_(b, ' ');
    }
    putu(b, (u32)task_nr_active(), 0);
    putc_(b, '/');
    putu(b, (u32)task_count(), 0);
    putc_(b, ' ');
    putu(b, (u32)task_last_pid(), 0);
    putc_(b, '\n');
}

/* "Name:" padded to sixteen, the figure right-aligned in eight, then
 * " kB": the exact shape of Linux's lines, which free(1) splits on. */
static void kb_line(struct pbuf *b, const char *name, u32 pages)
{
    int n = (int)strlen(name);

    puts_(b, name);
    while (n++ < 16) {
        putc_(b, ' ');
    }
    putu(b, pages * (PAGE_SIZE / 1024), 8);
    puts_(b, " kB\n");
}

static void gen_meminfo(struct pbuf *b)
{
    struct tc_stats tc;
    struct swapstats sw;
    u32 freep = pmm_available();

    textcache_stats(&tc);
    swap_stats(&sw);
    kb_line(b, "MemTotal:", pmm_total());
    kb_line(b, "MemFree:", freep);
    /* What could be had without swapping: the free pages, and the
     * cached file pages nobody has mapped, which are given up first. */
    kb_line(b, "MemAvailable:", freep + textcache_idle());
    kb_line(b, "Buffers:", 0);
    kb_line(b, "Cached:", tc.cached);
    kb_line(b, "SwapCached:", 0);
    kb_line(b, "SwapTotal:", sw.slots);
    kb_line(b, "SwapFree:", sw.slots - sw.used);
}

static void gen_mounts(struct pbuf *b)
{
    if (vfs_mounted()) {
        puts_(b, "/dev/");
        puts_(b, vfs_dev_name());
        puts_(b, " / ");
        puts_(b, vfs_fs_name());
        puts_(b, " rw 0 0\n");
    }
    puts_(b, "devtmpfs /dev devtmpfs rw 0 0\n");
    puts_(b, "proc /proc proc rw 0 0\n");
    puts_(b, "tmpfs /tmp tmpfs rw 0 0\n");
    puts_(b, "tmpfs /dev/shm tmpfs rw 0 0\n");
}

/*
 * The first lines of Linux's /proc/stat. There is one CPU, so "cpu"
 * and "cpu0" agree; nice, iowait and the interrupt columns are zero
 * because nothing here counts them apart. The interrupt and context
 * switch totals ("intr", "ctxt") are left out: nothing counts them.
 */
static void gen_stat(struct pbuf *b)
{
    u32 ticks[3], up = timer_jiffies();
    int cpu;

    task_cpu_ticks(ticks);
    for (cpu = 0; cpu < 2; cpu++) {
        puts_(b, cpu ? "cpu0 " : "cpu  ");
        field(b, ticks_to_centi(ticks[0]));     /* user    */
        field(b, 0);                            /* nice    */
        field(b, ticks_to_centi(ticks[1]));     /* system  */
        field(b, ticks_to_centi(ticks[2]));     /* idle    */
        puts_(b, "0 0 0 0 0 0\n");
    }
    puts_(b, "btime ");
    putu(b, now_seconds() - up / HZ, 0);
    puts_(b, "\nprocesses ");
    putu(b, (u32)task_last_pid(), 0);
    puts_(b, "\nprocs_running ");
    putu(b, (u32)task_nr_active(), 0);
    putc_(b, '\n');
}

static void gen_uptime(struct pbuf *b)
{
    u32 ticks[3];

    task_cpu_ticks(ticks);
    putcenti(b, ticks_to_centi(timer_jiffies()));
    putc_(b, ' ');
    putcenti(b, ticks_to_centi(ticks[2]));
    putc_(b, '\n');
}

/* --- a process ------------------------------------------------------ */

/*
 * Copy [start, end) of another task's memory: its command line or its
 * environment, which are where exec put them at the top of its stack.
 * Through uaccess, pointed at THAT address space, so a page that has
 * gone to swap is brought back rather than read as nothing. The space
 * is held for the length of the copy, which may sleep, so that the
 * task exiting meanwhile cannot free it under us.
 */
static void copy_mem(struct pbuf *b, struct addrspace *as, u32 start,
                     u32 end)
{
    struct addrspace *prev;
    char chunk[128];

    if (!as || start >= end) {
        return;
    }
    vm_share(as);
    prev = uaccess_set(as);
    while (start < end && !b->err) {
        u32 n = end - start, i;

        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (copy_from_user(chunk, start, n) < 0) {
            break;
        }
        for (i = 0; i < n; i++) {
            putc_(b, chunk[i]);
        }
        start += n;
    }
    uaccess_set(prev);
    vm_destroy(as);
}

/* How much of an address space is owned, and how much writable, in
 * pages: vsize and statm's data. */
struct sizes {
    u32 owned, writable;
};

static int count_region(void *arg, u32 start, u32 end, int prot, int shared)
{
    struct sizes *s = arg;
    u32 pages = (end - start) / PAGE_SIZE;

    (void)shared;
    s->owned += pages;
    if (prot & VM_WRITE) {
        s->writable += pages;
    }
    return 0;
}

static void sizes_of(struct addrspace *as, struct sizes *s)
{
    s->owned = s->writable = 0;
    if (as) {
        vm_regions(as, count_region, s);
    }
}

/* Linux's four signal masks, as bits: pending, blocked, ignored,
 * caught. */
static void sig_masks(struct task *t, u32 m[4])
{
    int i;

    m[0] = t->sig_pending;
    m[1] = t->sig_blocked;
    m[2] = m[3] = 0;
    for (i = 1; i < NSIG; i++) {
        sighandler_t h = t->sigact[i].sa_handler;

        if (h == SIG_IGN) {
            m[2] |= 1UL << (i - 1);
        } else if (h != SIG_DFL) {
            m[3] |= 1UL << (i - 1);
        }
    }
}

/*
 * Linux's fifty-two fields, in Linux's order (fs/proc/array.c,
 * do_task_stat). Zeroes, each for want of something to count: tty_nr
 * (nothing records a controlling terminal per process yet), the fault
 * counts, the scheduling and accounting fields, wchan, and the stack and
 * instruction pointers, which Linux itself zeroes for other processes.
 */
static void gen_pid_stat(struct pbuf *b, struct task *t)
{
    struct addrspace *as = t->as;
    struct as_image none, *im;
    struct sizes sz;
    u32 m[4];

    memset(&none, 0, sizeof(none));
    im = as ? &as->img : &none;
    sizes_of(as, &sz);
    sig_masks(t, m);

    putu(b, (u32)t->pid, 0);
    puts_(b, " (");
    comm(b, t);
    puts_(b, ") ");
    putc_(b, state_letter(t));
    putc_(b, ' ');
    field(b, t->parent ? (u32)t->parent->tgid : 0);     /* 4 ppid       */
    field(b, (u32)t->pgid);
    field(b, (u32)t->sid);
    field(b, 0);                                        /* 7 tty_nr     */
    puts_(b, "-1 ");                                    /* 8 tpgid      */
    field(b, as ? 0 : 0x00200000UL);                    /* 9 PF_KTHREAD */
    puts_(b, "0 0 0 0 ");                               /* 10-13 faults */
    field(b, ticks_to_centi(t->utime));                 /* 14           */
    field(b, ticks_to_centi(t->stime));
    field(b, ticks_to_centi(t->cutime));
    field(b, ticks_to_centi(t->cstime));
    puti(b, 20 + t->nice);                              /* 18 priority  */
    putc_(b, ' ');
    puti(b, t->nice);
    putc_(b, ' ');
    field(b, (u32)task_group_count(t));                 /* 20 threads   */
    field(b, 0);                                        /* 21 itrealvalue */
    field(b, ticks_to_centi(t->start));                 /* 22 starttime */
    field(b, sz.owned * PAGE_SIZE);                     /* 23 vsize     */
    field(b, as ? vm_mapped_pages(as) : 0);             /* 24 rss       */
    field(b, 0xffffffffUL);                             /* 25 rsslim    */
    field(b, im->start_code);
    field(b, im->end_code);
    field(b, im->start_stack);                          /* 28           */
    puts_(b, "0 0 ");                                   /* 29-30 sp, pc */
    field(b, m[0]);                                     /* 31 signal    */
    field(b, m[1]);
    field(b, m[2]);
    field(b, m[3]);                                     /* 34 sigcatch  */
    puts_(b, "0 0 0 ");                                 /* 35-37        */
    field(b, SIGCHLD);                                  /* 38 exit_signal */
    puts_(b, "0 0 0 0 0 0 ");                           /* 39-44        */
    field(b, im->start_data);                           /* 45           */
    field(b, im->end_data);
    field(b, as ? as->brk_start : 0);                   /* 47 start_brk */
    field(b, im->arg_start);
    field(b, im->arg_end);
    field(b, im->env_start);
    field(b, im->env_end);                              /* 51           */
    putu(b, t->state == TASK_ZOMBIE ? (u32)t->exit_status : 0, 0);
    putc_(b, '\n');
}

static void gen_statm(struct pbuf *b, struct task *t)
{
    struct sizes sz;
    u32 text = 0;

    sizes_of(t->as, &sz);
    if (t->as) {
        text = (t->as->img.end_code - t->as->img.start_code + PAGE_SIZE - 1)
               / PAGE_SIZE;
    }
    field(b, sz.owned);                                 /* size         */
    field(b, t->as ? vm_mapped_pages(t->as) : 0);       /* resident     */
    field(b, 0);                                        /* shared       */
    field(b, text);                                     /* text         */
    field(b, 0);                                        /* lib          */
    field(b, sz.writable);                              /* data + stack */
    puts_(b, "0\n");                                    /* dt           */
}

static void kv(struct pbuf *b, const char *key)
{
    puts_(b, key);
    putc_(b, '\t');
}

static void four(struct pbuf *b, u32 r, u32 e, u32 s)
{
    putu(b, r, 0);
    putc_(b, '\t');
    putu(b, e, 0);
    putc_(b, '\t');
    putu(b, s, 0);
    putc_(b, '\t');
    putu(b, e, 0);              /* the filesystem id is the effective one */
    putc_(b, '\n');
}

static void gen_status(struct pbuf *b, struct task *t)
{
    static const char *const names[] = {
        "R (running)", "S (sleeping)", "T (stopped)", "Z (zombie)"
    };
    struct sizes sz;
    u32 m[4];
    char s = state_letter(t);
    int i;

    sizes_of(t->as, &sz);
    sig_masks(t, m);
    kv(b, "Name:");
    comm(b, t);
    putc_(b, '\n');
    kv(b, "Umask:");
    putx(b, t->umask, 4);
    putc_(b, '\n');
    kv(b, "State:");
    puts_(b, names[s == 'R' ? 0 : s == 'S' ? 1 : s == 'T' ? 2 : 3]);
    putc_(b, '\n');
    kv(b, "Tgid:");
    putu(b, (u32)t->tgid, 0);
    putc_(b, '\n');
    kv(b, "Pid:");
    putu(b, (u32)t->pid, 0);
    putc_(b, '\n');
    kv(b, "PPid:");
    putu(b, t->parent ? (u32)t->parent->tgid : 0, 0);
    putc_(b, '\n');
    kv(b, "TracerPid:");
    puts_(b, "0\n");
    kv(b, "Uid:");
    four(b, t->uid, t->euid, t->suid);
    kv(b, "Gid:");
    four(b, t->gid, t->egid, t->sgid);
    kv(b, "FDSize:");
    putu(b, OPEN_MAX, 0);
    putc_(b, '\n');
    kv(b, "Groups:");
    for (i = 0; i < t->ngroups; i++) {
        putu(b, t->groups[i], 0);
        putc_(b, ' ');
    }
    putc_(b, '\n');
    if (t->as) {
        puts_(b, "VmSize:\t");
        putu(b, sz.owned * (PAGE_SIZE / 1024), 8);
        puts_(b, " kB\nVmRSS:\t");
        putu(b, vm_mapped_pages(t->as) * (PAGE_SIZE / 1024), 8);
        puts_(b, " kB\n");
    }
    kv(b, "Threads:");
    putu(b, (u32)task_group_count(t), 0);
    putc_(b, '\n');
    /* Sixty-four bits in Linux; this kernel's thirty-two are the low
     * half, and the same signal numbers. */
    kv(b, "SigPnd:");
    putx(b, 0, 8);
    putx(b, m[0], 8);
    putc_(b, '\n');
    kv(b, "SigBlk:");
    putx(b, 0, 8);
    putx(b, m[1], 8);
    putc_(b, '\n');
    kv(b, "SigIgn:");
    putx(b, 0, 8);
    putx(b, m[2], 8);
    putc_(b, '\n');
    kv(b, "SigCgt:");
    putx(b, 0, 8);
    putx(b, m[3], 8);
    putc_(b, '\n');
}

/*
 * /proc/<pid>/maps.
 *
 * There is no table of mappings to print (mmap.c says why): the regions
 * are read off the page tables, as runs of pages the program may use in
 * the same way, and NAMED from what exec recorded -- the program, its
 * interpreter, the heap and the stack. Anything else is anonymous here,
 * a shared library's text included, because nothing remembers which
 * file a page of the text cache came from; the offset column is 0 for
 * the same reason.
 *
 * "r" and "w" are what the program may do, read off the tables. The
 * 68040 has no execute bit -- anything readable can be run -- so "x"
 * cannot come from there, and shown on everything or on nothing it
 * would say nothing. It is taken from the ELF file instead: the
 * program's and the interpreter's executable segments (PF_X) are x,
 * and so is any other page the program may read but not write, which
 * is where a shared library's text is. That is where a debugger looks
 * for code, and what the file itself declared.
 */
struct maps_ctx {
    struct pbuf *b;
    struct addrspace *as;
    u32 exe_ino, interp_ino;
    u32 img_lo;
};

static int in(u32 a, u32 lo, u32 hi)
{
    return lo < hi && a >= (lo & ~(u32)(PAGE_SIZE - 1)) && a < hi;
}

static int executable(struct maps_ctx *c, u32 start, int prot)
{
    struct as_image *im = &c->as->img;

    if (prot & VM_NONE) {
        return 0;
    }
    if (in(start, im->start_code, im->end_code) ||
        in(start, im->interp_code_lo, im->interp_code_hi)) {
        return 1;
    }
    if ((c->img_lo && in(start, c->img_lo, c->as->brk_start)) ||
        in(start, im->interp_start, im->interp_end)) {
        return 0;               /* the rest of a file exec loaded: data */
    }
    return !(prot & VM_WRITE);
}

static void maps_line(struct maps_ctx *c, u32 start, u32 end, int prot,
                      int shared)
{
    struct pbuf *b = c->b;
    struct as_image *im = &c->as->img;
    const char *name = "";
    u32 ino = 0;
    int col;

    putx(b, start, 8);
    putc_(b, '-');
    putx(b, end, 8);
    putc_(b, ' ');
    putc_(b, (prot & VM_NONE) ? '-' : 'r');
    putc_(b, (prot & VM_WRITE) ? 'w' : '-');
    putc_(b, executable(c, start, prot) ? 'x' : '-');
    putc_(b, shared ? 's' : 'p');
    puts_(b, " 00000000 ");

    if (c->img_lo && start >= c->img_lo && end <= c->as->brk_start) {
        name = im->exe;
        ino = c->exe_ino;
    } else if (im->interp_end && start >= im->interp_start &&
               end <= im->interp_end) {
        name = im->interp;
        ino = c->interp_ino;
    } else if (start >= c->as->brk_start && end <= c->as->brk_cur) {
        name = "[heap]";
    } else if (start >= USER_VA_END - (u32)USER_STACK_PAGES * PAGE_SIZE) {
        name = "[stack]";
    }
    puts_(b, ino ? "03:01 " : "00:00 ");
    putu(b, ino, 0);
    col = 25 + 1 + 9 + 6 + 1;   /* where Linux pads the name out to */
    if (*name) {
        u32 n = ino, digits = 1;

        while (n >= 10) {
            n /= 10;
            digits++;
        }
        col += (int)digits;
        while (col++ < 73) {
            putc_(b, ' ');
        }
        puts_(b, name);
    }
    putc_(b, '\n');
}

static int maps_region(void *arg, u32 start, u32 end, int prot, int shared)
{
    struct maps_ctx *c = arg;
    struct as_image *im = &c->as->img;
    /* Where one name ends and the next begins: a run that crosses one
     * is two lines, as it would be two mappings on Linux. */
    u32 cuts[10];
    int i;

    cuts[0] = c->img_lo;
    cuts[1] = c->as->brk_start;
    cuts[2] = c->as->brk_cur;
    cuts[3] = im->interp_start;
    cuts[4] = im->interp_end;
    cuts[5] = USER_VA_END - (u32)USER_STACK_PAGES * PAGE_SIZE;
    /* And where code meets data: a linker may leave them adjacent with
     * the same protection, and they are still two lines. */
    cuts[6] = im->end_code;
    cuts[7] = im->start_data;
    cuts[8] = im->interp_code_hi;
    cuts[9] = im->interp_code_lo & ~(u32)(PAGE_SIZE - 1);

    while (start < end && !c->b->err) {
        u32 stop = end;

        for (i = 0; i < 10; i++) {
            u32 k = PAGE_ALIGN_UP(cuts[i]);

            if (k > start && k < stop) {
                stop = k;
            }
        }
        maps_line(c, start, stop, prot, shared);
        start = stop;
    }
    return c->b->err != 0;
}

static u32 ino_of(const char *path)
{
    struct stat st;

    return (path[0] && vfs_stat(path, &st) == 0) ? st.st_ino : 0;
}

static void gen_maps(struct pbuf *b, struct task *t)
{
    struct maps_ctx c;
    struct as_image *im;

    if (!t->as || t->state == TASK_ZOMBIE) {
        return;                 /* no user memory, or none any more */
    }
    /* Held: ino_of() may sleep in the filesystem, and the task may
     * finish meanwhile. Nothing of `t` is read after this. */
    c.as = vm_share(t->as);
    im = &c.as->img;
    c.b = b;
    c.img_lo = im->start_code;
    if (im->start_data && (!c.img_lo || im->start_data < c.img_lo)) {
        c.img_lo = im->start_data;
    }
    c.img_lo = c.img_lo & ~(u32)(PAGE_SIZE - 1);
    c.exe_ino = ino_of(im->exe);
    c.interp_ino = ino_of(im->interp);
    vm_regions(c.as, maps_region, &c);
    vm_destroy(c.as);
}

/* ---------------------------------------------------------------- */
/* Open files                                                        */
/* ---------------------------------------------------------------- */

#define PROC_OPEN_MAX   64
#define PROC_NAME_MAX   48      /* "/proc/262143/fd/63" and room over */

static struct proc_file {
    int used;
    int dir;
    char path[PROC_NAME_MAX];
    struct pbuf b;
} pfiles[PROC_OPEN_MAX];

static void pbuf_free(struct pbuf *b)
{
    int i;

    for (i = 0; i < PBUF_PAGES; i++) {
        if (b->page[i]) {
            pmm_free(b->page[i]);
            b->page[i] = 0;
        }
    }
    b->len = 0;
}

static s32 pf_read(struct file *f, void *buf, u32 len)
{
    struct proc_file *pf = f->priv;
    u32 done = 0;

    while (done < len && f->pos < pf->b.len) {
        u32 p = f->pos / PAGE_SIZE, off = f->pos % PAGE_SIZE;
        u32 n = PAGE_SIZE - off;

        if (n > pf->b.len - f->pos) {
            n = pf->b.len - f->pos;
        }
        if (n > len - done) {
            n = len - done;
        }
        memcpy((u8 *)buf + done, (u8 *)pf->b.page[p] + off, n);
        done += n;
        f->pos += n;
    }
    return (s32)done;
}

static s32 pf_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EBADF;              /* never opened for writing */
}

static s32 pf_lseek(struct file *f, s32 offset, int whence)
{
    struct proc_file *pf = f->priv;
    s32 base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (s32)f->pos :
               whence == SEEK_END ? (s32)pf->b.len : -1;

    if (base < 0 || base + offset < 0) {
        return -EINVAL;
    }
    if (pf->dir && (whence != SEEK_SET)) {
        return -EINVAL;         /* a directory rewinds, nothing more */
    }
    f->pos = (u32)(base + offset);
    return (s32)f->pos;
}

static int pf_close(struct file *f)
{
    struct proc_file *pf = f->priv;

    pbuf_free(&pf->b);
    pf->used = 0;
    return 0;
}

static int pf_fstat(struct file *f, struct stat *st)
{
    struct proc_file *pf = f->priv;
    int r = proc_stat(pf->path, st);

    if (r == -ENOENT) {
        /* The process it describes has gone; the snapshot has not. */
        memset(st, 0, sizeof(*st));
        st->st_mode = pf->dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
        st->st_nlink = 1;
        r = 0;
    }
    return r;
}

static s32 pd_read(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EISDIR;
}

static const struct file_ops proc_file_ops = {
    pf_read,
    pf_write,
    pf_lseek,
    0,
    pf_close,
    pf_fstat,
    0,
    0,                          /* truncate: read-only */
    0,                          /* mmap: copied, like any file */
};

static const struct file_ops proc_dir_ops = {
    pd_read,
    pf_write,
    pf_lseek,
    0,
    pf_close,
    pf_fstat,
    0,
    0,
    0,
};

int proc_is_dir_file(struct file *f)
{
    return f && f->ops == &proc_dir_ops;
}

int proc_dir_path(struct file *f, char *out, u32 size)
{
    struct proc_file *pf = f->priv;

    if (strlen(pf->path) >= size) {
        return -ENAMETOOLONG;
    }
    strcpy(out, pf->path);
    return 0;
}

static void generate(const struct pnode *n, struct pbuf *b)
{
    struct task *t = n->pid >= 0 ? task_find(n->pid) : 0;

    switch (n->e->what) {
    case F_CPUINFO: gen_cpuinfo(b); return;
    case F_LOADAVG: gen_loadavg(b); return;
    case F_MEMINFO: gen_meminfo(b); return;
    case F_MOUNTS:  gen_mounts(b);  return;
    case F_STAT:    gen_stat(b);    return;
    case F_UPTIME:  gen_uptime(b);  return;
    }
    if (!t) {
        return;
    }
    switch (n->e->what) {
    case P_COMM:
        comm(b, t);
        putc_(b, '\n');
        return;
    case P_STAT:   gen_pid_stat(b, t); return;
    case P_STATM:  gen_statm(b, t);    return;
    case P_STATUS: gen_status(b, t);   return;
    case P_MAPS:   gen_maps(b, t);     return;
    /* LAST, and in this order: copying may sleep, and after it the
     * task may be gone. Nothing of it is read after the copy starts. */
    case P_CMDLINE:
        if (t->as && t->state != TASK_ZOMBIE) {
            copy_mem(b, t->as, t->as->img.arg_start, t->as->img.arg_end);
        }
        return;
    case P_ENVIRON:
        if (t->as && t->state != TASK_ZOMBIE) {
            copy_mem(b, t->as, t->as->img.env_start, t->as->img.env_end);
        }
        return;
    }
}

int proc_open(const char *path, int flags)
{
    struct pnode n;
    struct proc_file *pf = 0;
    struct task *t;
    int i, fd, dir, r = resolve(path, &n);

    if (r < 0) {
        return r;
    }
    if (n.type == N_LINK) {
        return -ELOOP;          /* O_NOFOLLOW on a link, as Linux says */
    }
    dir = (n.type != N_FILE);
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) {
        return dir ? -EISDIR : -EACCES;
    }
    if (!dir && (flags & O_DIRECTORY)) {
        return -ENOTDIR;
    }
    t = n.pid >= 0 ? task_find(n.pid) : 0;
    if (t && !may_inspect(t) &&
        (n.type == N_FD || (n.type == N_FILE && n.e->what == P_ENVIRON))) {
        return -EACCES;
    }
    if (strlen(path) >= PROC_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    for (i = 0; i < PROC_OPEN_MAX; i++) {
        if (!pfiles[i].used) {
            pf = &pfiles[i];
            break;
        }
    }
    if (!pf) {
        return -ENFILE;
    }
    memset(pf, 0, sizeof(*pf));
    pf->used = 1;
    pf->dir = dir;
    strcpy(pf->path, path);

    if (!dir) {
        generate(&n, &pf->b);
        if (pf->b.err) {
            r = pf->b.err;
            pbuf_free(&pf->b);
            pf->used = 0;
            return r;
        }
    }
    fd = fd_install(dir ? &proc_dir_ops : &proc_file_ops, pf,
                    flags & ~O_ACCMODE);
    if (fd < 0) {
        pbuf_free(&pf->b);
        pf->used = 0;
        return fd;
    }
    vfs_file_set_path(fd_get(fd), path);
    return fd;
}

/* ---------------------------------------------------------------- */
/* Listing a directory                                               */
/* ---------------------------------------------------------------- */

/*
 * Positions: 0 and 1 are "." and "..", then the directory's fixed
 * entries, then -- in /proc -- processes, and in fd/ descriptors, by
 * NUMBER rather than by count, so that a process ending between two
 * getdents calls does not make the listing skip another. Linux does
 * the same, for the same reason.
 */
#define POS_NUMBERED   0x10000UL

/*
 * Entry k (from 0) of /dev after its links -- "pts", "shm", then every
 * device whose name has no slash -- or of /dev/pts: every "pts/N".
 * The registry changes as ptys come and go, so a position is a count,
 * and a listing that races a new pty may show it or not.
 */
static int dev_entry(int pts, u32 k, char *name, u8 *type, u32 *next,
                     u32 pos)
{
    struct chardev *d;

    *next = pos + 1;
    if (!pts && k < 2) {
        strcpy(name, k ? "shm" : "pts");
        *type = DT_DIR;
        return 0;
    }
    if (!pts) {
        k -= 2;
    }
    for (d = dev_first_char(); d; d = d->next) {
        const char *nm = d->name;

        if (pts) {
            if (strncmp(nm, "pts/", 4) != 0) {
                continue;
            }
            nm += 4;
        } else {
            const char *s;

            for (s = nm; *s && *s != '/'; s++) {
            }
            if (*s) {
                continue;       /* under a directory of its own */
            }
        }
        if (k-- == 0) {
            if (strlen(nm) >= 32) {
                return -ENOENT;
            }
            strcpy(name, nm);
            *type = DT_CHR;
            return 0;
        }
    }
    return -ENOENT;
}

static int dir_entry(const struct pnode *n, u32 pos, char *name, u8 *type,
                     u32 *next)
{
    const struct entry *tab = n->type == N_ROOT ? root_ents :
                              n->type == N_DEVROOT ? dev_ents : pid_ents;
    u32 fixed = n->type == N_ROOT ? ROOT_ENTS :
                n->type == N_PID ? PID_ENTS :
                n->type == N_DEVROOT ? DEV_ENTS : 0;

    if (pos < 2) {
        strcpy(name, pos ? ".." : ".");
        *type = DT_DIR;
        *next = pos + 1;
        return 0;
    }
    if (pos < 2 + fixed) {
        const struct entry *e = &tab[pos - 2];

        strcpy(name, e->name);
        *type = e->type == N_FILE ? DT_REG : e->type == N_LINK ? DT_LNK
                                                                : DT_DIR;
        *next = pos + 1;
        return 0;
    }
    if (n->type == N_PID) {
        return -ENOENT;
    }
    if (n->type == N_DEVROOT || n->type == N_DEVPTS) {
        return dev_entry(n->type == N_DEVPTS, pos - 2 - fixed, name, type,
                         next, pos);
    }
    if (pos < POS_NUMBERED) {
        pos = POS_NUMBERED;
    }
    if (n->type == N_ROOT) {
        /* The lowest thread-group leader at or above pos: one entry per
         * process, as Linux lists them. */
        int best = -1, i;

        for (i = 0; ; i++) {
            struct task *t = task_nth(i);

            if (!t) {
                break;
            }
            if (t->pid == t->tgid && (u32)t->pid >= pos - POS_NUMBERED &&
                (best < 0 || t->pid < best)) {
                best = t->pid;
            }
        }
        if (best < 0) {
            return -ENOENT;
        }
        {
            char tmp[12];
            int k = 0, j = 0, v = best;

            do {
                tmp[k++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (k) {
                name[j++] = tmp[--k];
            }
            name[j] = '\0';
        }
        *type = DT_DIR;
        *next = POS_NUMBERED + (u32)best + 1;
        return 0;
    }
    /* fd/ */
    {
        struct task *t = task_find(n->pid);
        u32 fd;

        for (fd = pos - POS_NUMBERED; fd < OPEN_MAX; fd++) {
            if (task_file(t, (int)fd)) {
                char tmp[12];
                int k = 0, j = 0;
                u32 v = fd;

                do {
                    tmp[k++] = (char)('0' + v % 10);
                    v /= 10;
                } while (v);
                while (k) {
                    name[j++] = tmp[--k];
                }
                name[j] = '\0';
                *type = DT_LNK;
                *next = POS_NUMBERED + fd + 1;
                return 0;
            }
        }
        return -ENOENT;
    }
}

s32 proc_getdents64(struct file *f, u8 *buf, u32 len)
{
    struct proc_file *pf = f->priv;
    struct pnode n;
    u32 used = 0;
    int r = resolve(pf->path, &n);

    if (r < 0) {
        return 0;               /* the process has gone: nothing in it */
    }
    for (;;) {
        char name[32];
        u8 type;
        u32 next, nl, reclen;
        struct linux_dirent64 *d;

        if (dir_entry(&n, f->pos, name, &type, &next) < 0) {
            break;
        }
        nl = (u32)strlen(name);
        reclen = (19 + nl + 1 + 7) & ~7UL;
        if (used + reclen > len) {
            if (used == 0) {
                return -EINVAL;
            }
            break;
        }
        d = (struct linux_dirent64 *)(buf + used);
        memset(d, 0, reclen);
        d->d_ino = 0x80000000UL | f->pos;
        d->d_off = next;
        d->d_reclen = (u16)reclen;
        d->d_type = type;
        memcpy(d->d_name, name, nl + 1);
        used += reclen;
        f->pos = next;
    }
    return (s32)used;
}
