/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * exec.c - the ELF loader.
 *
 * Reference: Tool Interface Standard, Executable and Linking Format,
 * version 1.2 (the 32-bit chapters).
 *
 * Programs are ELF32, big-endian, EM_68K, ET_EXEC -- the toolchain's own
 * output, so `make` produces something runnable with no objcopy step and
 * no format of our own to document. The boot ROM flattens the kernel
 * because it has to run before there is anything to parse a header with;
 * by the time a program is loaded there is a filesystem, a seek and room
 * to do it properly.
 *
 * A program is linked at a fixed address. A dynamically linked one
 * also names an INTERPRETER (PT_INTERP) -- /lib/ld.so, ldso/ld.c --
 * which is loaded beside it, at its own fixed address, and started
 * first; it finds the program's headers and entry point through the
 * auxiliary vector below the environment, loads the shared libraries
 * and relocates everything, then jumps to the program. This file does
 * no relocation at all: the interpreter is an ET_EXEC too, so it needs
 * none, and the libraries are ld.so's business, through mmap.
 *
 * Every field is read a byte at a time. ELF32 for m68k is big-endian, so
 * a direct load would work -- but it would work by accident of the
 * header sitting at a convenient offset in a buffer, and the day someone
 * reads a phdr at an odd offset it stops working for reasons that take
 * an afternoon to find.
 */
#include "exec.h"
#include "vfs.h"
#include "task.h"
#include "tty.h"
#include "vm.h"
#include "pmm.h"
#include "uaccess.h"
#include "ptregs.h"
#include "syscall.h"
#include "console.h"
#include "errno.h"
#include "string.h"

/* --- ELF32 --------------------------------------------------------- */

#define EI_NIDENT     16
#define EHDR_SIZE     52
#define PHDR_SIZE     32

#define ELFCLASS32    1
#define ELFDATA2MSB   2         /* big-endian */
#define EV_CURRENT    1
#define ET_EXEC       2
#define EM_68K        4
#define PT_LOAD       1
#define PT_INTERP     3
#define PT_PHDR       6

/*
 * Where an interpreter may live: the megabyte below the stack's guard
 * page, which ldso/ld.ld links it into. Anything else offered as an
 * interpreter is refused, so a program cannot name some other file and
 * have it placed over its own image.
 */
#define LDSO_BASE     0x1fe00000UL
#define LDSO_END      (USER_BRK_LIMIT)

/* The auxiliary vector's keys, Linux's numbers. */
#define AT_NULL       0
#define AT_PHDR       3
#define AT_PHENT      4
#define AT_PHNUM      5
#define AT_PAGESZ     6
#define AT_BASE       7
#define AT_FLAGS      8
#define AT_ENTRY      9
#define AUXV_WORDS    16

#define INTERP_MAX    64

/* What loading an image learned that the stack needs to say. */
struct image {
    u32 entry;
    u32 phdr;                   /* where its program headers are, loaded */
    u32 phnum;
    u32 end;                    /* the highest byte of any segment      */
    char interp[INTERP_MAX];    /* "" for a static program               */
};

/* Header offsets, from the specification. */
#define E_TYPE        16
#define E_MACHINE     18
#define E_VERSION     20
#define E_ENTRY       24
#define E_PHOFF       28
#define E_PHENTSIZE   42
#define E_PHNUM       44

#define P_TYPE        0
#define P_OFFSET      4
#define P_VADDR       8
#define P_FILESZ      16
#define P_MEMSZ       20

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

/* --- running a program is making a task ------------------------------ */

/*
 * There is no "the running program" any more.
 *
 * exec.c used to own a single flag, a single saved context and a single
 * program area, and every one of those was a consequence of there being
 * nothing else to run. What it does now is build an address space, load
 * an image into it, and hand the result to the scheduler as a task --
 * after which this file has no further interest in it. The shell waits
 * for it, or does not, and both are the shell's business.
 */

/* --- loading -------------------------------------------------------- */

static int read_at(int fd, u32 offset, void *buf, u32 len)
{
    s32 n;

    if (fd_lseek(fd, (s32)offset, SEEK_SET) < 0) {
        return -EIO;
    }
    n = fd_read(fd, buf, len);
    if (n < 0) {
        return (int)n;
    }
    if ((u32)n != len) {
        /* A file too short to hold the header it claims is not a program
         * this kernel can run, which is what ENOEXEC means. */
        return -ENOEXEC;
    }
    return 0;
}

/*
 * Is this an m68k executable this kernel can run?
 *
 * Checked in the order that gives the most useful complaint: the magic
 * number first, because "that is not a program" is the common case and
 * deserves a plain answer.
 */
static int check_ident(const u8 *e)
{
    if (e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        return -ENOEXEC;
    }
    if (e[4] != ELFCLASS32 || e[5] != ELFDATA2MSB) {
        return -ENOEXEC;
    }
    if (be16(&e[E_TYPE]) != ET_EXEC) {
        return -ENOEXEC;
    }
    if (be16(&e[E_MACHINE]) != EM_68K) {
        return -ENOEXEC;
    }
    if (be32(&e[E_VERSION]) != EV_CURRENT) {
        return -ENOEXEC;
    }
    return 0;
}

/*
 * Make sure every page of [va, va+len) exists in the address space.
 *
 * Checked before mapping because two segments can share a page -- the
 * end of the text and the start of the data commonly do -- and mapping
 * the same virtual page twice would hand it a second physical page,
 * orphaning the first along with everything already read into it.
 */
static int reserve(struct addrspace *as, u32 va, u32 len)
{
    u32 first = PAGE_ALIGN_DOWN(va);
    u32 last = PAGE_ALIGN_UP(va + len);
    u32 p;

    for (p = first; p < last; p += PAGE_SIZE) {
        if (vm_translate(as, p, 0)) {
            continue;
        }
        if (!vm_map(as, p, 0, VM_USER | VM_WRITE)) {
            return -ENOMEM;
        }
    }
    return 0;
}

/*
 * Read from the file straight into the program's pages.
 *
 * A page at a time, because the program's memory is contiguous only in
 * its own address space: consecutive virtual pages come from wherever
 * the allocator had one, so a single read across a page boundary would
 * land the second half in the wrong place.
 *
 * The write goes to the PHYSICAL address, which the kernel can use
 * directly because its own map is identity. This is the loader reaching
 * into a program's memory before the program exists, which is the one
 * place doing so is not a bug.
 */
static int read_into(struct addrspace *as, int fd, u32 off, u32 va, u32 len)
{
    while (len > 0) {
        u32 pa = vm_translate(as, va, 1);
        u32 n = (u32)PAGE_SIZE - (va & PAGE_MASK);
        s32 got;

        if (!pa) {
            return -EFAULT;
        }
        if (n > len) {
            n = len;
        }
        if (fd_lseek(fd, (s32)off, SEEK_SET) < 0) {
            return -EIO;
        }
        got = fd_read(fd, (void *)pa, n);
        if (got < 0) {
            return (int)got;
        }
        if ((u32)got != n) {
            return -ENOEXEC;
        }
        va += n;
        off += n;
        len -= n;
    }
    return 0;
}

/*
 * Load every PT_LOAD segment into `as`, and say where things are. `lo`
 * and `hi` bound where the segments may go: the whole user area for a
 * program, the interpreter's megabyte for an interpreter.
 */
static int load_image(struct addrspace *as, int fd, struct image *img,
                      u32 lo, u32 hi)
{
    u8 ehdr[EHDR_SIZE];
    u8 phdr[PHDR_SIZE];
    u32 phoff;
    int phnum, phentsize, i, err;
    int loaded = 0;
    u32 image_end = 0;
    u32 *entry = &img->entry;

    memset(img, 0, sizeof(*img));

    err = read_at(fd, 0, ehdr, sizeof(ehdr));
    if (err < 0) {
        return err;
    }
    err = check_ident(ehdr);
    if (err < 0) {
        return err;
    }

    phoff = be32(&ehdr[E_PHOFF]);
    phnum = (int)be16(&ehdr[E_PHNUM]);
    phentsize = (int)be16(&ehdr[E_PHENTSIZE]);
    *entry = be32(&ehdr[E_ENTRY]);

    if (phoff == 0 || phnum <= 0 || phentsize < PHDR_SIZE) {
        return -ENOEXEC;
    }
    if (*entry < lo || *entry >= hi) {
        return -ENOEXEC;
    }
    img->phnum = (u32)phnum;

    for (i = 0; i < phnum; i++) {
        u32 type, off, vaddr, filesz, memsz;

        err = read_at(fd, phoff + (u32)i * (u32)phentsize,
                      phdr, PHDR_SIZE);
        if (err < 0) {
            return err;
        }

        type = be32(&phdr[P_TYPE]);
        off = be32(&phdr[P_OFFSET]);
        vaddr = be32(&phdr[P_VADDR]);
        filesz = be32(&phdr[P_FILESZ]);
        memsz = be32(&phdr[P_MEMSZ]);

        if (type == PT_INTERP) {
            if (filesz < 2 || filesz > INTERP_MAX) {
                return -ENOEXEC;
            }
            err = read_at(fd, off, img->interp, filesz);
            if (err < 0) {
                return err;
            }
            if (img->interp[filesz - 1] != '\0') {
                return -ENOEXEC;        /* not a string */
            }
            continue;
        }
        if (type == PT_PHDR) {
            img->phdr = vaddr;
            continue;
        }
        if (type != PT_LOAD) {
            continue;
        }
        /* Headers inside the first bytes of a loaded segment are loaded
         * with it; that is where they are when there is no PT_PHDR. */
        if (!img->phdr && off <= phoff && phoff < off + filesz) {
            img->phdr = vaddr + (phoff - off);
        }

        if (memsz < filesz) {
            return -ENOEXEC;
        }
        /*
         * The program says where it wants to live and the kernel decides
         * whether to believe it. The MMU would now catch a segment that
         * overlapped the kernel -- there is no kernel in this address
         * space to overlap -- but a segment outside the user area simply
         * has no page tables behind it, so refusing here gives a clear
         * answer instead of a fault during loading.
         */
        if (vaddr < lo || vaddr + memsz > hi || vaddr + memsz < vaddr) {
            return -ENOEXEC;
        }
        /* Leave room for the stack at the top. */
        if (vaddr + memsz > USER_VA_STACK_TOP -
                            (u32)USER_STACK_PAGES * PAGE_SIZE) {
            return -ENOMEM;
        }

        err = reserve(as, vaddr, memsz);
        if (err < 0) {
            return err;
        }
        if (filesz > 0) {
            err = read_into(as, fd, off, vaddr, filesz);
            if (err < 0) {
                return err;
            }
        }
        /*
         * Anything the file does not supply is .bss, and is already
         * zero: every page came from pmm_alloc, which zeroes. Doing it
         * again here would be writing zeroes over zeroes -- but it is
         * worth saying out loud, because the guarantee lives in the
         * allocator and this is where it is relied upon.
         */
        if (vaddr + memsz > image_end) {
            image_end = vaddr + memsz;
        }
        loaded++;
    }
    if (loaded == 0) {
        return -ENOEXEC;
    }
    img->end = image_end;
    return 0;
}

/*
 * Put argc and argv where the program will look for them.
 *
 * The strings themselves have to be copied INTO the address space --
 * they are the shell's, in the kernel, and the program cannot see
 * kernel memory any more. So the strings go at the top of the user
 * stack, then an array of pointers to them, then the three words crt0.s
 * reads.
 *
 * That last bit deserves a word. crt0 expects argc at 4(sp) and argv at
 * 8(sp), because it used to be entered with `jsr` and a return address
 * sat below them. There is no jsr any more -- a program is entered by
 * returning from a fabricated exception frame -- so a return address is
 * written by hand to keep the layout identical. It points at zero, which
 * is unmapped in every address space, so a program that manages to
 * return from _start faults instead of wandering.
 */
static int setup_stack(int argc, char **argv, char **envp, const u32 *auxv,
                       u32 auxv_words, u32 *out_sp)
{
    /* Static: 2 KB of them, too much for a kernel stack, and nothing in
     * here can sleep, so no second exec can be in the middle of it. */
    static u32 uargv[EXEC_MAX_ARGS + 1];
    static u32 uenv[EXEC_MAX_ENV + 1];
    u32 sp = USER_VA_STACK_TOP & ~3UL;
    u32 envc = 0;
    int i, err;

    /*
     * The environment goes in first, above the arguments, because the
     * conventional Unix layout puts envp above argv on the stack and
     * because nothing here depends on the order -- so it may as well be
     * the order everybody expects to find.
     */
    if (envp) {
        while (envc < EXEC_MAX_ENV && envp[envc]) {
            envc++;
        }
        for (i = (int)envc - 1; i >= 0; i--) {
            u32 len = (u32)strlen(envp[i]) + 1;

            sp -= len;
            err = copy_to_user(sp, envp[i], len);
            if (err < 0) {
                return err;
            }
            uenv[i] = sp;
        }
    }
    uenv[envc] = 0;

    /*
     * The auxiliary vector, immediately after envp's terminating NULL --
     * which is the only way anything finds it: Linux's layout, and what
     * ld.so walks. Key and value pairs, ending in AT_NULL. Every program
     * gets one; a static program has no use for it, and no harm from it.
     */
    sp &= ~3UL;
    sp -= auxv_words * 4;
    err = copy_to_user(sp, auxv, auxv_words * 4);
    if (err < 0) {
        return err;
    }

    sp -= (envc + 1) * 4;
    err = copy_to_user(sp, uenv, (envc + 1) * 4);
    if (err < 0) {
        return err;
    }
    {
        u32 uenvp = sp;

        for (i = argc - 1; i >= 0; i--) {
        u32 len = (u32)strlen(argv[i]) + 1;

        sp -= len;
        err = copy_to_user(sp, argv[i], len);
        if (err < 0) {
            return err;
        }
        uargv[i] = sp;
    }
    uargv[argc] = 0;                    /* argv is NULL terminated */

    sp &= ~3UL;
    sp -= (u32)(argc + 1) * 4;
    err = copy_to_user(sp, uargv, (u32)(argc + 1) * 4);
    if (err < 0) {
        return err;
    }

    {
        u32 frame[4];

        frame[0] = 0;                   /* the return address crt0 ignores */
        frame[1] = (u32)argc;
        frame[2] = sp;                  /* argv */
        frame[3] = uenvp;               /* envp */
        sp -= 16;
        err = copy_to_user(sp, frame, sizeof(frame));
        if (err < 0) {
            return err;
        }
    }
    }

    *out_sp = sp;
    return 0;
}

/* The command line, for the job table to show and for `fg` to name. */
static void describe(char *out, u32 max, int argc, char **argv)
{
    u32 n = 0;
    int i;

    for (i = 0; i < argc && n + 1 < max; i++) {
        const char *p = argv[i];

        if (i > 0) {
            out[n++] = ' ';
        }
        while (*p && n + 1 < max) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
}

/*
 * Build the address space a program will run in: its image, its stack,
 * and its arguments. Everything it will be able to reach.
 */
static int build(struct addrspace *as, const char *path,
                 int argc, char **argv, char **envp, u32 *entry, u32 *sp)
{
    static struct image prog, interp;   /* nothing here sleeps */
    u32 auxv[AUXV_WORDS], n = 0;
    u32 va;
    int fd, err;

    fd = fd_open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    err = load_image(as, fd, &prog, USER_VA_BASE, USER_VA_END);
    fd_close(fd);
    if (err < 0) {
        return err;
    }

    /*
     * The heap starts on the page after the program's highest segment,
     * which is where Linux puts it. Page aligned, so the last page of
     * .bss -- already mapped by reserve() -- belongs to the image and not
     * to the heap, and shrinking the heap can never unmap part of the
     * program. Set from the PROGRAM, not the interpreter, which lives
     * far above it.
     */
    as->brk_start = PAGE_ALIGN_UP(prog.end);
    as->brk_cur = as->brk_start;
    *entry = prog.entry;

    /*
     * A dynamically linked program: load its interpreter too, and start
     * THERE. The interpreter must be a plain program -- one that names
     * an interpreter of its own is refused, not followed.
     */
    interp.entry = 0;
    if (prog.interp[0]) {
        fd = fd_open(prog.interp, O_RDONLY);
        if (fd < 0) {
            return fd == -ENOENT ? -ELIBACC : fd;
        }
        err = load_image(as, fd, &interp, LDSO_BASE, LDSO_END);
        fd_close(fd);
        if (err < 0) {
            return err == -ENOEXEC ? -ELIBBAD : err;
        }
        if (interp.interp[0]) {
            return -ELIBBAD;
        }
        *entry = interp.entry;
    }

    auxv[n++] = AT_PHDR;   auxv[n++] = prog.phdr;
    auxv[n++] = AT_PHENT;  auxv[n++] = PHDR_SIZE;
    auxv[n++] = AT_PHNUM;  auxv[n++] = prog.phnum;
    auxv[n++] = AT_PAGESZ; auxv[n++] = PAGE_SIZE;
    auxv[n++] = AT_BASE;   auxv[n++] = prog.interp[0] ? LDSO_BASE : 0;
    auxv[n++] = AT_FLAGS;  auxv[n++] = 0;
    auxv[n++] = AT_ENTRY;  auxv[n++] = prog.entry;
    auxv[n++] = AT_NULL;   auxv[n++] = 0;

    /*
     * The stack, at the top of the user area, and nothing below it.
     *
     * The gap between the image and the stack is not merely unused, it
     * is UNMAPPED -- so a runaway stack runs off the bottom of its last
     * page and takes an access fault, instead of quietly eating the
     * program's own data the way it would have done when everything
     * shared one flat space. That is the first thing here that the MMU
     * buys which a comment could not.
     */
    /*
     * LAZILY, a megabyte of it: the pages come as the stack reaches them
     * (vm_fault). It was mapped whole, which made every program -- the
     * smallest included -- cost a megabyte before it ran an instruction.
     */
    for (va = USER_VA_END - (u32)USER_STACK_PAGES * PAGE_SIZE;
         va < USER_VA_END; va += PAGE_SIZE) {
        if (vm_map_lazy(as, va, VM_USER | VM_WRITE) < 0) {
            return -ENOMEM;
        }
    }

    return setup_stack(argc, argv, envp, auxv, n, sp);
}

/*
 * Load a program and start it as a task.
 *
 * Returns the new task's pid. IT DOES NOT WAIT: waiting is task_wait(),
 * and whether to do it is what separates a foreground job from a
 * background one. That separation is the whole of what `&` needed.
 */
int exec_spawn(const char *path, int argc, char **argv, char **envp)
{
    char cmd[JOB_CMD_MAX];
    struct addrspace *as;
    struct task *t;
    u32 entry = 0, sp = 0;
    int err;

    if (argc < 0 || argc > EXEC_MAX_ARGS) {
        return -E2BIG;
    }

    as = vm_create();
    if (!as) {
        return -ENOMEM;
    }

    /*
     * Pointed at the new address space before it is filled, because
     * filling it means copying the arguments in -- and copy_to_user is
     * how anything gets into a program's memory, including the arguments
     * it was started with.
     *
     * The RAW setting is saved and put back -- null, normally -- not
     * uaccess_current(). A program reaches its own memory through
     * current->as without any override; restoring its address space as
     * an override made every other task reach into it too.
     */
    {
        struct addrspace *saved = uaccess_set(as);

        err = build(as, path, argc, argv, envp, &entry, &sp);
        uaccess_set(saved);
    }

    if (err < 0) {
        vm_destroy(as);
        return err;
    }

    vm_ready(as);
    describe(cmd, sizeof(cmd), argc, argv);

    t = task_create_user(argv[0] ? argv[0] : path, entry, sp, as);
    if (!t) {
        vm_destroy(as);
        return -EAGAIN;
    }
    strncpy(t->cmd, cmd, JOB_CMD_MAX - 1);
    t->cmd[JOB_CMD_MAX - 1] = '\0';
    t->parent = current;

    /*
     * The child gets its parent's descriptors, pointing at the same open
     * files. That is what makes `prog > file` work: the shell opens the
     * file, points descriptor 1 at it, spawns, and the program writes
     * there without knowing anything happened.
     */
    fd_inherit(t, current);

    /*
     * And its working directory and umask, as a forked child does. This
     * was missing from the start: every program the shell ran began in
     * the root whatever `cd` had said, and nothing noticed, because
     * every test ran its programs from the root or named files in full.
     * awk was the first program to open a relative name after a `cd`.
     */
    task_cwd_inherit(t, current);
    task_cred_inherit(t, current);

    /*
     * The new task joins its spawner's process group, as a forked one
     * does. A shell moves each job into a group of its own; a program
     * that starts a helper keeps it in its own group, so the ctrl-C
     * that ends the program ends the helper too.
     *
     * The terminal is NOT handed over here any more. It was, so that a
     * ctrl-C typed while the image loaded would not be lost -- but that
     * gave every program that spawned a helper's terminal to the helper,
     * and deciding who has the terminal is the shell's business. The
     * price is that a ctrl-C typed during the load, a few milliseconds,
     * goes to the shell's group and nowhere.
     */
    t->pgid = current->pgid;
    t->sid = current->sid;
    t->nice = current->nice;

    return t->pid;
}

/* --- replacing a program: execve ------------------------------------- */

extern void fpu_restore(const u32 *area);

int exec_replace(const char *path, int argc, char **argv, char **envp,
                 struct pt_regs *regs)
{
    struct addrspace *as, *old = current->as;
    u32 entry = 0, sp = 0;
    int err, sig;

    if (argc < 0 || argc > EXEC_MAX_ARGS) {
        return -E2BIG;
    }
    if (!old) {
        return -EPERM;          /* a kernel task is not a program */
    }

    /*
     * EXEC ENDS EVERY OTHER THREAD, which POSIX requires and which this
     * has to do BEFORE anything is replaced: the image about to go is
     * the one they are running in. After this the process is one thread
     * again, and the address space below has a single holder.
     */
    task_group_kill(current);

    as = vm_create();
    if (!as) {
        return -ENOMEM;
    }
    {
        struct addrspace *saved = uaccess_set(as);

        err = build(as, path, argc, argv, envp, &entry, &sp);
        uaccess_set(saved);
    }
    if (err < 0) {
        vm_destroy(as);
        return err;             /* the caller carries on, untouched */
    }

    /* The point of no return: the old image goes. */
    current->as = as;
    vm_switch(as);
    vm_destroy(old);
    vm_ready(as);

    describe(current->cmd, JOB_CMD_MAX, argc, argv);
    strncpy(current->name, argv[0] ? argv[0] : path, TASK_NAME_MAX - 1);
    current->name[TASK_NAME_MAX - 1] = '\0';

    /*
     * What POSIX says survives an exec: the pid, the parent, the group,
     * the descriptors that are not close-on-exec, the working directory,
     * the signal mask, pending signals, ignored signals, the timers. What
     * does not: caught signals, which have nothing to return to, and go
     * back to their default.
     */
    fd_exec(current);
    for (sig = 1; sig < NSIG; sig++) {
        if (current->sigact[sig].sa_handler != SIG_IGN) {
            memset(&current->sigact[sig], 0, sizeof(current->sigact[sig]));
        }
    }
    current->sig_restore_mask = 0;
    current->ss_sp = current->ss_size = 0;  /* the old image's memory */

    /* A fresh FPU, as a new program's is. */
    {
        static u32 fresh[52];

        memset(fresh, 0, sizeof(fresh));
        fresh[0] = 0x41000000UL;        /* idle frame; see taskasm.s */
        fpu_restore(fresh);
    }

    /* And into it: every register clear, as a new program's are. */
    memset(regs->d, 0, sizeof(regs->d));
    memset(regs->a, 0, sizeof(regs->a));
    regs->pc = entry;
    regs->sr = 0;
    regs->format = 0;
    __asm__ volatile ("move.l %0,%%usp" : : "a"(sp));
    current->syscall_nr = -1;           /* never "restart" an exec */
    return 0;
}
