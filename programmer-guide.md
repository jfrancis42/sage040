# Sage040 Programmer's Guide

Everything needed to build and run code on the Sage040.

**The machine:** a 68040 workstation built only from real, documented silicon —
no virtio, no paravirtual anything. Every device below corresponds to a physical
part you could buy and solder, and every register here was verified against the
running emulator by the test suite in [`tests/`](tests/).

| Function | Part | Datasheet |
|---|---|---|
| CPU, FPU, MMU | Motorola **MC68040** | *M68040 User's Manual* (M68040UM) |
| Timers + interrupt controller | Motorola **MC68901** MFP | *MC68901 Multi-Function Peripheral* |
| Serial console | National **NS16550A** | *PC16550D* (TI/NS) |
| Disk | **ATA taskfile** (WD1003 lineage) | T13 ATA/ATAPI |
| Ethernet | SMSC **LAN91C111** | *LAN91C111* (SMSC/Microchip) |
| Video | Silicon Motion **SM501** | *SM501* |

---

## 1. Toolchain

Everything is at `~/m68k/install/bin` and on `PATH`:

```
m68k-elf-gcc      15.2.0     m68k-elf-gdb      17.1
m68k-elf-as/ld    2.45       objdump/objcopy/nm/readelf/size/ar/...
vasmm68k_mot      2.8c       (system-wide, Motorola syntax)
```

It is **freestanding** — no libc, no newlib. That is the right shape for kernel
work.

### Build flags

```make
CPUFLAGS := -mcpu=68040
CFLAGS   := $(CPUFLAGS) -ffreestanding -nostdlib -nostdinc -O2 \
            -Wall -Wextra -fno-builtin -fno-stack-protector -I.
LDFLAGS  := $(CPUFLAGS) -ffreestanding -nostdlib -T sage040.ld \
            -Wl,--build-id=none
```

`ld` warns `has a LOAD segment with RWX permissions` on every link. Harmless for
a single-segment bare-metal image; silence with `-Wl,--no-warn-rwx-segments`.

### Running

```bash
~/m68k/sage040-qemu/bin/qemu-system-m68k -M sage040 -cpu m68040 -m 4 \
    -kernel kernel.elf \
    -serial file:out.txt \
    -chardev file,id=mfpusart,path=usart.txt,input-path=usart.in \
    -serial chardev:mfpusart \
    -display none -no-reboot \
    -drive file=disk.img,format=raw,if=ide \
    -nic user,model=smc91c111
```

Serial 0 is the 16550 console; serial 1 is the MFP's USART.

> **`-serial stdio` shows nothing.** Use `-serial file:` or `-serial mon:stdio`.

> **`STOP` halts the CPU but not QEMU.** Scripts should watch the output for a
> sentinel and kill QEMU, as `tests/runtest.sh` does.

---

## 2. Booting

QEMU's `-kernel` loads a **big-endian ELF32** with `e_machine = EM_68K (4)` and
`EI_DATA = ELFDATA2MSB`, then starts at the ELF entry point with:

- **SP** = top of RAM − 16
- **PC** = ELF entry
- Supervisor mode, IPL 7 (all interrupts masked)

There is no ROM, no bootloader, and **deliberately no bootinfo block** — the OS
is expected to know its own machine.

### Linker script

```ld
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M }
_stack_top = ORIGIN(RAM) + LENGTH(RAM) - 16;

SECTIONS {
    .vectors 0x00000000 : { KEEP(*(.vectors)) } > RAM
    .text    0x00000400 : { *(.text .text.*) }  > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data   : { *(.data .data.*) }     > RAM
    .bss    : { . = ALIGN(4); __bss_start = .;
                *(.bss .bss.*) *(COMMON);
                . = ALIGN(4); __bss_end = .; } > RAM
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
```

Vectors live at `0x0` (256 × 4 bytes = 1 KB); code starts at `0x400`.

### Minimal entry

```asm
_start: move.w  #0x2700,%sr         | supervisor, interrupts masked
        lea     _stack_top,%sp
        lea     _vectors,%a0
        movec   %a0,%vbr            | 68040 has VBR; use it
        lea     __bss_start,%a0     | clear .bss
        lea     __bss_end,%a1
1:      cmpa.l  %a1,%a0
        bcc.s   2f
        clr.b   (%a0)+
        bra.s   1b
2:      jsr     main
```

A complete, working version is `tests/crt0.s`.

---

## 3. Memory map

| Base | Size | Device | Access width | Endianness |
|------|------|--------|--------------|------------|
| `0x00000000` | `-m` (default 4 MB) | RAM | any | big |
| `0xf0000000` | 16 MiB | SM501 video memory | any | **big** (plain RAM) |
| `0xff000000` | 8 | NS16550A UART | **byte** | n/a |
| `0xff100000` | 16 | ATA command block | byte, **16-bit for data** | **little** for data |
| `0xff101000` | 2 | ATA control block | byte | n/a |
| `0xff200000` | 16 | LAN91C111 | byte and 16-bit | native (big) |
| `0xff300000` | 24 | MC68901 MFP | **byte** | n/a |
| `0xff400000` | 2 MiB | SM501 control registers | **32-bit only** | **little** |

**Endianness is not uniform, and this is the single biggest source of bugs.**
See §9.

---

## 4. Interrupts

The **MC68901 is the interrupt controller.** It drives **IPL 6** and supplies
its own vector — there is no autovector controller on this board.

### Board wiring

| MFP pin | Channel | Source | Notes |
|---|---|---|---|
| GPIP5 | 7 | NS16550A UART | |
| GPIP4 | 6 | ATA | also **TAI**, timer A event input |
| GPIP3 | 3 | LAN91C111 | also **TBI**, timer B event input |

### Channel priority (15 highest → 0 lowest)

| Ch | Source | Ch | Source |
|---|---|---|---|
| 15 | GPIP7 | 7 | GPIP5 → **UART** |
| 14 | GPIP6 | 6 | GPIP4 → **ATA** |
| 13 | **Timer A** | 5 | **Timer C** |
| 12 | USART receive full | 4 | **Timer D** |
| 11 | USART receive error | 3 | GPIP3 → **ethernet** |
| 10 | USART transmit empty | 2 | GPIP2 |
| 9 | USART transmit error | 1 | GPIP1 |
| 8 | **Timer B** | 0 | GPIP0 |

Channels 8–15 live in the **A** registers (bit `ch-8`); channels 0–7 in the
**B** registers (bit `ch`).

### Vectors

```
vector = (VR & 0xF0) | channel
```

With `VR = 0x40`, timer C (channel 5) arrives at vector `0x45`, i.e. address
`VBR + 0x45*4`. Install handlers at all sixteen.

### The rules that actually bite

1. **A disabled channel never becomes pending.** If the IER bit is clear, the
   IPR bit is not set at all — the interrupt is lost, not deferred.
2. **IPR and ISR clear by writing a zero.** Writing a one leaves the bit alone.
   So a handler clears exactly its own bit with `IPRB = ~(1 << ch)`.
3. **Acknowledge clears the pending bit** automatically, as on real silicon.
4. **IMR masks delivery but not pending.** A masked channel stays pending and
   fires the moment it is unmasked.
5. **In software end-of-interrupt mode** (`VR` bit 3 set) acknowledge also sets
   the in-service bit, which inhibits that channel **and every lower-priority
   one** until the handler clears it. With the bit clear, acknowledge is
   automatic and ISR is never set.

### A working handler

The MFP is vectored, so one stub can serve all sixteen channels by recovering
the vector from the exception frame the 68040 pushed:

```
frame:  SP+0  SR (word)
        SP+2  PC (long)
        SP+6  format/vector word — bits 11-0 = vector * 4
```

```asm
_mfp_isr:
        movem.l %d0-%d1/%a0-%a1,-(%sp)   | 16 bytes pushed
        moveq   #0,%d0
        move.w  22(%sp),%d0              | 16 + 6
        andi.l  #0xfff,%d0
        lsr.l   #2,%d0                   | -> vector number
        move.l  %d0,-(%sp)
        jsr     mfp_isr_c
        addq.l  #4,%sp
        movem.l (%sp)+,%d0-%d1/%a0-%a1
        rte
```

```c
void mfp_isr_c(u32 vector)
{
    int ch = vector & 0x0f;
    /* ... service the device ... */
    if (ch >= 8) MMIO8(MFP_ISRA) = ~(1 << (ch - 8));   /* end of interrupt */
    else         MMIO8(MFP_ISRB) = ~(1 << ch);
}
```

### Setting the CPU interrupt mask

```c
static void set_ipl(int level)      /* 0 = all on, 7 = all masked */
{
    u16 sr = 0x2000 | ((level & 7) << 8);   /* supervisor + mask */
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
}
```

---

## 5. MC68901 MFP — `0xff300000`

Byte-wide registers at consecutive addresses.

| Off | Reg | Off | Reg | Off | Reg |
|---|---|---|---|---|---|
| `00` | GPIP | `08` | ISRB | `10` | TBDR |
| `01` | AER | `09` | IMRA | `11` | TCDR |
| `02` | DDR | `0a` | IMRB | `12` | TDDR |
| `03` | IERA | `0b` | VR | `13` | SCR |
| `04` | IERB | `0c` | TACR | `14` | UCR |
| `05` | IPRA | `0d` | TBCR | `15` | RSR |
| `06` | IPRB | `0e` | TCDCR | `16` | TSR |
| `07` | ISRA | `0f` | TADR | `17` | UDR |

### Timers

Four 8-bit down counters. Timer clock is **2.4576 MHz**. Each reloads from its
data register and raises its channel on passing zero.

Control values (low 3 bits select the prescaler):

| Value | Divisor | Value | Divisor |
|---|---|---|---|
| 0 | stopped | 4 | /50 |
| 1 | /4 | 5 | /64 |
| 2 | /10 | 6 | /100 |
| 3 | /16 | 7 | /200 |
| | | 8 | event count (A and B only) |

- Timer A: `TACR`, data `TADR`
- Timer B: `TBCR`, data `TBDR`
- Timer C: `TCDCR` bits 6–4, data `TCDR`
- Timer D: `TCDCR` bits 2–0, data `TDDR`

**Period** = `reload × prescale ÷ 2457600` seconds, where a reload of 0 means
256. Reading a data register while running returns the live counter; while
stopped it returns the reload value.

```c
/* ~10 ms tick: 200 prescale x 123 = 24600 cycles / 2.4576 MHz */
MMIO8(MFP_TDDR)  = 123;
MMIO8(MFP_TCDCR) = (MMIO8(MFP_TCDCR) & 0x70) | 7;   /* timer D, /200 */
```

> **Do not pick a tick faster than your handler.** A /4 prescale with a small
> reload gives a ~13 µs period, which is shorter than the interrupt handler
> takes to run — the machine livelocks and no foreground progress is ever made.
> This is real hardware behaviour, not an emulator artefact. Something in the
> **1–10 ms** range is sane for a scheduler tick.

**Event-count mode** (value 8, timers A and B): the timer counts active edges on
TAI (GPIP4) or TBI (GPIP3) instead of the clock. On this board those pins carry
the ATA and ethernet interrupts, so timer A can literally count disk interrupts.

### GPIP

8 bidirectional pins. `DDR` bit set = output. `AER` bit set = interrupt on the
**rising** edge, clear = falling. Reading `GPIP` returns the output latch for
output pins and the live pin for input pins — which makes it a convenient way to
see a peripheral's interrupt line directly.

### USART

Asynchronous transfer against the second serial port.

| Register | Bits |
|---|---|
| `TSR` | 0 = TE (enable), 4 = END, 7 = BE (buffer empty) |
| `RSR` | 0 = RE (enable), 6 = OE (overrun), 7 = BF (buffer full) |
| `UDR` | data; reading clears BF |

```c
MMIO8(MFP_TSR) = 0x01;                       /* enable transmitter */
while (!(MMIO8(MFP_TSR) & 0x80)) { }         /* wait for buffer empty */
MMIO8(MFP_UDR) = c;
```

Not modelled: synchronous modes, the sync character register, break generation,
parity. Pulse-width timer modes (control values 9–15) are treated as the
equivalent delay mode.

---

## 6. NS16550A UART — `0xff000000`

Byte registers at consecutive addresses, exactly as the datasheet numbers them.

| Off | Read | Write |
|---|---|---|
| 0 | RBR receive | THR transmit |
| 1 | IER | IER |
| 2 | IIR | FCR |
| 3 | LCR | LCR |
| 4 | MCR | MCR |
| 5 | LSR | — |
| 6 | MSR | — |
| 7 | SCR | SCR |

With `LCR` bit 7 (DLAB) set, offsets 0 and 1 become the divisor latch.

```c
void uart_init(void)
{
    MMIO8(UART_IER) = 0x00;
    MMIO8(UART_LCR) = 0x80;      /* DLAB */
    MMIO8(UART_DLL) = 0x01;
    MMIO8(UART_DLM) = 0x00;
    MMIO8(UART_LCR) = 0x03;      /* 8N1, DLAB off */
    MMIO8(UART_FCR) = 0x07;      /* FIFOs on, cleared */
    MMIO8(UART_MCR) = 0x03;      /* DTR + RTS */
}

void uart_putc(char c)
{
    while (!(MMIO8(UART_LSR) & 0x20)) { }    /* THRE */
    MMIO8(UART_THR) = c;
}
```

Useful bits: `LSR` 0 = data ready, 5 = transmit holding empty, 6 = transmitter
empty. `MCR` bit 4 = **local loopback**, which ties TX to RX inside the chip and
is the easiest way to test the interrupt path without external input.

For interrupts: set `IER` bit 0 (receive data available), clear `FCR` to get one
interrupt per byte, and service via MFP channel 7.

---

## 7. ATA disk — `0xff100000` / `0xff101000`

The command block is the standard eight-register taskfile. Offset 0 is the
**16-bit data register**; offsets 1–7 are byte registers.

| Off | Read | Write |
|---|---|---|
| 0 | data (16-bit) | data (16-bit) |
| 1 | error | features |
| 2 | sector count | sector count |
| 3 | LBA low | LBA low |
| 4 | LBA mid | LBA mid |
| 5 | LBA high | LBA high |
| 6 | device/head | device/head |
| 7 | status | command |

Control block at `0xff101000`: read = alternate status (does **not** clear the
interrupt), write = device control (bit 1 = nIEN, set to mask interrupts).

Status bits: `0x80` BSY, `0x40` DRDY, `0x20` DF, `0x08` DRQ, `0x01` ERR.
Commands: `0xEC` IDENTIFY, `0x20` READ SECTORS, `0x30` WRITE SECTORS.

```c
int ata_read(u32 lba, u8 *dst)          /* one sector, polled PIO */
{
    while (MMIO8(ATA_ALTSTAT) & 0x80) { }            /* wait BSY clear */
    MMIO8(ATA_DEVICE) = 0xE0 | ((lba >> 24) & 0x0f); /* LBA, master */
    MMIO8(ATA_NSECT)  = 1;
    MMIO8(ATA_LBAL)   = lba & 0xff;
    MMIO8(ATA_LBAM)   = (lba >> 8) & 0xff;
    MMIO8(ATA_LBAH)   = (lba >> 16) & 0xff;
    MMIO8(ATA_COMMAND) = 0x20;
    while (!(MMIO8(ATA_ALTSTAT) & 0x08)) { }         /* wait DRQ */
    for (int i = 0; i < 256; i++) {
        u16 w = MMIO16(ATA_DATA);
        dst[i*2 + 0] = w >> 8;                       /* see byte order below */
        dst[i*2 + 1] = w & 0xff;
    }
    return 0;
}
```

### ATA byte order

The data register needs care, and **byte streams and word values want
opposite treatment**.

QEMU's MMIO IDE region is `DEVICE_LITTLE_ENDIAN`, so a 16-bit read on this
big-endian CPU comes back byte-swapped relative to ATA's own word value.

- **Sector data is a byte stream.** The two bytes of each transferred word,
  stored **high byte first**, are exactly the two bytes that sit on the media
  in that order. A plain big-endian store reproduces the disk contents
  verbatim — no swap.
- **`IDENTIFY` returns 16-bit values.** Those *do* need swapping, or a
  capacity of 16384 sectors reads back as 64. Having swapped them into ATA's
  native word order, the model string's first character is the high byte of
  each word.

Getting this wrong is nastier than it looks, because **a write-then-read-back
test cannot detect it**. A driver that swaps on the way out and swaps again on
the way in is perfectly self-consistent while writing a byte-swapped image to
the media — which only shows up the first time something else has to read the
disk, such as a boot ROM loading a kernel the host `dd`'d into place. The only
check that catches it from inside the guest is reading bytes somebody else
wrote; `tests/t3-ata.c` does that against a signature the harness plants in
the image before boot.

Interrupts arrive on MFP channel 6. Clear nIEN (`DEVCTL = 0`) to enable them,
and read the **status** register (not alternate status) to drop the line.

---

## 8. LAN91C111 ethernet — `0xff200000`

A 16-byte window whose meaning depends on the selected bank. Offset 14 is the
bank-select register and is visible from every bank. 16-bit register accesses
work naturally — no swapping needed.

| Bank | Offsets |
|---|---|
| 0 | 0 TCR, 2 EPH status, 4 RCR, 6 counter, 8 MIR, 10 RPCR |
| 1 | 0 config, 2 base, **4–9 MAC address**, 10 general, 12 control |
| 2 | 0 MMU command, 2 PNR / 3 allocation result, 4 FIFO, 6 pointer, 8 data, 12 interrupt |
| 3 | 0 multicast, 8 management, **10 revision** (`0x3391`), 12 early receive |

Key bits: `TCR` 0x0001 TXENA, 0x0080 PAD_EN; `RCR` 0x0100 RXEN, 0x0200 STRIP_CRC.
Pointer register: 0x8000 receive area, 0x4000 auto-increment, 0x2000 read.
MMU commands: 0x20 allocate TX, 0x40 reset, 0x60 remove RX, 0x80 release,
0xC0 enqueue.

There are **no descriptor rings** — packets live in the chip's own memory and
move through a data port. That is what makes this chip far easier than a SONIC
or a LANCE.

### Transmit

```c
smc_bank(2);
MMIO16(SMC_B2_MMUCMD) = 0x0020;                  /* allocate a TX buffer */
while (!(MMIO8(SMC_B2_INT) & 0x08)) { }          /* wait for ALLOC */
MMIO8(SMC_B2_PNR) = MMIO8(SMC_B2_PNR + 1);       /* use the allocated packet */
MMIO16(SMC_B2_PTR) = 0x4000;                     /* write, auto-increment */
MMIO8(SMC_B2_DATA) = 0; MMIO8(SMC_B2_DATA) = 0;  /* status word */
MMIO8(SMC_B2_DATA) = (len + 6) & 0xff;           /* byte count, lo then hi */
MMIO8(SMC_B2_DATA) = ((len + 6) >> 8) & 0xff;
for (i = 0; i < len; i++) MMIO8(SMC_B2_DATA) = frame[i];
MMIO8(SMC_B2_DATA) = 0;                          /* pad (even length) */
MMIO8(SMC_B2_DATA) = 0;                          /* control byte, ODD clear */
MMIO16(SMC_B2_MMUCMD) = 0x00C0;                  /* enqueue */
```

Byte count includes 6 bytes of overhead. For an **odd**-length frame use
`len + 5` and set bit 5 (ODD) in the trailing control byte.

### Receive

```c
while (!(MMIO8(SMC_B2_INT) & 0x01)) { }          /* RCV */
MMIO16(SMC_B2_PTR) = 0x8000 | 0x4000 | 0x2000;   /* rx, auto-inc, read */
(void)MMIO8(SMC_B2_DATA); (void)MMIO8(SMC_B2_DATA);   /* status word */
len  = MMIO8(SMC_B2_DATA);
len |= MMIO8(SMC_B2_DATA) << 8;
for (i = 0; i < len; i++) buf[i] = MMIO8(SMC_B2_DATA);
MMIO16(SMC_B2_MMUCMD) = 0x0060;                  /* remove + release */
```

Interrupts arrive on MFP channel 3.

---

## 9. SM501 video — `0xf0000000` / `0xff400000`

Two windows:

- **Video memory** at `0xf0000000`, **16 MiB**, ordinary linear RAM in the CPU's
  own big-endian order. A framebuffer driver is little more than writing pixels
  here.
- **Control registers** at `0xff400000`. These are **little-endian and accept
  32-bit accesses only**.

```c
static inline u32 sm501_bswap32(u32 v)
{
    return ((v & 0x000000ff) << 24) | ((v & 0x0000ff00) << 8) |
           ((v & 0x00ff0000) >> 8)  | ((v & 0xff000000) >> 24);
}
#define SM501_RD(a)     sm501_bswap32(MMIO32(a))
#define SM501_WR(a, v)  (MMIO32(a) = sm501_bswap32((u32)(v)))
```

Useful registers (offsets from `0xff400000`):

| Off | Register |
|---|---|
| `0x000000` | system control |
| `0x000004` | misc control |
| `0x000010` | DRAM control |
| `0x000060` | device ID — reads **`0x050100A0`** |
| `0x080000` | panel display control |

Reading the device ID is the cheapest way to confirm the driver's endian
handling is right: through `SM501_RD` it is `0x050100A0`; a raw `MMIO32` gives
`0xA0000105`.

### The 2D drawing engine — `0xff500000`

Same access rules: little-endian, 32-bit only. Registers are at offsets from
`0xff500000`:

| Off | Register |
|---|---|
| `0x04` | destination, `(x << 16) | y` |
| `0x08` | dimension, `(width << 16) | height` |
| `0x0C` | control — **writing bit 31 starts the operation** |
| `0x10` | pitch, `(dst << 16) | src`, in pixels |
| `0x14` | foreground colour |
| `0x1C` | stretch — bits 21–20 are the pixel format (0 = 8bpp) |
| `0x44` | destination base, a byte offset into video memory |

Command number goes in bits 20–16 of control: **0 = BitBlt, 1 = Rectangle
Fill**. Clearing a 640×480 8bpp buffer:

```c
SM501_WR(SM501_2D_DST_BASE, fb_offset);
SM501_WR(SM501_2D_DEST, 0);                             /* x = y = 0     */
SM501_WR(SM501_2D_DIMENSION, (640UL << 16) | 480);
SM501_WR(SM501_2D_PITCH, (640UL << 16) | 640);
SM501_WR(SM501_2D_FOREGROUND, colour);
SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);          /* XY addressing */
SM501_WR(SM501_2D_CONTROL, SM501_2D_START | SM501_2D_CMD_RECTFILL);
```

Seven register writes instead of 76,800 CPU stores. In the cube demo this
raised the whole-frame rate by **5.9x**, because the clear was almost the
entire cost of a frame.

Bits 19–16 of the stretch register must be zero; anything else selects linear
rather than XY addressing, which is not modelled. The operation is synchronous
from the guest's point of view — by the time the control write returns, the
fill has happened.

---

## 10. The 68040 MMU

Fully available. The 68030 has **no MMU at all** in this emulator, which is why
the machine is a 68040.

- **Transparent translation**: `ITT0`/`ITT1` for instruction fetch,
  `DTT0`/`DTT1` for data. Format: bits 31–24 logical base, 23–16 mask (a set bit
  means *ignore*), bit 15 enable, bits 14–13 S-field (`00` user only, `01`
  supervisor only, `1x` both), bit 2 write-protect.
- **Root pointers**: `SRP` for supervisor accesses, `URP` for user.
- **`TC`**: bit 15 enable, bit 14 selects 8 KB pages (clear = 4 KB).
- Access via `movec`; flush with `pflusha`; probe with `ptest`.

### 4 KB page table layout

```
root index    = (va >> 25) & 0x7f     128 entries, 512-byte table, 512-aligned
pointer index = (va >> 18) & 0x7f     128 entries, 512-byte table, 512-aligned
page index    = (va >> 12) & 0x3f      64 entries, 256-byte table, 256-aligned
offset        =  va & 0xfff
```

Descriptors: table entries need bit 1 set (use `| 0x02`); page descriptors need
bits 1–0 non-zero (use `| 0x01`). Bit 2 is write-protect, bit 7 supervisor-only.
One page table covers 256 KB.

### Bringing it up safely

Make instruction fetch and the I/O block transparent first, so code and console
keep working no matter what the tables say, then put everything else through the
tables:

```c
set_itt0(0x00FFC000);   /* base 0x00, mask 0xff => matches everything */
set_dtt0(0xFF00C000);   /* base 0xff, mask 0x00 => the I/O block only */
set_itt1(0); set_dtt1(0);
build_tables();
set_srp(ROOT_TABLE); set_urp(ROOT_TABLE);
pflusha();
set_tc(0x8000);         /* enable, 4 KB pages */
pflusha();
```

Remember the stack and your globals are data accesses — they must be mapped
before you switch on, or the first push after `set_tc` faults.

---

## 11. Gotchas

**Endianness differs per device.** The CPU, RAM and SM501 video memory are
big-endian. All SM501 **control registers** are little-endian. The MFP, UART
and LAN91C111 need no swapping. The ATA **data register** is the subtle one —
see the byte-order note in §7: sector bytes need no swap, `IDENTIFY` word
values do.

**Access width is enforced.** SM501 control registers accept **32-bit only**;
the MFP and UART are **byte** registers. A wrong-width access to a QEMU device
can land in the wrong byte lane and produce **no output and no error at all** —
when a new driver is silent, suspect access width before anything else.

**Empty delay loops vanish at `-O2`.** `for (i = 0; i < 100000; i++) {}` is
deleted outright. Increment a `volatile` inside the loop.

**Don't out-run your interrupt handler.** See the timer warning in §5.

**A disabled MFP channel loses interrupts**, it does not defer them.

**Reading ATA alternate status does not clear the interrupt.** Read the real
status register.

---

## 12. Debugging

```bash
# add to the QEMU line:  -S -gdb tcp::1234
m68k-elf-gdb kernel.elf \
    -ex 'target remote :1234' \
    -ex 'break main' \
    -ex 'continue'
```

| Command | Use |
|---|---|
| `info registers` | D/A registers, PC, SR |
| `info all-registers` | adds FPU registers |
| `x/20i $pc` | disassemble |
| `stepi` / `nexti` | single-step |
| `x/4xw 0xff300000` | peek at a device |

`p $vbr` returns `void` — GDB's m68k description does not expose the control
registers. Read them from the guest instead.

QEMU logging: `-d unimp,guest_errors -D qemu.log` catches accesses to
unimplemented registers, which is often the fastest explanation for a driver
that does nothing. Note that `-d int` does **not** log m68k interrupts.

---

## 13. Running at period speed

By default QEMU executes as fast as the host allows, which for this machine is
roughly 150x a real 68040. `-icount` fixes that: it charges every guest
instruction a fixed 2^N nanoseconds, so the machine runs at a chosen
instruction rate **and virtual time becomes deterministic**.

```bash
qemu-system-m68k -M sage040 -cpu m68040 -m 4 \
    -icount shift=6,sleep=on -kernel kernel.elf ...
```

| shift | instructions/sec | roughly |
|---|---|---|
| 4 | 62.5 M | far beyond any 68k |
| 5 | 31.2 M | faster than any 68040 shipped |
| **6** | **15.6 M** | **a 25 MHz 68040** |
| 7 | 7.8 M | a 68020 |
| 8 | 3.9 M | an 8 MHz 68000 |

`sleep=on` makes QEMU wait so virtual time tracks the wall clock — what you
want to watch something run. `sleep=off` lets it finish sooner while keeping
virtual time correct, which is what you want for scripted measurement.

### Which speed is period-correct

The MC68040 launched in 1990 at **25 MHz**; **33 MHz** and **40 MHz** followed.
25 MHz is the canonical figure — it is what shipped in the Macintosh Quadra
700 and 900, the Amiga 4000/040 and the HP 9000/380. The Quadra 950 and
NeXTstation Turbo ran 33 MHz, the Quadra 840AV 40 MHz. Motorola quoted about
20 MIPS for the 25 MHz part.

`shift` is a power of two, so 25 MHz (shift 6) lands nicely and 33 MHz does
not; there is no finer control.

### What this does and does not model

It is a **reproducible order-of-magnitude model, not cycle accuracy**:

- **Every instruction costs the same.** A `divs.l` is charged like a `nop`,
  though on real silicon it is tens of cycles. Divide- and FPU-heavy code is
  therefore modelled optimistically.
- **No caches and no memory latency.** The 68040's 4 KB instruction and 4 KB
  data caches, and the cost of missing them into 1990s DRAM, are absent. Code
  that streams through memory — clearing a framebuffer, say — is modelled as
  instruction-bound when on real hardware it would be bandwidth-bound.
- **Device registers are nearly free**, which flatters anything that offloads
  work to a peripheral.

The errors run in both directions, so do not quote it as a hardware
measurement. What it is genuinely good for: making timing **deterministic and
host-independent**, so a benchmark repeats exactly, and putting the machine in
the right performance regime to judge whether a design decision matters. The
cube demo is a concrete example — at host speed clearing the framebuffer by
CPU looked merely slow, and at 25 MHz-equivalent speed it cannot hold the
frame rate at all.

---

## 14. Quick reference

```
CPU          MC68040, on-chip FPU and MMU, big-endian
RAM          0x00000000, -m (default 4 MB)
Vectors      0x00000000 (256 x 4), code at 0x00000400
Stack        top of RAM - 16, set before entry

UART         0xff000000   byte regs        MFP ch 7
ATA cmd      0xff100000   byte + 16-bit    MFP ch 6   data reg LITTLE endian
ATA ctl      0xff101000   byte
Ethernet     0xff200000   byte + 16-bit    MFP ch 3
MFP          0xff300000   byte regs        drives IPL 6, vectored
SM501 regs   0xff400000   32-bit LITTLE endian
SM501 VRAM   0xf0000000   16 MiB, big-endian

MFP vector   (VR & 0xF0) | channel
MFP clock    2.4576 MHz, prescalers 4/10/16/50/64/100/200
```

Worked, tested code for every device is in [`tests/`](tests/) — `t6` for the
interrupt chain, `t7`/`t8`/`t9` for the MFP, `t3` for ATA, `t4` for ethernet,
`t5` for the MMU, `t10` for video.
