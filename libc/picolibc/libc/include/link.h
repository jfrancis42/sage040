/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright © 2026 Jeff Francis
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * <link.h>: dl_iterate_phdr, which is how a program -- or an unwinder,
 * or a sanitizer -- finds every object loaded into it and their program
 * headers. glibc's structure, field for field. There is no <elf.h> in
 * this C library, so the few ELF types it needs are defined here.
 */
#ifndef _LINK_H_
#define _LINK_H_

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>

#ifndef _ELF_H
typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef uint32_t Elf32_Word;
typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;
#endif

#define ElfW(type)  Elf32_##type

struct dl_phdr_info {
    ElfW(Addr)        dlpi_addr;        /* where the object is loaded    */
    const char       *dlpi_name;        /* its path; "" for the program  */
    const ElfW(Phdr) *dlpi_phdr;
    ElfW(Half)        dlpi_phnum;
    unsigned long long dlpi_adds;       /* objects loaded so far          */
    unsigned long long dlpi_subs;       /* and unloaded: always 0 here    */
    size_t            dlpi_tls_modid;   /* its TLS module, or 0           */
    void             *dlpi_tls_data;    /* this thread's block, or 0      */
};

_BEGIN_STD_C

int dl_iterate_phdr(int (*__callback)(struct dl_phdr_info *, size_t, void *),
                    void *__data);

_END_STD_C

#endif /* _LINK_H_ */
