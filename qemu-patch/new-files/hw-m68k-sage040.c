/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sage040 - a plain 68040 workstation built only from real, documented silicon.
 *
 * Every device here corresponds to a physical part with a publicly available
 * datasheet.  There is deliberately no virtio, no goldfish, and no invented
 * hardware of any kind.
 *
 *   MC68040          CPU, on-chip FPU and MMU   (Motorola M68040UM)
 *   MC68901 MFP      timers + interrupt ctrl    (Motorola MC68901 datasheet)
 *   NS16550A         serial console             (TI/NS PC16550D datasheet)
 *   ATA taskfile     disk, WD1003 lineage       (T13 ATA/ATAPI specs)
 *   SMSC LAN91C111   ethernet                   (SMSC LAN91C111 datasheet)
 *   Silicon Motion SM501  bitmapped video       (SM501 datasheet)
 *   ST M48T59            clock + 8 KB NVRAM      (M48T59 datasheet)
 *
 * Interrupts
 * ----------
 * The MC68901 is the system interrupt controller, exactly as on real 68k
 * boards: it drives one 68040 IPL line and supplies its own vector during
 * the acknowledge cycle.  Peripheral interrupt outputs are wired to its
 * GPIP inputs.  Two of those pins double as the MFP's timer event inputs,
 * which is a property of the real chip and is used deliberately here:
 *
 *   GPIP5 (channel  7) <- NS16550A UART
 *   GPIP4 (channel  6) <- ATA            (also TAI, timer A event input)
 *   GPIP3 (channel  3) <- LAN91C111      (also TBI, timer B event input)
 *   GPIP2 (channel  2) <- M48T59         (alarm and watchdog)
 *
 * Physical memory map:
 *
 *   0x00000000  RAM (size from -m); vectors at 0, code conventionally at 0x400
 *   0xf0000000  SM501 video memory (16 MiB)
 *   0xff000000  NS16550A UART, byte-spaced registers
 *   0xff100000  ATA command block  (8 regs, offset 0 = 16-bit data)
 *   0xff101000  ATA control block  (read: alt status, write: device control)
 *   0xff200000  LAN91C111, 16-byte bank-switched window
 *   0xff300000  MC68901 MFP, 24 byte-spaced registers
 *   0xff400000  SM501 control registers
 *   0xff600000  M48T59 NVRAM (8 KiB); the clock is the last eight bytes
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "hw/ide/mmio.h"
#include "hw/net/smc91c111.h"
#include "hw/misc/mc68901.h"
#include "net/net.h"
#include "elf.h"

#define SAGE040_SM501_VRAM    0xf0000000
#define SAGE040_SERIAL_BASE   0xff000000
#define SAGE040_IDE_CMD_BASE  0xff100000
#define SAGE040_IDE_CTL_BASE  0xff101000
#define SAGE040_NET_BASE      0xff200000
#define SAGE040_MFP_BASE      0xff300000
#define SAGE040_SM501_MMIO    0xff400000
#define SAGE040_RTC_BASE      0xff600000

#define SAGE040_SM501_VRAM_SIZE (16 * MiB)

/* 16550 reference clock; divisor 1 gives 115200 baud. */
#define SAGE040_UART_BAUDBASE 115200

/* The MFP drives IPL 6 and its XTAL1 runs at the classic 2.4576 MHz. */
#define SAGE040_MFP_IPL       6
#define SAGE040_MFP_FREQ      2457600

/* GPIP pins the peripherals are wired to. */
#define SAGE040_GPIP_UART     5
#define SAGE040_GPIP_ATA      4     /* also TAI, timer A event input */
#define SAGE040_GPIP_NET      3     /* also TBI, timer B event input */
#define SAGE040_GPIP_RTC      2     /* M48T59 alarm / watchdog output   */

/*
 * The M48T59 stores a two-digit BCD year and the driver adds a century to
 * it, so the part covers 2000-2099 here.  Boards choose this: Sun's use
 * 1968.
 */
#define SAGE040_RTC_BASE_YEAR 2000

typedef struct {
    M68kCPU *cpu;
    uint32_t initial_pc;
    uint32_t initial_stack;
} ResetInfo;

static void sage040_cpu_reset(void *opaque)
{
    ResetInfo *reset_info = opaque;
    M68kCPU *cpu = reset_info->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = reset_info->initial_stack;
    cpu->env.pc = reset_info->initial_pc;
}

static void sage040_init(MachineState *machine)
{
    M68kCPU *cpu;
    DeviceState *mfp_dev, *ide_dev, *sm501_dev;
    DeviceState *rtc_dev;
    SysBusDevice *sysbus;
    ResetInfo *reset_info;
    uint64_t elf_entry;
    int kernel_size;

    if (machine->ram_size > 2 * GiB) {
        error_report("sage040: maximum supported RAM is 2 GiB");
        exit(1);
    }

    reset_info = g_new0(ResetInfo, 1);

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    reset_info->cpu = cpu;
    reset_info->initial_stack = machine->ram_size - 16;
    qemu_register_reset(sage040_cpu_reset, reset_info);

    /* RAM at physical zero, so the 68040 reset vectors live at 0x0/0x4. */
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /*
     * MC68901 MFP: the system interrupt controller.  It owns the CPU's
     * interrupt input, which is why there is no m68k-irqc here - the m68k
     * CPU accepts a level and a vector from exactly one source, and using
     * the MFP is what a real board of this shape does.
     */
    mfp_dev = qdev_new(TYPE_MC68901);
    object_property_set_link(OBJECT(mfp_dev), "m68k-cpu",
                             OBJECT(cpu), &error_abort);
    qdev_prop_set_uint32(mfp_dev, "irq-level", SAGE040_MFP_IPL);
    qdev_prop_set_uint64(mfp_dev, "timer-frequency", SAGE040_MFP_FREQ);
    qdev_prop_set_chr(mfp_dev, "chardev", serial_hd(1));
    sysbus = SYS_BUS_DEVICE(mfp_dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, SAGE040_MFP_BASE);

    /*
     * NS16550A UART.  regshift 0 puts the eight registers at consecutive
     * byte addresses exactly as the datasheet numbers them.
     */
    serial_mm_init(get_system_memory(), SAGE040_SERIAL_BASE, 0,
                   qdev_get_gpio_in(mfp_dev, SAGE040_GPIP_UART),
                   SAGE040_UART_BAUDBASE, serial_hd(0), DEVICE_BIG_ENDIAN);

    /*
     * ATA taskfile.  Region 0 is the command block: offset 0 is the 16-bit
     * data register, offsets 1-7 are the usual taskfile registers.  Region 1
     * is the control block.
     */
    ide_dev = qdev_new(TYPE_MMIO_IDE);
    qdev_prop_set_uint32(ide_dev, "shift", 0);
    sysbus = SYS_BUS_DEVICE(ide_dev);
    /*
     * Connect the interrupt BEFORE realizing.  mmio_ide_realizefn() passes
     * s->irq to ide_bus_init_output_irq() by value, so a connection made
     * afterwards updates s->irq but leaves the IDE bus holding the old
     * (null) handle and the drive's interrupt never arrives.
     */
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(mfp_dev, SAGE040_GPIP_ATA));
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, SAGE040_IDE_CMD_BASE);
    sysbus_mmio_map(sysbus, 1, SAGE040_IDE_CTL_BASE);
    mmio_ide_init_drives(ide_dev,
                         drive_get(IF_IDE, 0, 0),
                         drive_get(IF_IDE, 0, 1));

    /* SMSC LAN91C111: 16-byte bank-switched register window. */
    smc91c111_init(SAGE040_NET_BASE,
                   qdev_get_gpio_in(mfp_dev, SAGE040_GPIP_NET));

    /* Silicon Motion SM501: video memory plus a control register block. */
    sm501_dev = qdev_new("sysbus-sm501");
    qdev_prop_set_uint32(sm501_dev, "vram-size", SAGE040_SM501_VRAM_SIZE);
    sysbus = SYS_BUS_DEVICE(sm501_dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, SAGE040_SM501_VRAM);
    sysbus_mmio_map(sysbus, 1, SAGE040_SM501_MMIO);

    /*
     * ST M48T59 TIMEKEEPER: 8 KiB of battery-backed SRAM whose last eight
     * bytes are the clock, plus alarm and watchdog registers just below
     * them.  Chosen over the MC146818 for two reasons: QEMU's MC146818
     * model is an ISA device and this board has no ISA bus, and the
     * M48T59 is directly memory mapped with byte-wide registers like
     * everything else here.  The NVRAM is a genuine addition - it is the
     * only storage on the machine that survives a power cycle without
     * going through the disk.
     *
     * mmio region 0 is the directly mapped window; region 1 is the
     * indirect address/data port pair some boards use instead, which this
     * one does not need.
     */
    rtc_dev = qdev_new("sysbus-m48t59");
    qdev_prop_set_int32(rtc_dev, "base-year", SAGE040_RTC_BASE_YEAR);
    sysbus = SYS_BUS_DEVICE(rtc_dev);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(mfp_dev, SAGE040_GPIP_RTC));
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, SAGE040_RTC_BASE);

    /*
     * Boot protocol: load a big-endian ELF32 (EM_68K) and start at its entry
     * point with SP at the top of RAM.  No ROM, no bootloader, no bootinfo
     * block - the OS is expected to know its own machine.
     */
    if (machine->kernel_filename) {
        kernel_size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                               &elf_entry, NULL, NULL, NULL,
                               ELFDATA2MSB, EM_68K, 0, 0);
        if (kernel_size < 0) {
            error_report("sage040: could not load kernel '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        reset_info->initial_pc = elf_entry;
    } else if (!qtest_enabled()) {
        error_report("sage040: no -kernel argument given");
        exit(1);
    }
}

static void sage040_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Sage040 (68040, MC68901 MFP, 16550A, ATA, LAN91C111, SM501)";
    mc->init = sage040_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
    mc->default_ram_id = "sage040.ram";
    mc->default_ram_size = 4 * MiB;
}

static const TypeInfo sage040_machine_info = {
    .name       = MACHINE_TYPE_NAME("sage040"),
    .parent     = TYPE_MACHINE,
    .class_init = sage040_machine_class_init,
};

static void sage040_machine_register_types(void)
{
    type_register_static(&sage040_machine_info);
}

type_init(sage040_machine_register_types)
