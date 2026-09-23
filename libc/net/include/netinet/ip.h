/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 Jeff Francis */
/*
 * netinet/ip.h - the IPv4 header, and the type-of-service values that
 * go in it.
 *
 * Almost nothing includes this to build a packet; the kernel's network
 * stack has its own definitions and does not use these. What includes
 * it is ordinary software setting IP_TOS on a socket -- ssh asks for
 * IPTOS_LOWDELAY on an interactive session and IPTOS_THROUGHPUT on a
 * bulk transfer -- and that software expects the constants to be here,
 * at this path, because that is where every Unix keeps them.
 *
 * Both spellings of the header are here. `struct ip` is BSD's and is
 * what portable code uses; `struct iphdr` is Linux's and is what code
 * written on Linux uses. They describe the same bytes.
 */
#ifndef _NETINET_IP_H_
#define _NETINET_IP_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <netinet/in.h>

/*
 * Type of service. The low bit is reserved; the four that matter are a
 * hint to the routers in between about what to do when they cannot do
 * everything.
 */
#define IPTOS_TOS_MASK          0x1E
#define IPTOS_TOS(tos)          ((tos) & IPTOS_TOS_MASK)
#define IPTOS_LOWDELAY          0x10
#define IPTOS_THROUGHPUT        0x08
#define IPTOS_RELIABILITY       0x04
#define IPTOS_LOWCOST           0x02
#define IPTOS_MINCOST           IPTOS_LOWCOST

/* Precedence, the top three bits: the older way of saying the same. */
#define IPTOS_PREC_MASK                 0xE0
#define IPTOS_PREC(tos)                 ((tos) & IPTOS_PREC_MASK)
#define IPTOS_PREC_NETCONTROL           0xE0
#define IPTOS_PREC_INTERNETCONTROL      0xC0
#define IPTOS_PREC_CRITIC_ECP           0xA0
#define IPTOS_PREC_FLASHOVERRIDE        0x80
#define IPTOS_PREC_FLASH                0x60
#define IPTOS_PREC_IMMEDIATE            0x40
#define IPTOS_PREC_PRIORITY             0x20
#define IPTOS_PREC_ROUTINE              0x00

/* DiffServ, which replaced the above and reuses the same octet. */
#define IPTOS_DSCP_MASK         0xfc
#define IPTOS_DSCP(x)           ((x) & IPTOS_DSCP_MASK)
#define IPTOS_DSCP_AF11         0x28
#define IPTOS_DSCP_AF21         0x48
#define IPTOS_DSCP_AF31         0x68
#define IPTOS_DSCP_AF41         0x88
#define IPTOS_DSCP_EF           0xb8
#define IPTOS_CLASS_MASK        0xe0
#define IPTOS_CLASS(class)      ((class) & IPTOS_CLASS_MASK)
#define IPTOS_CLASS_CS0         0x00
#define IPTOS_CLASS_CS1         0x20
#define IPTOS_CLASS_CS2         0x40
#define IPTOS_CLASS_CS3         0x60
#define IPTOS_CLASS_CS4         0x80
#define IPTOS_CLASS_CS5         0xa0
#define IPTOS_CLASS_CS6         0xc0
#define IPTOS_CLASS_CS7         0xe0
#define IPTOS_CLASS_DEFAULT     IPTOS_CLASS_CS0

/* Options, in the rare packet that carries any. */
#define IPOPT_COPY              0x80
#define IPOPT_CLASS_MASK        0x60
#define IPOPT_NUMBER_MASK       0x1f
#define IPOPT_CONTROL           0x00
#define IPOPT_RESERVED1         0x20
#define IPOPT_MEASUREMENT       0x40
#define IPOPT_RESERVED2         0x60
#define IPOPT_END               0
#define IPOPT_NOOP              1
#define IPOPT_SEC               (2 | IPOPT_COPY)
#define IPOPT_LSRR              (3 | IPOPT_COPY)
#define IPOPT_TIMESTAMP         (4 | IPOPT_MEASUREMENT)
#define IPOPT_RR                7
#define IPOPT_SSRR              (9 | IPOPT_COPY)

#define IPVERSION       4
#define IP_MAXPACKET    65535
#define MAXTTL          255
#define IPDEFTTL        64
#define IPFRAGTTL       60
#define IPTTLDEC        1

_BEGIN_STD_C

/*
 * The header itself. The bit-field order follows the byte order,
 * which is why this is written twice: on a big-endian machine the
 * version comes first within its octet, and on a little-endian one
 * the header length does. This machine is big-endian, and the
 * #if is kept so the file stays correct if it is ever read
 * anywhere else.
 */
struct iphdr {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned int ihl:4;
    unsigned int version:4;
#else
    unsigned int version:4;
    unsigned int ihl:4;
#endif
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
    /* Options follow, if ihl > 5. */
};

/* BSD's spelling of the same bytes. */
struct ip {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned int ip_hl:4;
    unsigned int ip_v:4;
#else
    unsigned int ip_v:4;
    unsigned int ip_hl:4;
#endif
    uint8_t  ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_off;
#define IP_RF      0x8000       /* reserved */
#define IP_DF      0x4000       /* do not fragment */
#define IP_MF      0x2000       /* more fragments follow */
#define IP_OFFMASK 0x1fff
    uint8_t  ip_ttl;
    uint8_t  ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};

_END_STD_C

#endif /* _NETINET_IP_H_ */
