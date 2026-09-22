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
| Clock + NVRAM | ST **M48T59** TIMEKEEPER | *M48T59* (STMicroelectronics) |

---

## Which kind of code are you writing?

There are two, and almost everything in this guide is about the first.

**Bare metal.** Your code *is* the machine: loaded by `-kernel` or by the boot
ROM, entered in supervisor mode with interrupts masked, and it owns every
register in the tables below. The test suite, the boot ROM and `cube/` are all
like this. Sections 1–14 are written for you.

**A program.** The kernel is already running, owns the hardware, and you reach
it through `trap #0`. You get file descriptors, a filesystem, a terminal, a
framebuffer and a clock, and you touch no registers at all. `system/` and
`apps/` are like this. **Section 15** is written for you, and the rest of
this guide is then background rather than instruction.

The difference is not a matter of taste, and it is no longer a matter of
discipline either. **The MMU is on and programs run unprivileged**, so a
program that pokes a register does not quietly work — it takes a bus
error and is killed, and the shell says so and carries on. This used to
say the opposite, which was true when it was written and stopped being
true when user mode arrived.

Two things enforce the split: the hardware, as above, and the include
paths — `lib/program.mk` does not put the hardware header on the path.

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

> **Match the serial option to how you are driving it.** Interactively,
> `-serial stdio` is fine. For a *scripted* session `-serial mon:stdio` does
> not forward piped stdin at all — use
> `-chardev stdio,id=con,signal=off -serial chardev:con`. For capture only,
> `-serial file:`.

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
| `0xff600000` | 8 KiB | M48T59 clock + NVRAM | byte | — |
| `0xff700000` | 4 KiB | Intel 8042 keyboard | byte | — |

**Endianness is not uniform, and this is the single biggest source of bugs.**
See §10.

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
| GPIP2 | 2 | M48T59 | alarm and watchdog |
| GPIP1 | 1 | Intel 8042 | keyboard output buffer full |

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

## 9. M48T59 clock and NVRAM — `0xff600000`

*Reference: STMicroelectronics M48T59 datasheet.*

One 8 KiB SRAM window. Everything below `0x1ff0` is ordinary
battery-backed RAM; the top sixteen bytes are the alarm, the watchdog and
the clock:

```
0xff601ff0  flags (read-only)      0xff601ff8  control
0xff601ff2  alarm seconds          0xff601ff9  seconds (bit 7 = ST, stop)
0xff601ff3  alarm minutes          0xff601ffa  minutes
0xff601ff4  alarm hours            0xff601ffb  hours (24-hour)
0xff601ff5  alarm date             0xff601ffc  day of week
0xff601ff6  interrupts             0xff601ffd  date
0xff601ff7  watchdog               0xff601ffe  month
                                   0xff601fff  year (two digits)
```

**Byte accesses only, and every time field is BCD.** There is no century
register, so the board supplies it: `RTC_BASE_YEAR` is 2000 and the part
covers 2000–2099.

The datasheet's protocol is to set **R** (bit 6 of control) before reading
the seven time bytes and clear it afterwards, so the clock cannot advance
mid-read, and to set **W** (bit 7) before writing them and clear it after,
which is what transfers them into the counters.

**Neither bit does anything under QEMU.** The model reads live from the
host clock and applies each write as it happens. Two things follow, both
measured and both covered by `t11-rtc`:

- A read can straddle a carry, so read the clock twice and repeat if the
  seconds moved. That loop is what actually gives a consistent reading
  here, and costs nothing on hardware where R works.
- Each field is applied through a normalising conversion, so the obvious
  write order passes through dates that do not exist. Writing 29 February
  while the clock still holds a non-leap year silently produces 1 March.
  Write the day as 1 first, then the year, then the month, then the real
  day, so every intermediate state is a date that exists.

The day-of-week register is not worth reading: nothing keeps it consistent
with the date, and QEMU numbers it from 0 while the datasheet numbers it
from 1. Derive the weekday from the date instead.

Presence cannot be tested from the time registers — a dead bus reads as
zeroes and zeroes are a legal-looking BCD midnight. Write and read back an
NVRAM byte instead, and put it back.

---

## 10. SM501 video — `0xf0000000` / `0xff400000`

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

Seven register writes instead of 76,800 CPU stores, and the clear is
almost the entire cost of a frame. In the cube demo it raises the
whole-frame rate by **5.9x at host speed and 11.3x at a period-correct
25 MHz** — the slower the CPU, the more the blitter is worth, which is
the opposite of how it is usually quoted. See §14 for why the
period-speed figure is the meaningful one.

Bits 19–16 of the stretch register must be zero; anything else selects linear
rather than XY addressing, which is not modelled. The operation is synchronous
from the guest's point of view — by the time the control write returns, the
fill has happened.

---

## 11. The 68040 MMU

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

## 12. Gotchas

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

## 13. Debugging

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

## 14. Running at period speed

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

## 15. Writing a program

Everything above is about owning the machine. This section is about not
owning it: the kernel is running, it owns the hardware, and a program
reaches it through `trap #0`.

See [`apps/`](apps/) and [`system/`](system/) for working examples — `hello.c` is thirty lines,
`fbtest.c` draws one of everything, `cube.c` is a real one.

### The system call interface

The convention is **Linux/m68k's, unchanged**:

```
d0 = call number
d1, d2, d3, d4, d5 = arguments
      trap #0
d0 = result, or a negated errno
```

That is not an imitation. Linux picked the obvious convention for this
architecture and there is nothing to improve on. The numbers are
**Linux/m68k's**, from `arch/m68k/kernel/syscalls/syscall.tbl`, so
`__NR_write` is 4, and the error values are Linux's, so `-ENOENT` is
`-2`. `kernel/abicheck.sh` checks every number in `uapi.h` against that
table on each build -- the socket calls were i386's, all three too high,
until a C library built on the real table called the wrong ones.

The calls this system has always had:

| | |
|---|---|
| `exit` 1, `read` 3, `write` 4, `open` 5, `close` 6 | the usual |
| `unlink` 10, `rename` 38, `stat` 106, `getdents` 141 | files |
| `lseek` 19, `fsync` 118, `sync` 36, `statfs` 99 | more files |
| `mkdir` 39, `rmdir` 40, `chdir` 12, `getcwd` 183 | directories |
| `time` 13, `stime` 25, `times` 43, `nanosleep` 162 | time |
| `getpid` 20, `kill` 37, `waitpid` 7, `sched_yield` 158 | tasks |
| `socket` 356 … `shutdown` 370 | sockets: `socketpair`, `accept4`, the socket options, names, `sendmsg`/`recvmsg` |
| `ioctl` 54, `uname` 122, `sysinfo` 116, `reboot` 88 | the rest |
| `spawn` 1000, `jobctl` 1001, `netctl` 1002 | **not Linux** — see below |

Beside them are the rest of Linux's interface, the calls a C library
makes -- `statx`, `getdents64`, `openat` and the other `*at` calls,
`rt_sigaction` with `SA_SIGINFO`, `pipe2`, `dup3`, `wait4`,
`clock_gettime`, `_llseek`, `prlimit64`, `getrandom` and the uid calls
-- in `kernel/syslinux.c`. `stat` (106), `fstat` (108) and `getdents`
(141) have Linux's numbers and this system's own simpler structures;
`statx` and `getdents64` are the Linux-shaped ones.

The three at 1000 are local because Linux has nothing to match. They
were at 400-402 until it turned out Linux/m68k gives those to `msgsnd`,
`msgrcv` and `msgctl`.
`spawn` takes a path and an argument vector and creates a task directly,
the direct alternative to `fork` + `execve` -- though `fork` shares its
pages copy-on-write, so the pair is cheap too. `jobctl` is what `fg`, `bg`,
`jobs` and `ps` are built on; `netctl` is what `ifconfig`, `ping` and
`netstat` are built on, being the operations that configure an interface
or send one echo request and so have no socket to hang off.

There is **no global `errno`**: a call returns a non-negative result or
the negated error. A global would only start making sense once there are
threads to get it wrong.

`spawn` is numbered clear of Linux's range because Linux has no such
call. `fork` (2), `execve` (11) and `waitpid` (7) are the real ones, and
`waitpid`'s status is Linux's encoding: use `WIFEXITED`, `WEXITSTATUS`,
`WIFSIGNALED`, `WTERMSIG`, `WIFSTOPPED` from `uapi.h`. To run a command
the way `system()` does, `fork`, then `execvp("sh", {"sh", "-c", cmd})`,
then `waitpid`.

### Two ways to write one

**Against picolibc**, a real C library, for anything written the way
programs are written elsewhere: `<stdio.h>`, `printf`, `malloc`,
`fork`, `opendir`, `sigaction`. `make libc` once, then a Makefile of

```make
TOPDIR := ..
PROGS  := myprog
include $(TOPDIR)/libc/libc.mk
```

`libc/test/` is the example, and `libc/README.md` lists what differs
from glibc -- mostly that `environ` must be declared by the program and
`CLOCK_MONOTONIC` wants `_GNU_SOURCE`.

That builds a **static** program, which runs on any disk. `LINK=dynamic`
(or naming the target `myprog.dyn`) links it against `/lib/libc.so`
instead -- see *Shared libraries* below.

**Against `lib/ulib`**, this system's own few hundred lines of wrappers,
for the programs in `apps/` and `system/`: small, no stdio, and the
system's own calls such as `spawn`. The rest of this section is about
that one.

### What a program includes

Two headers, and only two:

```c
#include "ulib.h"        /* the library: syscall stubs, strlen, puts... */
```

which pulls in `kernel/uapi.h`, the ABI — call numbers, `O_CREAT`,
`struct stat`, the ioctl numbers. A program does **not** get `kernel.h`,
`vfs.h`, `dev.h`, anything under `drivers/`, or the machine's hardware
header. `lib/program.mk` deliberately leaves `tests/` off the include
path, so reaching a chip means editing a Makefile rather than adding an
`#include` — which makes it a decision instead of a slip.

### Building one

Drop the source into `apps/` (or `system/`, if it is part of the system
rather than something somebody chose to run) and add its name to that
directory's `PROGS`. The rules live in one place, `lib/program.mk`, so
the two cannot drift apart:

```make
LIB := $(TOPDIR)/lib

CFLAGS  := -mcpu=68040 -ffreestanding -nostdlib -nostdinc -O2 \
           -Wall -Wextra -Werror -fno-builtin -fno-stack-protector \
           -I$(LIB) -I$(TOPDIR) -I$(TOPDIR)/kernel
LDFLAGS := -mcpu=68040 -ffreestanding -nostdlib -T $(LIB)/user.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

COMMON := $(LIB)/crt0.s $(LIB)/ulib.c $(LIB)/ulib.h $(LIB)/user.ld

myprog: myprog.c $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) \
	    -x assembler-with-cpp $(LIB)/crt0.s -x c $(LIB)/ulib.c $< -o $@
```

Note what is **not** on the include path: `tests/`, where the machine's
hardware header lives. A program cannot reach a chip by accident, and if
one ever needs to, the include path is what has to change — which makes
it a decision rather than a slip.

`lib/user.ld` links at **`0x10000000`**, not 1 MB, and the program area
is 256 MB with a 1 MB stack at the top. It has no vector table — a
program is entered at its ELF entry point, not found at a fixed address
by a ROM.

There is no flattening step. The kernel loads ELF directly, which is why
`make` produces something runnable.

### Getting it onto the disk, and running it

```bash
make install            # mcopy, uppercasing the name: myprog -> MYPROG
```

```
sage$ myprog one two
```

Anything the shell does not recognise as a builtin is looked up on the
disk and run. **Programs carry no extension** — the kernel decides what
is executable by reading the first four bytes of the file, not its name,
because a FAT16 volume has no execute permission bit to consult. A text
file named `CUBE.EXE` is still refused.

### Limits, the NVRAM, and interrupts

A task may have **64 descriptors** (`OPEN_MAX`, `EMFILE` past it); the
machine runs **64 tasks** (`fork` says `EAGAIN` when the table is full,
`ENOMEM` when memory is), holds **128 filesystem files** open at once
across all of them, 64 sockets and 32 TCP connections.

**`/dev/nvram`** is 8176 bytes that survive a reset: `open`, `read`,
`write`, `lseek`, nothing past the end (`ENOSPC`). `/bin/nvram` keeps
`KEY=VALUE` settings there -- `nvram net.ip=10.0.0.5`, `nvram net.ip`,
`nvram -d KEY` -- and `ifconfig nvram` configures the interface from
`net=dhcp` or `net.ip`, `net.mask`, `net.gw`. Use the program's format
if other programs are to read what you store.

**`/bin/irqs`** shows interrupts taken per MFP channel, and whether the
disk's waits slept or polled -- the `kstat(KSTAT_IRQ, ...)` call (1005),
which, like `memctl`, takes the size of the caller's structure.

### Memory is demand paged, and there can be swap

A program's stack, heap and anonymous `mmap`s take no memory until they
are touched: `mmap` of 32 MB costs its page tables, and each page comes
into being, zeroed, the first time it is used. `mmap` still refuses
what memory and swap together could never supply. `fork` shares every
page copy-on-write.

With a swap file, memory can be overcommitted past RAM:

```
sage$ swapon /swap          # an existing file, at its full size
sage$ free                  # shows the swap line
sage$ swapoff /swap         # brings every page back first, or refuses
```

Make the file on the host (`dd` of zeroes, then `mcopy`); the kernel
refuses to use a file it would have to grow, and while it is on the
file cannot be written, truncated, renamed or deleted -- `ETXTBSY`.
A program that touches more than memory and swap can hold is killed
(SIGKILL, status 137) when it touches a page there is no room for.

`memctl(MEMCTL_STATS, sizeof m, &m)` (1004) reports the counters --
faults, pages in and out, swap in use. Pass the size of YOUR structure:
it has grown, and the kernel writes no more than it is told.

### Networking from picolibc

A picolibc program has the BSD socket API and `getaddrinfo` --
`<sys/socket.h>`, `<netinet/in.h>`, `<netdb.h>` -- so a network program
ported from Linux builds as it is, IPv4-only (`libc/README.md`, *The
network layer*). Names resolve through `/etc/hosts` and DNS, with a
per-process cache that honours TTLs. lib/ulib's `resolve_host()` is the
same lookup for the system's own programs.

### Shared libraries

A program built against picolibc can link against **`/lib/libc.so`**
rather than carry its own copy of the library. It is then a fifth of
the size -- libctest is 23 KB on disk instead of 118, 18 KB of its own
in memory instead of 72 -- and, what
matters more, **every program running shares one copy of libc's text in
memory**: the kernel hands each process the same physical pages
(`kernel/textcache.c`), and only libc's data is per process.

```make
LINK := dynamic
include $(TOPDIR)/libc/libc.mk
```

It works the way it does on Linux, deliberately, and nothing about it is
this system's own invention:

- The program names an interpreter, **`/lib/ld.so`** (`ldso/`), in a
  `PT_INTERP` header. The kernel loads both and starts the interpreter,
  which loads each `DT_NEEDED` library -- from `LD_LIBRARY_PATH`, then
  `/lib` -- relocates everything, runs the libraries' constructors, and
  jumps to the program.
- **Every symbol is bound before `main`** (there is no lazy binding). A
  missing library or a missing symbol is refused at start, exit status
  127, with a message saying which -- never a crash half way through.
- `LD_TRACE_LOADED_OBJECTS=1` makes a program list its libraries and
  stop, as `ldd` does elsewhere.
- Lookup is the ELF rule: the program first, then the libraries in load
  order, first definition wins.

**Writing a library of your own:**

```make
libfoo.so: foo.c
	$(CC) $(CFLAGS) -fPIC -mcpu=68040 -nostdlib -Wl,-shared \
	    -Wl,-soname,libfoo.so -Wl,--hash-style=sysv -Wl,-z,now \
	    $< -L$(SAGE_LIBC)/lib -lc -o $@
```

`libc/test/Makefile` builds `libsot.so` exactly this way. Three things
there are not optional:

- **`-Wl,-shared`, not `-shared`.** This toolchain's gcc is the
  bare-metal `m68k-elf` one, and its driver passes neither `-shared` nor
  `-static` on to the linker. `-shared` alone quietly produces an
  executable.
- **`-fPIC`**, so the library's text holds no absolute addresses and can
  be shared. A library built without it still works -- ld.so applies its
  text relocations -- but each process then has its own copy of every
  page that needed one.
- **`--hash-style=sysv`**, which is the table `ld.so` reads.

A function's address is the same in the program and every library, as C
requires: when a program calls `printf`, the linker gives `printf` an
address in the program's own PLT, and `ld.so` hands that address to any
library that asks for `&printf`. (So `&printf` in a program is not an
address inside libc -- which only matters to a test that goes looking
for libc's pages, as `libc/test/sotest.c` explains.)

**Private memory.** A read-only page of a library is shared; making it
writable (`mprotect`) gives the process its own copy first, so no
process can ever change another's code. Library data is always private.

### The memory a program gets

**A program runs unprivileged, in an address space of its own.** It
begins at `0x10000000` and has 256 megabytes of virtual space. Every
program has the same addresses, because no two of them can see each
other: the numbers are virtual and the physical pages behind them come
from wherever the allocator had some.

```
0x10000000  your image: text, rodata, data, bss
            the heap, from the next page up, as far as brk() moves it
...         UNMAPPED -- a runaway stack faults here
0x1ff00000  your stack, 1 MB, growing down
0x1ffffff0  the top of it, where argc and argv were put
0x20000000  the end of everything you can reach
```

Outside that, nothing. Not the kernel, not the UART, not the
framebuffer, not video memory -- an access to any of them is a bus
error and the kernel kills the program with a segmentation fault. That
is not a convention to respect, it is a page table: `apps/faulter.c`
tries all of them and `kernel/vmtest.sh` checks that every one fails.

**The heap is `brk()` and `sbrk()`**, with Linux's meaning. It starts
on the page after your image and may grow to one page below the stack
-- about 255 MB, or as much as the machine has free, whichever is less.
New heap memory is always zero. `sbrk(n)` returns where the break
*was*, or `(void *)-1`; `brk(p)` returns 0 or `-ENOMEM`. Memory given
back by shrinking is unmapped, so touching it afterwards is a
segmentation fault rather than a quiet read of stale data.

**`mmap` works, with Linux's meaning** and one honest limit. Anonymous
memory is placed top down from just below the stack, so the heap and
your mappings share the gap without meeting. A file mapping is a
*copy* made when you call `mmap`: exact for `MAP_PRIVATE`; `MAP_SHARED`
is allowed only read-only and does not see later writes to the file;
`MAP_SHARED` with `PROT_WRITE` is refused with `-ENODEV` rather than
quietly not writing the file. `PROT_EXEC` means nothing because the
68040 cannot tell execute from read. `mprotect` works on any page you
own, including `PROT_NONE`. `mmap()` in `ulib` returns `MAP_FAILED`;
`syscall(__NR_mmap2, ...)` gives you the errno.

**`malloc`, `free`, `calloc` and `realloc` are in `lib/malloc.c`**,
declared through `ulib.h`. Blocks are 8-byte aligned. Requests of 128
KB or more get pages of their own from `mmap` and go back to the system
when freed. `realloc` grows a block in place when it can. A double free
or a pointer `malloc` never returned ends the program with status 134
and a message, rather than corrupting the heap. `malloc_check()` walks
the heap and verifies it, and `mallinfo()` reports on it with glibc's
field names. It is a stand-in: the C library will replace it.

Programs are linked with `--gc-sections`, so one that never allocates
does not carry the allocator.

**A pointer you pass to a system call is checked.** The kernel cannot
dereference your addresses -- they mean nothing in its own space -- so
it walks your page tables and copies. A bad pointer comes back as
`-EFAULT` and your program keeps running; it does not take the machine
with it.

**You cannot mask an interrupt, halt the processor, or touch a control
register.** Those instructions are privileged and attempting one is an
exception. The way to ask for anything the hardware can do is a system
call or an ioctl.



The kernel bounds-checks every `PT_LOAD` segment against the program
window before reading a byte of it — a mislinked program is refused
rather than loaded. That check is about *loading*; what protects the
machine while your code *runs* is the MMU, which is a separate thing and
is described above.

`.bss` is zeroed by the kernel, not by `crt0.s`: the loader knows which
part of a segment came from the file and the program does not.

`argc`, `argv` and `envp` arrive on the stack, below the return address
the `jsr` pushed. `crt0.s` picks them up at `4(%sp)`, `8(%sp)` and
`12(%sp)`, and stores the third in `environ` before calling `main` —
which is what makes `getenv()` work and what makes the shell's exported
variables visible to you. Getting those offsets off by one slot gives a
plausible-looking garbage `argc` and a bus error shortly after.

### Files

```c
int fd = open("NOTES.TXT", O_WRONLY | O_CREAT | O_TRUNC);
write(fd, "hello\n", 6);
close(fd);
```

`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_EXCL`, `O_TRUNC`,
`O_APPEND`, `O_CLOEXEC`, `O_NONBLOCK`, `O_DIRECTORY`, and `lseek` with
`SEEK_SET`/`SEEK_CUR`/`SEEK_END`. The access mode is the low two bits
the way POSIX has it, so **`O_RDONLY` is zero** and testing for it with
`&` does not work — use `(flags & O_ACCMODE)`.

The volume is FAT16 with subdirectories and **VFAT long names** -- up to
255 characters, stored as UTF-16 and handed to programs as UTF-8, case
preserved and looked up case-insensitively (in ASCII).
`chdir`, `getcwd`, `mkdir` and `rmdir` all work, a path may be absolute
or relative, and the working directory belongs to the task.

**`rename` is POSIX's**: it replaces a file that is already at the
destination -- which is how an editor saves, writing a temporary and
renaming it over the original -- and it moves between directories,
directories included. It refuses to move a directory into itself.

**A directory can be opened** read-only, and read with `getdents64`
(Linux's records, `.` and `..` included), which is what `readdir()` in
a C library is. `read` on one is `EISDIR`, as on Linux. lib/ulib's older
`getdents(index, &dirent)` walks the working directory by index and
returns `-ENOENT` when there are no more.

**Names** resolve with `resolve_host(name, &addr)` in lib/ulib: a dotted
quad, `/etc/hosts`, `localhost`, then DNS to each `nameserver` in
`/etc/resolv.conf` (`ADDRESS` or `ADDRESS#PORT`), or to the server DHCP
handed out if the file names none. It returns 0, `-ENOENT` for a name
that does not exist, `-ETIMEDOUT` when nobody answers. `host NAME` is
the command-line way to ask; `ntpdate [-q] [-p PORT] SERVER` sets the
clock over SNTP.

`ftruncate` and `truncate` cut a file or extend it with zeroes; `flock`
takes BSD advisory locks, shared or exclusive, held by the open file
description and released at its last close. A file can be open for
writing while other descriptors read it, and they see what is written.

**Shut the machine down, or it checks the disk next time.** Mounting
marks the volume in use and unmounting -- `halt`, `shutdown`, `reboot`
-- marks it clean; a machine reset or an emulator killed leaves it
marked, and the next boot runs the check and repairs what it finds.
`fsck` checks on demand, `fsck -y` repairs; repair is refused while a
file is open.

**Inode numbers are made up**, because FAT has none: a directory is its
first cluster, a file is where its entry sits. They are nonzero, stable,
and the same from `stat` and `readdir` -- but renaming a file moves its
entry and so changes its number.

A name that is already an upper-case 8.3 name is stored as one alone;
anything else also gets an 8.3 alias for DOS -- itself upper-cased if
that is free (`readme.txt` is `README.TXT`), otherwise `NAME~1`.
Trailing dots and spaces are dropped, as Windows and Linux's vfat do.

### The terminal

Descriptors 0, 1 and 2 are the console before your first instruction
runs, exactly as a shell would hand them over.

`read(0, ...)` returns **one whole line**, echoed as it was typed, with
backspace and ctrl-U already handled, and returns **0** at end of input —
ctrl-D on an empty line. That is canonical mode, done in the tty driver,
so every program does not implement line editing again slightly
differently.

A newline written to the terminal becomes CR+LF; a newline written to a
file stays a newline. Same `write()`, and the difference is the tty's,
which is what ONLCR means on a real system.

To ask whether a key is waiting without blocking on a read that may
never return:

```c
u32 n;
ioctl(STDIN_FILENO, FIONREAD, (u32)&n);   /* n = 0 or 1 */
```

`ulib.h` wraps that as `key_waiting()`, which is what `apps/cube.c`
actually calls.

#### Raw mode, if you want the keystrokes yourself

Canonical mode is the default because most programs want a line. A
program that wants characters as they arrive — an editor, a game, a
pager — clears `ICANON` and `ECHO` the way it would on Linux, with
Linux's `struct termios`:

```c
struct termios saved, raw;

ioctl(STDIN_FILENO, TCGETS, (u32)&saved);
raw = saved;
raw.c_lflag &= ~(u32)(ICANON | ECHO);
ioctl(STDIN_FILENO, TCSETS, (u32)&raw);

/* read() now returns as soon as anything has arrived */

ioctl(STDIN_FILENO, TCSETS, (u32)&saved);     /* put it back */
```

**Leave `ISIG` alone.** Clearing it looks like asking for every
keystroke and means ctrl-C stops working — the one thing a terminal
must never stop doing. readline does not clear it either.

Putting it back matters less than it looks: a program killed by ctrl-C
never gets the chance, so the kernel restores the terminal itself when a
program ends. Restore it anyway, because that will stop being true the
moment programs are not the only thing running.

`kernel/edit.c` is a worked example of all of this — it is the shell's
line editor, and nothing in it is privileged.

`tcgetattr(fd, &t)` and `tcsetattr(fd, TCSANOW, &t)` are in `ulib.h`
too, with POSIX's names; they are those two ioctls.

#### How big the terminal is

```c
struct winsize w;
ioctl(STDIN_FILENO, TIOCGWINSZ, (u32)&w);    /* w.ws_row, w.ws_col */
```

Linux's call and structure. What it reports is **the smallest of the
console's enabled outputs**, because a full-screen program has to fit on
all of them at once: the screen is 30x80 and says so, and the serial
line -- which cannot be measured -- is 24x80 until somebody sets it. So
with both on, a program is told 24x80; with `console fbcon off`, the
line's size; with `console ttyS0 off`, 30x80. Pixel sizes are filled in
only when one output decided both dimensions.

`TIOCSWINSZ` sets the **serial line's** size, not the answer directly.
`stty rows 40 cols 100` does it by hand, and `resize` asks the terminal
on the far end -- cursor to 999;999, then `ESC[6n` -- the way xterm's
resize(1) does. Setting the line to 40 rows with the screen enabled
still reports 30: the screen has 30.

**Handle `SIGWINCH`.** Any change to what `TIOCGWINSZ` would report --
from `TIOCSWINSZ`, or from an output being switched on or off -- sends
it to the terminal's foreground group, and only that group. Its default
is to be ignored. `TERM` is `vt102` in the shell's environment.

#### Arrow keys and other escape sequences

Extended keys arrive as VT100 escape sequences from **both** inputs:
`ESC [ A` through `ESC [ D` for the arrows, `ESC [ H` and `ESC [ F` for
Home and End, `ESC [ 3 ~` for Delete. The keyboard driver emits the same
bytes a serial terminal sends, so a program parses them once and does
not know or care which one it is reading.

#### ctrl-C and ctrl-Z

The terminal raises SIGINT and SIGTSTP on your program. By default
ctrl-C ends it with status 130, and it works even if you never call the
kernel at all, because the timer interrupt notices. Install a handler
with `signal()` or `sigaction()` to do something else. ctrl-Z
stops it, and `fg` resumes it from the system call it was in. A `read()`
interrupted by ctrl-C returns `-EINTR`, exactly as it would on Linux.

This used to carry the caveat that a program making no system calls could
not be stopped. It no longer applies to ctrl-C, which is delivered from
the tick as well as at the system call boundary -- `apps/spin` makes no
system calls at all and is still interruptible. ctrl-Z is different and
deliberately so: it is only ever delivered at a system call, because
stopping means being resumable and an interrupted instruction stream is
not.

#### Where the console is

This machine's console is several devices at once — output goes to the
screen and the serial line, input comes from the keyboard and the serial
line — so a program can ask which, and turn an output off:

```c
struct console_info ci;
int i;

for (i = 0; ; i++) {
    ci.which = CONS_SINK;           /* or CONS_SOURCE */
    ci.index = i;
    if (ioctl(STDIN_FILENO, TIOCGCONS, (u32)&ci) < 0) {
        break;                      /* -ENOENT past the end */
    }
    /* ci.name, ci.enabled */
}
```

`TIOCSCONS` takes a `struct console_set` — a name and an on/off — and
returns `-EBUSY` rather than turning off the last one, because a machine
with no console output cannot tell you why. A source cannot be turned
off at all.

Both are local, in the 0x54F0 block; Linux has no equivalent because it
picks its console at boot with `console=` rather than through a
descriptor. `shell.c`'s `console` command is the worked example.

#### The network

The socket interface is Linux's, signatures and all, so code written
for Linux compiles unchanged. A socket is a file descriptor, so `read()`
and `write()` work on one as well as `send()` and `recv()`.

```c
struct sockaddr_in sa;
int fd = socket(AF_INET, SOCK_STREAM, 0);

memset(&sa, 0, sizeof(sa));
sa.sin_family = AF_INET;
sa.sin_port   = htons(80);
inet_aton("10.0.2.2", &sa.sin_addr);

connect(fd, (struct sockaddr *)&sa, sizeof(sa));
write(fd, "GET / HTTP/1.0\r\n\r\n", 18);
while ((n = read(fd, buf, sizeof(buf))) > 0) { ... }   /* 0 = closed */
close(fd);
```

Everything else Linux has is here too: `accept4`, `getsockname`,
`getpeername`, `setsockopt`/`getsockopt`, `sendmsg`/`recvmsg`,
`shutdown`, `socketpair(AF_UNIX, SOCK_STREAM)`, non-blocking sockets and
`EINPROGRESS`, and the `MSG_*` flags. **`127.0.0.1` works**, and so does
the machine's own address, with or without a network. `apps/socktest.c`
exercises all of it over loopback.

Both travel on **`lo`**, an interface of its own -- 127.0.0.1/8, with
its own counters -- which `ifconfig` lists after `eth0`, and which
`ifconfig lo down` takes down (127/8 is then `ENETUNREACH`, as on
Linux). A frame arriving from the wire addressed to 127/8, or claiming
to come from it, is dropped, so a service bound to 127.0.0.1 really is
reachable only from this machine.

`bind()`, `listen()` and `accept()` work the other way round;
`apps/httpd.c` is a worked example that serves files off the disk, and
`apps/fetch.c` is the client side.

`htons()` and friends are the identity on this machine, because network
byte order is big-endian and so is a 68040. **Call them anyway** — the
habit is what makes the code portable and it costs nothing here.

UDP uses `sendto()` and `recvfrom()` with the same descriptor type.

**There is no resolver**, so addresses are numeric. That is the next
thing missing rather than an oversight.

**Blocking is a real sleep, and it lasts until it is satisfied**, as on
Linux. A timeout is `SO_RCVTIMEO` or `SO_SNDTIMEO`, which end a wait
with `EAGAIN`. Closing never waits: the connection finishes its FIN
handshake on its own.

#### Stopping the machine

`reboot(RB_POWER_OFF)` flushes the filesystem and stops the machine;
`reboot(RB_HALT_SYSTEM)` stops the processor and leaves it there.
`system/shutdown.c` is four lines around the first of those.

### The framebuffer

`/dev/fb0`, drawn with ioctls:

```c
struct fb_info info;
struct fb_line l;
int fb = open("/dev/fb0", O_RDWR);

ioctl(fb, FBIO_GETINFO, (u32)&info);      /* width, height, bpp, pitch */
ioctl(fb, FBIO_CLEAR, 0);
l.x0 = 0; l.y0 = 0; l.x1 = 639; l.y1 = 479; l.colour = 1;
ioctl(fb, FBIO_LINE, (u32)&l);
ioctl(fb, FBIO_FLIP, 0);                  /* show what you drew */
```

| ioctl | argument |
|---|---|
| `FBIO_GETINFO` | `struct fb_info *` out |
| `FBIO_SETMODE` | `struct fb_mode *` — 8 bpp only so far |
| `FBIO_POINT` | `struct fb_point *` |
| `FBIO_LINE` | `struct fb_line *` |
| `FBIO_RECT` | `struct fb_rect *`, `filled` 0 or 1 |
| `FBIO_CLEAR` | the colour, **by value** |
| `FBIO_FLIP` | none |
| `FBIO_SYNC` | none — wait for the blitter |
| `FBIO_PALETTE` | `struct fb_palette *`, `rgb` as `0x00RRGGBB` |
| `FBIO_COPY` | `struct fb_copy *` — move a rectangle, upward only |
| `FBIO_DOUBLE` | 1 or 0 **by value** — double buffering on or off |

Drawing through ioctl rather than through graphics system calls because
a framebuffer is a device and the device model already carries it.
Linux controls its framebuffer the same way, and then expects you to
`mmap` the memory and draw yourself. That works here too:

```c
u8 *vram = mmap(0, info.mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
u8 *px = vram + info.draw_offset;         /* the buffer being drawn */

px[y * info.pitch + x] = 3;               /* one byte a pixel, a palette index */
```

The mapping is the video memory itself, uncached, not a copy: it costs
no RAM beyond its page tables, a child made by `fork` shares it, and the
blitter's work is visible through it once `FBIO_SYNC` returns.
`FBIO_GETINFO` says how large video memory is (`mem_size`) and where
the buffer being drawn (`draw_offset`) and the one on screen
(`show_offset`) begin in it -- they change at every `FBIO_FLIP`, so
read them again after one. `apps/fbmap.c` is the example.

**It is double buffered.** Drawing goes to the buffer that is not on
screen; `FBIO_FLIP` swaps them with one register write, so the change
lands between frames rather than halfway down one. Nothing appears until
you flip.

Colours are palette indices. The driver sets up eight: 0 black, 1 green,
2 white, 3 red, 4 blue, 5 yellow, 6 cyan, 7 magenta.

Off-screen coordinates are clipped, not rejected — a shape that runs off
the edge is not an error.

### The text console

The screen can be a terminal instead of a canvas. `/dev/fbcon` is 80x30
of the IBM PC 8x16 font in green, and it is a **VT102**: cursor
addressing, erasing, insert and delete of lines and characters, scroll
regions, bold, underline, reverse and the ANSI colours, the DEC line
drawing set. Use `TERM=vt102`.

```c
int con = open("/dev/fbcon", O_WRONLY);
write(con, "\033[2J\033[10;30H\033[7m hello \033[m\r\n", 28);
```

Two things a program written on a PC console might not expect:

- **LF is a line feed and nothing else**, as on a VT102. `/dev/console`
  turns `\n` into `\r\n` (`ONLCR`) on the way to every sink, so a
  program writing through descriptor 1 never notices; one writing to
  `/dev/fbcon` directly gets exactly the bytes it wrote.
- **The last column wraps late.** Writing column 80 leaves the cursor
  there, and the wrap happens when the next character arrives (`xn` in
  terminfo). A full bottom line does not scroll.

The console answers "where is the cursor" (`ESC[6n`) and "what are you"
(`ESC[c`) only when the screen is the sole output. With the serial line
enabled too, the terminal on the other end answers, and a second reply
would be garbage in the program's input.

`/dev/vcsa` reads the screen back, as on Linux: four bytes (rows,
columns, cursor column, cursor row), then a character and an attribute
byte for every cell. The attribute byte is foreground in bits 0-2, bold
in 3, background in 4-6 and underline in 7, with reverse already applied
by swapping the colours. `ioctl(con, FBCON_REDRAW, 0)` draws the whole
console again from that buffer, which is what a program that drew over
it wants on its way out.

You rarely need to: `/dev/console` already writes to the screen **and**
the serial line at once. The terminal has a list of output sinks, and
`/dev/fbcon` is one of them, so `write(1, ...)` reaches both.

**It cannot be read from.** A screen is not an input device. Input comes
through the terminal, from whatever sources it has — the serial port
**and** the 8042 keyboard, both live at once — and a program never has to
know which: descriptor 0 works either way.

Note that writing to it turns double buffering **off**, because a console
draws a character at a time and each one has to appear. A program that
wants to animate afterwards must ask for it back with
`ioctl(fb, FBIO_DOUBLE, 1)`, which is why `apps/cube.c` does.

### Time

```c
time_t now = time(0);            /* seconds since 1970 */
struct timeval tv;
gettimeofday(&tv, 0);            /* the same clock, to the tick */
struct tms t;
u32 ticks = times(&t);           /* ticks since boot; t gets your user
                                    and system time, and your children's */
msleep(20);                      /* nanosleep, rounded up to a tick */
alarm(1);                        /* SIGALRM in a second */
setitimer(ITIMER_REAL, &it, 0);  /* or VIRTUAL, or PROF: Linux's three */
```

`time()`, `gettimeofday()` and the timestamps the filesystem writes all
read **one clock**: the RTC's second, taken at the moment it changes,
plus the ticks since. So they never disagree, and `gettimeofday()`'s
fraction is real, good to the 10 ms tick. `settimeofday()` sets the RTC
as well. Interval timers run at the tick's resolution, and one shorter
than a tick is rounded up to a tick rather than to nothing.

`HZ` is 100, so a tick is 10 ms and a 50 fps frame is exactly two of
them. It comes from `uapi.h`, not from a kernel header: `times()`
returns ticks, and a count of ticks means nothing without the rate, so
the rate crosses the boundary with it. Linux answers the same question
through `sysconf(_SC_CLK_TCK)`; there is no `sysconf` here.

A sleeping program is off the run queue until its time is up, so it
costs nothing and other programs run meanwhile. Prefer it to a delay
loop, which is only ever right on the machine it was tuned on.

### What a program can do that it once could not

Kept because this list used to say the opposite, and somebody reading an
older copy should know which way round it is now.

- **Run at the same time as another.** Each task has its own address
  space; several programs run at once. The single program area at 1 MB
  and the `-EBUSY` from a nested `spawn` are both gone.
- **Run in the background.** `&` and `bg` really run things, `jobs`
  lists them and `fg` brings one back.
- **Signal another program, and catch signals.** `kill`, `raise`,
  `getpid` and `waitpid`; `sigaction`, `signal`, `sigprocmask`,
  `sigpending`, `sigsuspend` and `pause`, all with Linux's numbers and
  meanings. A handler is an ordinary function of one argument. `signal()`
  sets `SA_RESTART`, as glibc's does.
- **Use the network the Linux way.** The whole socket API with Linux's
  signatures, loopback included; see *The network* below.
- **Use pipes and redirection.** `pipe`, `dup`, `dup2` and `fcntl`
  (`O_NONBLOCK`, `FD_CLOEXEC`, `F_DUPFD`), with Linux's meanings: end of
  file when the last writer closes, SIGPIPE and `EPIPE` when the last
  reader has gone, and writes of up to `PIPE_BUF` never interleaved.
  The shell's `<`, `>`, `>>`, `2>`, `2>&1` and `|` reach a program's
  descriptors. Start a child with its ends on 0 and 1 by `dup2`ing them
  there before `spawn` and closing your own copies; mark the ones it
  must not have `FD_CLOEXEC`.
- **Belong to a process group.** `getpgrp`, `setpgid`, `tcgetpgrp` and
  `kill(0, ...)` / `kill(-pgid, ...)`. The shell gives each job a group
  of its own, and a helper you `spawn` joins yours.
- **Wait on several descriptors** with `poll()` or `select()`: the
  terminal, sockets and files. A terminal in canonical mode is readable
  when a character is waiting, not when a whole line is. Put it in raw
  mode, as any program that polls a terminal does, and readable means
  exactly that.
- **Use subdirectories**, a working directory, and relative paths.
- **Read its environment.** `getenv()`, inherited from the shell.

### What a program cannot do yet

- **Use an alternate signal stack.** `SA_ONSTACK` is refused with
  `EINVAL`. Nor can a program catch the signal from its own access
  fault: that one still ends it. (`SA_SIGINFO` handlers work, with
  Linux/m68k's `siginfo` and `ucontext`.)

`design.md` §11 is the open-items list, and `emacs.md` costs the whole
set out against one real program.

---

## 16. Quick reference

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
M48T59       0xff600000   byte regs        MFP ch 2   clock = last 8 bytes
8042 kbd     0xff700000   byte regs        MFP ch 1   data +0, status +1

MFP vector   (VR & 0xF0) | channel
MFP clock    2.4576 MHz, prescalers 4/10/16/50/64/100/200
MFP tick     timer D, /200, reload 123 -> 99.9 Hz (the kernel's HZ=100)

syscalls     d0 = number, d1-d5 = args, trap #0, d0 = result or -errno
             Linux/m68k convention, numbers and errnos (abicheck.sh)
devices      /dev/console /dev/tty  the terminal (sources + sinks)
             /dev/ttyS0   the serial port, raw
             /dev/kbd0    the 8042 keyboard, an input source
             /dev/fbcon   the text console, output only
             /dev/vcsa    what is on it, readable
             /dev/fb0     the framebuffer
             /dev/hda     the disk
```

Worked, tested code for every device is in [`tests/`](tests/) — `t6` for the
interrupt chain, `t7`/`t8`/`t9` for the MFP, `t3` for ATA, `t4` for ethernet,
`t5` for the MMU, `t10` for video, `t11` for the clock and its NVRAM, `t12`
for the keyboard. **Four scripted sessions** test the system rather than
a device: `kernel/fstest.sh` for the filesystem (39 checks),
`kernel/edittest.sh` for the line editor, history, job control and
`shutdown` (27), `kernel/vmtest.sh` for memory protection (15), and
`kernel/nettest.sh` for DHCP, ARP, ICMP and TCP against a web server on
the host (13).

Driver versions of most of them are in [`kernel/drivers/`](kernel/drivers/), which is
where to look for code that has to keep working rather than code that only has
to pass once.
