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
 * Deliberately not supported: dynamic linking, relocation, shared
 * objects, interpreters. A program is statically linked at a fixed
 * address, which is the only thing that makes sense without an MMU
 * turned on.
 *
 * Every field is read a byte at a time. ELF32 for m68k is big-endian, so
 * a direct load would work -- but it would work by accident of the
 * header sitting at a convenient offset in a buffer, and the day someone
 * reads a phdr at an odd offset it stops working for reasons that take
 * an afternoon to find.
 */
#include "exec.h"
#include "vfs.h"
#include "job.h"
#include "tty.h"
#include "vm.h"
#include "pmm.h"
#include "uaccess.h"
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

/* --- the one running program --------------------------------------- */

extern int exec_enter(u32 entry, u32 usp, u32 kstack_top);
extern void exec_longjmp(int status) __attribute__((noreturn));
extern void exec_abort(int status) __attribute__((noreturn));

static int running;

/* The address space of the program that is running, so that killing it
 * from an interrupt has something to take apart afterwards. */
static struct addrspace *running_as;

int exec_running(void)
{
    return running;
}

int exec_exit(int status)
{
    if (!running) {
        /*
         * The shell has nowhere to exit to. Saying so is better than
         * halting the machine, which is what a program calling exit()
         * with no parent would otherwise amount to.
         */
        return -ENOSYS;
    }
    running = 0;
    exec_longjmp(status);
    return 0;                   /* not reached */
}

/*
 * Killed rather than exited: ctrl-C.
 *
 * Goes out through exec_abort rather than exec_longjmp because the
 * caller may be the timer interrupt handler, where the interrupt mask
 * has to be put back by hand -- nothing is going to execute an RTE and
 * restore it. The two are otherwise the same unwind.
 */
void exec_kill(int status)
{
    running = 0;
    /*
     * The address space is NOT torn down here. This can be called from
     * the timer interrupt, and vm_destroy walks page tables and frees
     * pages -- work that has no business happening inside an interrupt
     * handler, on a stack that is about to be discarded. exec_spawn
     * cleans up when it gets control back, a few instructions later,
     * standing on its own stack.
     */
    exec_abort(status);
}

struct addrspace *exec_addrspace(void)
{
    return running_as;
}

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

/* Load every PT_LOAD segment into `as` and return the entry point. */
static int load_image(struct addrspace *as, int fd, u32 *entry)
{
    u8 ehdr[EHDR_SIZE];
    u8 phdr[PHDR_SIZE];
    u32 phoff;
    int phnum, phentsize, i, err;
    int loaded = 0;

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
    if (*entry < USER_VA_BASE || *entry >= USER_VA_END) {
        return -ENOEXEC;
    }

    for (i = 0; i < phnum; i++) {
        u32 type, off, vaddr, filesz, memsz;

        err = read_at(fd, phoff + (u32)i * (u32)phentsize,
                      phdr, PHDR_SIZE);
        if (err < 0) {
            return err;
        }

        type = be32(&phdr[P_TYPE]);
        if (type != PT_LOAD) {
            continue;
        }

        off = be32(&phdr[P_OFFSET]);
        vaddr = be32(&phdr[P_VADDR]);
        filesz = be32(&phdr[P_FILESZ]);
        memsz = be32(&phdr[P_MEMSZ]);

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
        if (vaddr < USER_VA_BASE || vaddr + memsz > USER_VA_END ||
            vaddr + memsz < vaddr) {
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
        loaded++;
    }

    return loaded > 0 ? 0 : -ENOEXEC;
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
static int setup_stack(int argc, char **argv, u32 *out_sp)
{
    u32 uargv[EXEC_MAX_ARGS + 1];
    u32 sp = USER_VA_STACK_TOP & ~3UL;
    int i, err;

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
        u32 frame[3];

        frame[0] = 0;                   /* the return address crt0 ignores */
        frame[1] = (u32)argc;
        frame[2] = sp;                  /* argv */
        sp -= 12;
        err = copy_to_user(sp, frame, sizeof(frame));
        if (err < 0) {
            return err;
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
 * Is a stopped job holding something a new program would need?
 *
 * It used to be the program area: one image at a fixed address, so a
 * second program loaded straight over a stopped one. That is gone --
 * every program has its own address space now and two of them cannot
 * collide. What is left is narrower: execasm.s holds ONE saved kernel
 * context, so one program can be part-way through at a time, and
 * starting a second while one is stopped would leave the first with no
 * way home.
 *
 * The restriction survives, in other words, but for a different reason,
 * and the reason it survives for now is the scheduler rather than the
 * MMU.
 */
static int area_held(void)
{
    struct job *j;
    int i;

    for (i = 0; (j = job_nth(i)) != 0; i++) {
        if (j->state == JOB_STOPPED) {
            return 1;
        }
    }
    return 0;
}

/*
 * A supervisor stack for one program, with a hole underneath it.
 *
 * Three pages: a guard page and then two of stack. The guard is taken
 * out of the KERNEL's map, not the program's -- it is the kernel that
 * would overflow, running the program's system calls -- so an overflow
 * is an access fault at the instruction that did it, rather than a
 * quiet corruption of whatever page happened to be next. A kernel stack
 * that overruns and keeps going is close to the worst failure a system
 * can have, because the damage surfaces somewhere else entirely.
 */
#define KSTACK_PAGES    2
#define KSTACK_TOTAL    (KSTACK_PAGES + 1)

static u32 kstack_alloc(u32 *top)
{
    u32 base = pmm_alloc_pages(KSTACK_TOTAL);

    if (!base) {
        return 0;
    }
    vm_kernel_present(base, 0);         /* the guard */
    *top = base + KSTACK_TOTAL * (u32)PAGE_SIZE;
    return base;
}

static void kstack_free(u32 base)
{
    if (!base) {
        return;
    }
    /* Put the guard page back before the block goes into the pool, or
     * the next thing to be handed that page finds it unmapped. */
    vm_kernel_present(base, 1);
    pmm_free_pages(base, KSTACK_TOTAL);
}

/*
 * Build the address space a program will run in: its image, its stack,
 * and its arguments. Everything it will be able to reach.
 */
static int build(struct addrspace *as, const char *path,
                 int argc, char **argv, u32 *entry, u32 *sp)
{
    u32 va;
    int fd, err;

    fd = fd_open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    err = load_image(as, fd, entry);
    fd_close(fd);
    if (err < 0) {
        return err;
    }

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
    for (va = USER_VA_END - (u32)USER_STACK_PAGES * PAGE_SIZE;
         va < USER_VA_END; va += PAGE_SIZE) {
        if (!vm_map(as, va, 0, VM_USER | VM_WRITE)) {
            return -ENOMEM;
        }
    }

    return setup_stack(argc, argv, sp);
}

int exec_spawn(const char *path, int argc, char **argv)
{
    char cmd[JOB_CMD_MAX];
    struct addrspace *as;
    struct job *j;
    u32 entry = 0, sp = 0, kstack, ktop = 0;
    int err, status, id, prev_fg, prev_depth;

    if (running) {
        /* One at a time: execasm.s has room for one saved context, and a
         * second spawn would overwrite the first's way home. */
        return -EBUSY;
    }
    if (area_held()) {
        return -EBUSY;
    }
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
     */
    uaccess_set(as);
    err = build(as, path, argc, argv, &entry, &sp);
    uaccess_set(0);

    if (err < 0) {
        vm_destroy(as);
        return err;
    }

    describe(cmd, sizeof(cmd), argc, argv);
    kstack = kstack_alloc(&ktop);
    if (!kstack) {
        vm_destroy(as);
        return -ENOMEM;
    }

    id = job_create(cmd, 0);
    if (id < 0) {
        kstack_free(kstack);
        vm_destroy(as);
        return id;
    }
    j = job_get(id);
    j->state = JOB_RUNNING;
    j->as = as;
    j->kstack = kstack;

    /* From here the terminal's ctrl-C and ctrl-Z mean this program. */
    prev_fg = job_foreground();
    job_set_foreground(id);

    running = 1;
    running_as = as;
    uaccess_set(as);
    vm_switch(as);

    /* The program's own count starts at zero, whatever depth the caller
     * was at when it asked for this. */
    prev_depth = syscall_depth_swap(0);

    status = exec_enter(entry, sp, ktop);

    syscall_depth_swap(prev_depth);
    vm_switch(0);
    uaccess_set(0);
    running_as = 0;
    running = 0;
    job_set_foreground(prev_fg);

    /*
     * Put the terminal back. A program that set raw mode and was then
     * killed by ctrl-C never got the chance to, and a console left with
     * echo off looks exactly like a machine that has crashed.
     */
    tty_reset();

    /*
     * Whatever the program left on the disk should be on the disk. It
     * may have been stopped partway through by exit(), with buffers
     * still held, and the shell is about to prompt as though nothing
     * happened.
     */
    vfs_sync();

    if (j->state == JOB_STOPPED) {
        /* Still alive, its address space intact and still holding its
         * pages, listed by `jobs` and resumable with `fg`. Nothing went
         * wrong, so this is not an errno. */
        return SPAWN_STOPPED;
    }

    if (j->state == JOB_DONE) {
        status = j->status;     /* killed: 128 + the signal */
    } else {
        j->state = JOB_DONE;
        j->status = status;
    }

    /* Every page it was given goes back, including the ones holding its
     * tables and the stack its system calls ran on. This is the whole of
     * "the program is gone". */
    vm_destroy(as);
    kstack_free(j->kstack);
    j->as = 0;
    j->kstack = 0;
    return status;
}

/*
 * Resume a stopped job. Returns its exit status, or SPAWN_STOPPED if it
 * stopped again.
 */
int exec_continue(int id)
{
    struct job *j = job_get(id);
    int status, prev_fg, prev_depth;

    if (!j) {
        return -ENOENT;
    }
    if (j->state != JOB_STOPPED) {
        return -EINVAL;
    }
    if (running) {
        return -EBUSY;
    }

    prev_fg = job_foreground();
    job_set_foreground(id);
    j->state = JOB_RUNNING;
    j->signalled = 0;

    running = 1;
    running_as = j->as;
    uaccess_set(j->as);
    vm_switch(j->as);

    /* Back to the depth it stopped at, in the middle of a system call. */
    prev_depth = syscall_depth_swap(j->depth);

    status = exec_resume(j->saved_sp);

    syscall_depth_swap(prev_depth);
    vm_switch(0);
    uaccess_set(0);
    running_as = 0;
    running = 0;
    job_set_foreground(prev_fg);

    tty_reset();
    vfs_sync();

    if (j->state == JOB_STOPPED) {
        return SPAWN_STOPPED;
    }
    if (j->state == JOB_DONE) {
        status = j->status;
    } else {
        j->state = JOB_DONE;
        j->status = status;
    }
    vm_destroy(j->as);
    kstack_free(j->kstack);
    j->as = 0;
    j->kstack = 0;
    return status;
}
