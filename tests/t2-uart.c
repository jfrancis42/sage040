/*
 * t2-uart.c - NS16550A UART.
 *
 * Reference: TI/National PC16550D datasheet.
 *
 * The interesting part is the local-loopback test: setting MCR bit 4
 * internally wires the transmitter to the receiver, so we can prove the
 * chip really moves bytes rather than merely accepting writes.
 */
#include "sage040.h"

int main(void)
{
    u8 v, got;
    int i;

    test_begin("t2 NS16550A UART");

    /* --- scratch register: proves the register file is real storage --- */
    MMIO8(UART_SCR) = 0xA5;
    v = MMIO8(UART_SCR);
    uart_puts("  SCR wrote 0xA5, read 0x"); uart_puthex8(v); uart_putc('\n');
    if (v == 0xA5) {
        test_ok("scratch register holds a value");
    } else {
        test_fail("scratch register did not read back");
    }

    MMIO8(UART_SCR) = 0x5A;
    if (MMIO8(UART_SCR) == 0x5A) {
        test_ok("scratch register holds a second value");
    } else {
        test_fail("scratch register stuck");
    }

    /* --- divisor latch: only visible when LCR.DLAB is set --- */
    MMIO8(UART_LCR) = LCR_DLAB;
    MMIO8(UART_DLL) = 0x12;
    MMIO8(UART_DLM) = 0x34;
    v = MMIO8(UART_DLL);
    got = MMIO8(UART_DLM);
    MMIO8(UART_LCR) = LCR_8N1;          /* DLAB off again */
    uart_puts("  DLL=0x"); uart_puthex8(v);
    uart_puts(" DLM=0x"); uart_puthex8(got); uart_putc('\n');
    if (v == 0x12 && got == 0x34) {
        test_ok("divisor latch readable when DLAB=1");
    } else {
        test_fail("divisor latch WRONG");
    }

    /* --- LCR holds the line format --- */
    v = MMIO8(UART_LCR);
    uart_puts("  LCR         = 0x"); uart_puthex8(v); uart_putc('\n');
    if ((v & 0x03) == LCR_8N1 && !(v & LCR_DLAB)) {
        test_ok("LCR reports 8N1 with DLAB clear");
    } else {
        test_fail("LCR WRONG");
    }

    /* --- LSR should say the transmitter is idle --- */
    v = MMIO8(UART_LSR);
    uart_puts("  LSR         = 0x"); uart_puthex8(v); uart_putc('\n');
    if ((v & (LSR_THRE | LSR_TEMT)) == (LSR_THRE | LSR_TEMT)) {
        test_ok("LSR reports transmitter empty");
    } else {
        test_fail("LSR does not report an idle transmitter");
    }

    /*
     * --- local loopback ---
     * MCR bit 4 loops TX back into RX inside the chip.  Anything we write
     * to THR must reappear in RBR.  This is the real proof the datapath
     * works; everything above only proves registers exist.
     */
    uart_puts("  entering local loopback...\n");
    MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;
    MMIO8(UART_MCR) = MCR_LOOP | MCR_DTR | MCR_RTS;

    {
        static const u8 pattern[] = { 0x00, 0x55, 0xAA, 0xFF, 0x42, 0x7F };
        int good = 1;

        for (i = 0; i < (int)sizeof(pattern); i++) {
            int spin = 0;

            while (!(MMIO8(UART_LSR) & LSR_THRE) && spin < 100000) {
                spin++;
            }
            MMIO8(UART_THR) = pattern[i];

            spin = 0;
            while (!(MMIO8(UART_LSR) & LSR_DR) && spin < 100000) {
                spin++;
            }
            if (!(MMIO8(UART_LSR) & LSR_DR)) {
                good = 0;
                break;
            }
            got = MMIO8(UART_RBR);
            if (got != pattern[i]) {
                good = 0;
                break;
            }
        }

        /* Leave loopback before reporting, or nothing reaches the console. */
        MMIO8(UART_MCR) = MCR_DTR | MCR_RTS;
        MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;

        if (good) {
            test_ok("loopback returned all 6 test bytes intact");
        } else {
            uart_puts("  failed at byte "); uart_putdec((u32)i);
            uart_puts(", got 0x"); uart_puthex8(got); uart_putc('\n');
            test_fail("loopback data mismatch");
        }
    }

    /* --- and the console still works after leaving loopback --- */
    test_ok("console still alive after loopback");

    test_end();
    return 0;
}
