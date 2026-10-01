/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mount - put another volume into the tree, or say what is there.
 *
 *   mount                          list what is mounted (/proc/mounts)
 *   mount [-t TYPE] [-o ro|rw] [-r] DEVICE DIR
 *
 * DEVICE is a partition of the disk -- /dev/hda2, or plain hda2 -- and
 * DIR an existing directory on the disk. The type can only be ext2 (or
 * ext3, the same volume with its journal, or auto); anything else is
 * refused, as Linux refuses a type it has no driver for. Only root
 * mounts.
 */
#include "ulib.h"

static const char *why(s32 err)
{
    switch (-err) {
    case ENOENT:  return "no such device or directory";
    case ENOTDIR: return "not a directory";
    case EBUSY:   return "already mounted, or something is mounted there";
    case EINVAL:  return "cannot mount there, or not a volume this can read";
    case EPERM:   return "only root can mount";
    case ENODEV:  return "unknown filesystem type";
    case EOPNOTSUPP: return "the volume uses a feature this kernel lacks";
    case EIO:     return "the device could not be read";
    case ENOMEM:  return "not enough memory";
    case EMFILE:  return "too many volumes mounted";
    default:      return "failed";
    }
}

static int list(void)
{
    char buf[512];
    int fd = open("/proc/mounts", O_RDONLY);
    s32 n;

    if (fd < 0) {
        eputs("mount: cannot read /proc/mounts\n");
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (u32)n);
    }
    close(fd);
    return 0;
}

static void usage(void)
{
    eputs("usage: mount [-t TYPE] [-o ro|rw] [-r] DEVICE DIR\n"
          "       mount            (list what is mounted)\n");
}

int main(int argc, char **argv)
{
    const char *type = 0, *src = 0, *dir = 0;
    u32 flags = 0;
    s32 r;
    int i;

    if (argc == 1) {
        return list();
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            const char *o = argv[++i];

            if (strcmp(o, "ro") == 0) {
                flags |= MS_RDONLY;
            } else if (strcmp(o, "rw") == 0 || strcmp(o, "defaults") == 0) {
                flags &= ~(u32)MS_RDONLY;
            } else {
                eputs("mount: option not supported: ");
                eputs(o);
                eputs("\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0) {
            flags |= MS_RDONLY;
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        } else if (!src) {
            src = argv[i];
        } else if (!dir) {
            dir = argv[i];
        } else {
            usage();
            return 1;
        }
    }
    if (!src || !dir) {
        usage();
        return 1;
    }
    r = syscall(__NR_mount, (u32)src, (u32)dir, (u32)type, flags, 0);
    if (r < 0) {
        eputs("mount: ");
        eputs(src);
        eputs(" on ");
        eputs(dir);
        eputs(": ");
        eputs(why(r));
        eputs("\n");
        return 32;              /* mount(8)'s "mount failure" */
    }
    return 0;
}
