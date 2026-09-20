/*
 * t10-sm501.c - Silicon Motion SM501 display controller.
 *
 * Reference: Silicon Motion SM501 datasheet.
 *
 * The SM501 presents two windows: 16 MiB of video memory at 0xf0000000
 * and a control register block at 0xff400000.  For a simple OS the video
 * memory is the interesting part - it is ordinary linear memory that the
 * display engine scans out, so a framebuffer driver is little more than
 * "write pixels there".
 *
 * Covered:
 *   - the device identifies itself
 *   - control registers are readable
 *   - video memory stores and returns 8-, 16- and 32-bit values
 *   - all 16 MiB are distinct (no aliasing)
 *   - a framebuffer-sized fill survives a full read-back
 */
#include "sage040.h"

int main(void)
{
    u32 id;
    u32 off;
    int bad;

    test_begin("t10 Silicon Motion SM501");

    /* --- identity --- */
    id = SM501_RD(SM501_DEVICEID);
    uart_puts("  DEVICEID    = 0x"); uart_puthex32(id); uart_putc('\n');
    if (id == SM501_DEVICEID_VALUE) {
        test_ok("device identifies as SM501 revision A0");
    } else {
        test_fail("unexpected device id");
    }

    if (MMIO32(SM501_DEVICEID) == 0xA0000105UL) {
        test_ok("registers are little-endian, as the datasheet specifies");
    } else {
        test_fail("register endianness is not what was expected");
    }

    uart_puts("  SYSCTL      = 0x"); uart_puthex32(SM501_RD(SM501_SYSTEM_CONTROL));
    uart_puts("\n  MISCCTL     = 0x"); uart_puthex32(SM501_RD(SM501_MISC_CONTROL));
    uart_puts("\n  DRAMCTL     = 0x"); uart_puthex32(SM501_RD(SM501_DRAM_CONTROL));
    uart_putc('\n');
    test_ok("control registers are readable");

    /* --- video memory holds values at every width --- */
    MMIO32(SM501_VRAM + 0x000) = 0xDEADBEEFUL;
    MMIO16(SM501_VRAM + 0x100) = 0xC0DE;
    MMIO8(SM501_VRAM + 0x200)  = 0x5A;
    if (MMIO32(SM501_VRAM + 0x000) == 0xDEADBEEFUL &&
        MMIO16(SM501_VRAM + 0x100) == 0xC0DE &&
        MMIO8(SM501_VRAM + 0x200) == 0x5A) {
        test_ok("video memory stores 8, 16 and 32 bit values");
    } else {
        test_fail("video memory did not read back");
    }

    /* --- byte order through video memory --- */
    MMIO32(SM501_VRAM + 0x300) = 0x11223344UL;
    if (MMIO8(SM501_VRAM + 0x300) == 0x11 &&
        MMIO8(SM501_VRAM + 0x303) == 0x44) {
        test_ok("video memory is big-endian, matching the CPU");
    } else {
        uart_puts("  bytes = ");
        uart_puthex8(MMIO8(SM501_VRAM + 0x300));
        uart_putc(' ');
        uart_puthex8(MMIO8(SM501_VRAM + 0x303));
        uart_putc('\n');
        test_fail("video memory byte order WRONG");
    }

    /* --- the full 16 MiB is distinct storage, not an alias --- */
    MMIO32(SM501_VRAM + 0x00000000UL) = 0x11111111UL;
    MMIO32(SM501_VRAM + 0x00400000UL) = 0x22222222UL;
    MMIO32(SM501_VRAM + 0x00800000UL) = 0x33333333UL;
    MMIO32(SM501_VRAM + 0x00FFFFFCUL) = 0x44444444UL;
    if (MMIO32(SM501_VRAM + 0x00000000UL) == 0x11111111UL &&
        MMIO32(SM501_VRAM + 0x00400000UL) == 0x22222222UL &&
        MMIO32(SM501_VRAM + 0x00800000UL) == 0x33333333UL &&
        MMIO32(SM501_VRAM + 0x00FFFFFCUL) == 0x44444444UL) {
        test_ok("all 16 MiB of video memory are independent");
    } else {
        test_fail("video memory aliases - size is wrong");
    }

    /* --- a framebuffer-sized fill: 640x480 at 8bpp --- */
    bad = 0;
    for (off = 0; off < 640UL * 480UL; off += 4) {
        MMIO32(SM501_VRAM + off) = off;
    }
    for (off = 0; off < 640UL * 480UL; off += 4) {
        if (MMIO32(SM501_VRAM + off) != off) {
            bad = 1;
            break;
        }
    }
    uart_puts("  filled and verified 640x480 bytes ("); uart_putdec(640UL * 480UL);
    uart_puts(")\n");
    if (!bad) {
        test_ok("a 640x480 framebuffer survives a full read-back");
    } else {
        uart_puts("  first mismatch at offset "); uart_putdec(off); uart_putc('\n');
        test_fail("framebuffer read-back mismatch");
    }

    test_end();
    return 0;
}
