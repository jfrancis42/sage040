/*
 * t9-mfp-usart.c - the MC68901 USART.
 *
 * Reference: Motorola MC68901 Multi-Function Peripheral datasheet.
 *
 * The MFP's serial channel is the board's second serial port.  The test
 * harness wires it to a file for transmit and feeds it three known bytes
 * for receive, so both directions are exercised against real data rather
 * than just register state.
 *
 * Covered:
 *   - UCR and SCR hold their values
 *   - the transmitter reports buffer-empty only once enabled
 *   - transmitted bytes actually leave the chip (checked by the harness
 *     against t9-mfp-usart.usart)
 *   - the receiver sets buffer-full and returns the bytes sent to it
 *   - reading the data register clears buffer-full
 *   - the transmit-empty and receive-full interrupt channels fire
 */
#include "sage040.h"

static volatile int delay_sink;

static void delay(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        delay_sink++;
    }
}

static void usart_putc(char c)
{
    int spin;

    for (spin = 0; spin < 200000; spin++) {
        if (MMIO8(MFP_TSR) & MFP_TSR_BE) {
            break;
        }
    }
    MMIO8(MFP_UDR) = (u8)c;
}

int main(void)
{
    u8 v;
    int i, spin;

    test_begin("t9 MC68901 USART");

    mfp_set_ipl(7);
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    MMIO8(MFP_IMRA) = 0;
    MMIO8(MFP_IMRB) = 0;
    mfp_install_vectors(0x40);
    MMIO8(MFP_VR) = 0x40;

    /* --- control registers --- */
    MMIO8(MFP_UCR) = 0x88;          /* /16 clock, 8 bits, 1 stop */
    MMIO8(MFP_SCR) = 0x16;
    if (MMIO8(MFP_UCR) == 0x88 && MMIO8(MFP_SCR) == 0x16) {
        test_ok("UCR and SCR hold their values");
    } else {
        test_fail("UCR / SCR WRONG");
    }

    /* --- transmitter starts disabled --- */
    MMIO8(MFP_TSR) = 0x00;
    v = MMIO8(MFP_TSR);
    uart_puts("  TSR disabled = 0x"); uart_puthex8(v); uart_putc('\n');
    if (!(v & MFP_TSR_TE) && !(v & MFP_TSR_BE)) {
        test_ok("a disabled transmitter does not claim an empty buffer");
    } else {
        test_fail("disabled transmitter status WRONG");
    }

    /* --- enable it --- */
    MMIO8(MFP_TSR) = MFP_TSR_TE;
    v = MMIO8(MFP_TSR);
    uart_puts("  TSR enabled  = 0x"); uart_puthex8(v); uart_putc('\n');
    if ((v & MFP_TSR_TE) && (v & MFP_TSR_BE)) {
        test_ok("enabling the transmitter reports buffer empty");
    } else {
        test_fail("enabled transmitter status WRONG");
    }

    /* --- send a known string; the harness checks the file --- */
    {
        static const char msg[] = "MFP-USART-TX-OK\n";

        for (i = 0; msg[i]; i++) {
            usart_putc(msg[i]);
        }
        test_ok("transmitted 16 bytes through the USART");
    }

    /* --- transmit-empty interrupt --- */
    mfp_reset_counts();
    mfp_enable(MFPCH_XMIT_EMPTY);
    mfp_clear_pending(MFPCH_XMIT_EMPTY);
    usart_putc('X');
    if (mfp_is_pending(MFPCH_XMIT_EMPTY)) {
        test_ok("transmit-empty sets its pending bit (channel 10)");
    } else {
        test_fail("transmit-empty channel did not become pending");
    }

    mfp_set_ipl(0);
    for (spin = 0; spin < 500000 && mfp_irq_count == 0; spin++) {
        /* wait for delivery */
    }
    mfp_set_ipl(7);
    mfp_disable(MFPCH_XMIT_EMPTY);

    uart_puts("  xmit vector = 0x"); uart_puthex8((u8)mfp_last_vector);
    uart_putc('\n');
    if (mfp_irq_count > 0 && mfp_last_channel == MFPCH_XMIT_EMPTY) {
        test_ok("transmit-empty interrupt delivered on channel 10");
    } else {
        test_fail("transmit-empty interrupt not delivered");
    }

    /* --- receiver --- */
    MMIO8(MFP_RSR) = 0x00;
    if (!(MMIO8(MFP_RSR) & MFP_RSR_RE)) {
        test_ok("receiver starts disabled");
    } else {
        test_fail("receiver enable bit WRONG");
    }

    mfp_reset_counts();
    mfp_enable(MFPCH_RCV_FULL);
    mfp_clear_pending(MFPCH_RCV_FULL);
    MMIO8(MFP_RSR) = MFP_RSR_RE;        /* enable: input may now arrive */

    for (spin = 0; spin < 2000000; spin++) {
        if (MMIO8(MFP_RSR) & MFP_RSR_BF) {
            break;
        }
        delay(4);
    }

    v = MMIO8(MFP_RSR);
    uart_puts("  RSR after enable = 0x"); uart_puthex8(v); uart_putc('\n');
    if (v & MFP_RSR_BF) {
        test_ok("receiver reports buffer full after input arrives");
    } else {
        test_fail("no input ever reached the receiver");
        test_end();
        return 0;
    }

    if (mfp_is_pending(MFPCH_RCV_FULL)) {
        test_ok("receive-full set its pending bit (channel 12)");
    } else {
        test_fail("receive-full channel did not become pending");
    }

    /* The harness feeds the three bytes "RX!". */
    {
        char got[4];
        int n = 0;

        for (n = 0; n < 3; n++) {
            for (spin = 0; spin < 2000000; spin++) {
                if (MMIO8(MFP_RSR) & MFP_RSR_BF) {
                    break;
                }
                delay(4);
            }
            if (!(MMIO8(MFP_RSR) & MFP_RSR_BF)) {
                break;
            }
            got[n] = (char)MMIO8(MFP_UDR);
        }
        got[3] = 0;

        uart_puts("  received '"); uart_puts(got); uart_puts("'\n");
        if (n == 3 && got[0] == 'R' && got[1] == 'X' && got[2] == '!') {
            test_ok("received exactly the bytes the harness sent");
        } else {
            test_fail("received data WRONG");
        }
    }

    /* Reading the data register must clear buffer-full. */
    if (!(MMIO8(MFP_RSR) & MFP_RSR_BF)) {
        test_ok("reading the data register clears buffer full");
    } else {
        test_fail("buffer-full stuck after reading the data register");
    }

    MMIO8(MFP_RSR) = 0;
    MMIO8(MFP_TSR) = 0;
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    mfp_set_ipl(7);

    test_end();
    return 0;
}
