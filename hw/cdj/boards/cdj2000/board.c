/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
#include "cdj_ether.h"
#include "cdj_ata.h"
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
    cdj2000_panel_init(sysmem, 0xFFE20000);
    /* The BF531 display processor, its own window; off unless asked for. */
    cdj2000_display_init();

    /* Same SH7724 fast-EtherC/E-DMAC layout as the NXS2's, at this SoC's own
     * base; its MDIO read routine needs one extra turnaround lead-in bit
     * (static trace only, not yet confirmed against a live MDIO read). */
    cdj_ether_init(sysmem, "sh7763.ether", 0xFEF00000,
                  cdj_count_irq(irq[S63_GETHER0], "GETHER0"), 1);
    /* SDHI (TMIO-style): Sd_Test_Init 0x041FEE38 only reads SOFT_RST/INFO1/
     * INFO2 back before deciding there is no card; a backed register file
     * answers that with card-detect and every status bit clear, which is
     * enough for SD init to return without a card fitted. */
    cdj_regs(sysmem, "sh7763.sdhi", 0xFFE40000, 0x1000, NULL);
    /* The ATAPI (CD drive) task file and control block, not GPIO: the
     * IDENTIFY sequence (0x042971EC) programs this range with the SH7724
     * ATAPI_CONTROL* layout, offset for offset. GPIO/PFC is the next 64 KiB. */
    cdj_ata_init(sysmem, "sh7763.atapi", 0xFFF00000);
    cdj_unimp("sh7763.gpio",  0xFFF10000, 0x10000);
    cdj2000_latch_init(sysmem, dsp);
    /* On-chip USB host (the front stick port), driven by the same HCD as the
     * NXS2's usb_r8a66597.c at its own base. A full boot trace of usbh_load()
     * is one straight-line init pass at the same offsets that file's register
     * layout names -- SYSCFG0 (+0x00), CFIFOSEL/D0FIFOSEL/D1FIFOSEL (+0x20/
     * +0x28/+0x2C), INTENB0/1 (+0x30/+0x32), BRDYENB/NRDYENB/BEMPENB (+0x36/
     * +0x38/+0x3A), then PIPESEL/PIPEBUF (+0x64/+0x6A) per pipe -- with every
     * read immediately followed by a write to the same offset (read-modify-
     * write) and no repeated read at one offset, so nothing here is a
     * read-dependent poll. A plain read-back register file satisfies it. */
    cdj_regs(sysmem, "sh7763.usbh", 0xFE400000, 0x1000, NULL);
    /* Two identical 4 KB windows 1 MB apart (0xFF401000, 0xFF501000): a
     * driver at 0x042A38FC (forced-disassembled, unreached by auto-analysis)
     * writes both with the same offsets (0x08, 0x10, 0x18, 0x28, 0x40, 0x50)
     * in lockstep, then a fixed-count software delay, never branching on
     * what it reads back. 0x08/0x10/0x18 match the published sh_mmcif
     * CE_ARG/CE_CMD_CTRL/CE_CLK_CTRL offsets, but 0x28 is read-modify-written
     * where that layout has a read-only CE_RESP1, so the identity is not
     * settled. A plain read-back register file satisfies every access seen. */
    cdj_regs(sysmem, "sh7763.blk-ff40", 0xFF400000, 0x10000, NULL);
    cdj_regs(sysmem, "sh7763.blk-ff50", 0xFF500000, 0x10000, NULL);
    cdj_unimp("sh7763.blk-ffd3", 0xFFD30000, 0x10000);
    cdj_unimp("sh7763.blk-ff2f", 0xFF2F0000, 0x10000);
    /* The rear USB-B function chip (MIDI/HID/audio class), not the NXS2's
     * R8A66597 host controller: its register layout past +0x0E does not
     * match that chip (+0x10 is an interrupt-enable register, not DMA0CFG).
     * The probe (0x042399C8) writes 1 to +0x000 and polls it for bit 0 up to
     * 256 times; a plain read-back register file answers that with nothing
     * behind it, which is enough for the boot (a failed probe only parks the
     * USB task, it does not stop MAIN). The USB host itself is on-chip at
     * 0xFE400000 (see below). */
    cdj_regs(sysmem, "cdj2000.usbf", 0x01000000, 0x1000, NULL);

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
