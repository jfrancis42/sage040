/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * locale - which locale is in force, and which there are.
 *
 *   locale                 the categories, as the environment sets them
 *   locale -a              the locales setlocale(3) will accept
 *   locale -m              the character sets (charmaps)
 *   locale [-k] NAME...    a value: charmap, codeset, decimal_point,
 *                          thousands_sep, d_t_fmt, d_fmt, t_fmt,
 *                          yesexpr, noexpr -- or a category's name
 *
 * picolibc's locales are its character sets: the part of a name before
 * the dot is accepted and ignored, the part after chooses the charset
 * (C.UTF-8, en_US.UTF-8 and de_DE.ISO-8859-1 are all fine; zh_TW.big5
 * is not, there being no Big5). So -a lists POSIX, C and C.<charset>
 * for each one, each checked with setlocale before it is printed -- a
 * list that claims a locale setlocale refuses is worse than none.
 */
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const charsets[] = {
    "UTF-8", "ISO-8859-1", "ISO-8859-2", "ISO-8859-3", "ISO-8859-4",
    "ISO-8859-5", "ISO-8859-6", "ISO-8859-7", "ISO-8859-8", "ISO-8859-9",
    "ISO-8859-10", "ISO-8859-11", "ISO-8859-13", "ISO-8859-14",
    "ISO-8859-15", "ISO-8859-16", "CP437", "CP720", "CP737", "CP775",
    "CP850", "CP852", "CP855", "CP857", "CP858", "CP862", "CP866",
    "CP874", "CP1125", "CP1250", "CP1251", "CP1252", "CP1253", "CP1254",
    "CP1255", "CP1256", "CP1257", "CP1258", "KOI8-R", "KOI8-U",
    "GEORGIAN-PS", "PT154", "KOI8-T", "JIS", "EUC-JP", "SHIFT-JIS",
};
#define NCHARSETS (sizeof(charsets) / sizeof(charsets[0]))

static const char *const cats[] = {
    "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE", "LC_MONETARY",
    "LC_MESSAGES",
};

/* POSIX's rule for what a category is: LC_ALL, then the category's own
 * variable, then LANG, then POSIX. `quoted` says it was not set
 * directly, which is how glibc's locale marks it. */
static const char *value_of(const char *cat, int *quoted)
{
    const char *all = getenv("LC_ALL"), *v = getenv(cat), *lang = getenv("LANG");

    *quoted = 1;
    if (all && *all) {
        return all;
    }
    if (v && *v) {
        *quoted = 0;
        return v;
    }
    return lang && *lang ? lang : "POSIX";
}

static void show_env(void)
{
    const char *lang = getenv("LANG"), *all = getenv("LC_ALL");
    unsigned i;

    printf("LANG=%s\n", lang ? lang : "");
    for (i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) {
        int q;
        const char *v = value_of(cats[i], &q);

        printf(q ? "%s=\"%s\"\n" : "%s=%s\n", cats[i], v);
    }
    printf("LC_ALL=%s\n", all ? all : "");
}

static int usable(const char *name)
{
    return setlocale(LC_CTYPE, name) != NULL;
}

static void show_all(void)
{
    char name[32];
    unsigned i;

    puts("C");
    puts("POSIX");
    for (i = 0; i < NCHARSETS; i++) {
        snprintf(name, sizeof(name), "C.%s", charsets[i]);
        if (usable(name)) {
            puts(name);
        }
    }
    setlocale(LC_CTYPE, "C");
}

static int show_key(const char *k, int with_name)
{
    static const struct { const char *name; nl_item item; } keys[] = {
        { "charmap", CODESET }, { "codeset", CODESET },
        { "decimal_point", RADIXCHAR }, { "thousands_sep", THOUSEP },
        { "d_t_fmt", D_T_FMT }, { "d_fmt", D_FMT }, { "t_fmt", T_FMT },
        { "yesexpr", YESEXPR }, { "noexpr", NOEXPR },
    };
    unsigned i;

    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(k, keys[i].name) == 0) {
            const char *v = nl_langinfo(keys[i].item);

            if (with_name) {
                printf("%s=\"%s\"\n", k, v ? v : "");
            } else {
                puts(v ? v : "");
            }
            return 0;
        }
    }
    for (i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) {
        if (strcmp(k, cats[i]) == 0) {
            int q;

            puts(value_of(cats[i], &q));
            return 0;
        }
    }
    fprintf(stderr, "locale: unknown name \"%s\"\n", k);
    return 1;
}

int main(int argc, char **argv)
{
    int i = 1, kflag = 0, rc = 0;

    if (argc == 1) {
        show_env();
        return 0;
    }
    if (strcmp(argv[1], "-a") == 0) {
        show_all();
        return 0;
    }
    if (strcmp(argv[1], "-m") == 0) {
        unsigned c;

        for (c = 0; c < NCHARSETS; c++) {
            puts(charsets[c]);
        }
        return 0;
    }
    if (strcmp(argv[1], "-k") == 0) {
        kflag = 1;
        i = 2;
    }
    if (argv[i] && argv[i][0] == '-') {
        fprintf(stderr, "usage: locale [-a | -m | [-k] name...]\n");
        return 1;
    }
    if (!setlocale(LC_ALL, "")) {
        fprintf(stderr, "locale: Cannot set LC_ALL to default locale: "
                "No such file or directory\n");
    }
    for (; i < argc; i++) {
        rc |= show_key(argv[i], kflag);
    }
    return rc;
}
