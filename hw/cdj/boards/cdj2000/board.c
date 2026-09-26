/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
/*
 * Pioneer CDJ-2000 and CDJ-2000NXS MAIN board (Renesas SH7763) -- bring-up.
 *
 * One platform, two machines: the NXS is the CDJ-2000 with twice the DRAM.
 * The numbers are the firmware's own, read from the bootloader and the
 * decompressed images: the bootloader unpacks MAIN to DRAM at 0x04000000
 * and enters it at P2 +0x800 with no stack set up by _start itself.
 *
 * What the board models so far is what the boot needs first: the DMAC (the
 * firmware clears bss with DMAC channel 5 and polls TE), the clock register
 * the tick constant is chosen from, the interrupt controller, the timers
 * (the kernel tick is TMU channel 4) and both SCIFs. Every other
 * SH7763 block is a named unimplemented region, so the bring-up log
 * (scripts/run/boot_main.sh) lists what to model next.
 */

/* The kernel tick is TMU channel 4 at 13500 counts per interrupt: 1 ms at
 * 54 MHz. The firmware picks 13500 over 11250 when FRQCR's top nibble is 3. */
#define SH7763_PERIPH_HZ    54000000
#define SH7763_FRQCR_BOOT   0x30000000

static const CdjBoardDesc cdj2000nxs_board = {
    .name = "cdj2000nxs",
    /* .data is copied to 0x0A799760 and the stack starts at 0xAC000000;
     * the bootloader's memory test covers 0xA4000000..0xABFFEF00. */
    .dram_phys = 0x04000000,
    .dram_size = 128 * MiB,
    /* Sector tables at 0x04075D94 (addresses) and 0x04075EB0 (sizes). The
     * firmware never sends an ID or CFI query, so the NXS2's IDs do. */
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .fw_entry = 0xA4000800,
    .init_sp = 0xAC000000,
    /* The bootloader enters the image in register bank 0. From the reset
     * value's bank 1, the interrupt-disable at 0x04367E5C saves the old SR in
     * bank 1's r0 and returns bank 0's (zero), and the matching restore drops
     * to user mode: an illegal-instruction fault on the next stc sr. */
    .init_sr = 0x500000F0,              /* MD=1, BL=1, IMASK=0xF, RB=0 */
    /* The NXS2's RTOS quirk, byte for byte: SR is restored from this global
     * (0x04367E34, 0x04367E40) before 0x04367E24 first writes it. */
    .sr_seed_slot = 0x04D1368C,
    .sr_seed = 0x400000F0,
    .periph_hz = SH7763_PERIPH_HZ,
    .ccn_trace_lo = 0x640,              /* DMAC DMTE0..DMTE3 */
    .ccn_trace_hi = 0x6A0,
    .exit_report = cdj_sh7763_intc_report,
};

static const CdjBoardDesc cdj2000_board = {
    .name = "cdj2000",
    .dram_phys = 0x04000000,
    .dram_size = 64 * MiB,
    /* Sector tables at 0x0406976C and 0x04069888. */
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .fw_entry = 0xA4000800,
    /* 56 bytes below the top of DRAM, where the stage-2 call leaves it. */
    .init_sp = 0xA7FFFFC8,
    .init_sr = 0x500000F0,              /* bank 0, as on the NXS above */
    /* Written at 0x042E66F8, restored at 0x042E6708 and 0x042E6714. */
    .sr_seed_slot = 0x04FC45F0,
    .sr_seed = 0x400000F0,
    .periph_hz = SH7763_PERIPH_HZ,
    .ccn_trace_lo = 0x640,
    .ccn_trace_hi = 0x6A0,
    .exit_report = cdj_sh7763_intc_report,
};

static void sh7763_board_init(MachineState *machine, const CdjBoardDesc *desc,
                              const CdjDspWires *dsp)
{
    MemoryRegion *sysmem = get_system_memory();
    SuperHCPU *cpu = cdj_board_init(machine, desc);
    qemu_irq *irq;
    static const CdjRegInit cpg[] = {
        { 0x00, SH7763_FRQCR_BOOT },
        { 0 },
    };

    /* The interrupt controller models the registers the firmware uses; the
     * rest of both blocks is logged underneath it. */
    cdj_sh7763_intc_init(sysmem, cpu);
    cdj_unimp("sh7763.intc",  0xFFD00000, 0x10000);
    cdj_unimp("sh7763.intc2", 0xFFD40000, 0x10000);
    irq = cdj_sh7763_intc.irqs;
    cdj_tmu_init(sysmem, 0x1FD80000,
                 cdj_count_irq(irq[S63_TMU0], "TMU0"),
                 cdj_count_irq(irq[S63_TMU1], "TMU1"),
                 cdj_count_irq(irq[S63_TMU2], "TMU2"));
    cdj_tmu_init(sysmem, 0x1FDC0000,
                 cdj_count_irq(irq[S63_TMU3], "TMU3"),
                 cdj_count_irq(irq[S63_TMU4], "TMU4 tick"),
                 cdj_count_irq(irq[S63_TMU5], "TMU5"));
    cdj_irqcount_exit.notify = cdj_irqcount_dump;
    qemu_add_exit_notifier(&cdj_irqcount_exit);

    /* The same SH-4A DMAC as the NXS2's, at another base, with its resource
     * selectors (DMARS) at +0x1000. No DREQ source until USB is modelled. */
    {
        CdjDmacDei dei[4] = {
            { cdj_count_irq(irq[S63_DMTE0], "DMAC DMTE0") },
            { cdj_count_irq(irq[S63_DMTE1], "DMAC DMTE1") },
            { cdj_count_irq(irq[S63_DMTE2], "DMAC DMTE2") },
            { cdj_count_irq(irq[S63_DMTE3], "DMAC DMTE3") },
        };

        cdj_dmac_init(sysmem, "sh7763.dmac", 0xFF608000, 0, 0, dei);
    }

    /* The flash is 4 MB by its own sector table (71 sectors at 0x04075D94,
     * the last ending at 0x400000), yet the boot scans 16-bit words from
     * 0x400000 up. Nothing is fitted there: the bus reads all ones, which
     * is what ends that scan on the real board. */
    cdj_open_bus(sysmem, "cdj2000.cs0-open", 0x00400000, 0x00400000);

    cdj_regs(sysmem, "sh7763.cpg", 0xFFC80000, 0x1000, cpg);
    cdj_unimp("sh7763.wdt",   0xFFCC0000, 0x1000);
    cdj_unimp("sh7763.bsc",   0xFF800000, 0x10000);
    cdj_unimp("sh7763.sdram-mode", 0xFFA00000, 0x1000);

    /* Both SCIFs run through one driver; which is the console is not known,
     * so each gets a chardev: serial_hd(0) and (1), or a null backend. */
    cdj_scif(sysmem, "scif0", 0xFFE00000,
             serial_hd(0) ?: qemu_chr_new("scif0-null", "null", NULL));
    cdj_scif(sysmem, "scif1", 0xFFE10000,
             serial_hd(1) ?: qemu_chr_new("scif1-null", "null", NULL));
    /* The DMA-fed port to the front-panel microcontroller (M16C). */
    cdj_unimp("sh7763.scif2-panel", 0xFFE20000, 0x1000);

    cdj_unimp("sh7763.ether", 0xFEF00000, 0x10000);
    cdj_unimp("sh7763.sdhi",  0xFFE40000, 0x1000);
    cdj_unimp("sh7763.gpio",  0xFFF00000, 0x20000);
    cdj2000_latch_init(sysmem, dsp);
    cdj_unimp("sh7763.blk-ff40", 0xFF400000, 0x10000);
    cdj_unimp("sh7763.blk-ff50", 0xFF500000, 0x10000);
    cdj_unimp("sh7763.blk-ffd3", 0xFFD30000, 0x10000);
    cdj_unimp("sh7763.blk-ff2f", 0xFF2F0000, 0x10000);
    /* An R8A66597 USB controller on the external bus, by its register
     * offsets: the NXS2's USB chip, a candidate for the shared model. */
    cdj_unimp("r8a66597.usb", 0x01000000, 0x1000);

    cdj_board_load(machine);
    cdj_board_start(cpu);
}

static void cdj2000nxs_init(MachineState *machine)
{
    /* The NXS's DSP is a C674x behind its host port, addressed through HPIA. */
    sh7763_board_init(machine, &cdj2000nxs_board,
                      cdj_c6747_init(get_system_memory(), 0x0C000000));
}

static void cdj2000_init(MachineState *machine)
{
    /* The CDJ-2000's is a C6727 behind a full-address host port: its
     * uncached area-3 window at 0x0C0C0000 (454 references in the image) is
     * DSP memory. */
    sh7763_board_init(machine, &cdj2000_board,
                      cdj_c6727_init(get_system_memory(), 0x0C000000));
}

/* The SH7763 is SH-4A; QEMU only accepts icbi/synco on a CPU with
 * SH_FEATURE_SH4A, i.e. the SH7785. */
static void cdj2000nxs_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-2000NXS (Renesas SH7763), bring-up";
    mc->init = cdj2000nxs_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = 128 * MiB;
}

static void cdj2000_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-2000 (Renesas SH7763), bring-up";
    mc->init = cdj2000_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = 64 * MiB;
}

DEFINE_MACHINE("cdj2000nxs", cdj2000nxs_machine_init)
DEFINE_MACHINE("cdj2000", cdj2000_machine_init)
