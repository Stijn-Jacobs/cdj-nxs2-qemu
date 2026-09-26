#include "cdj.h"
#include "cdj_getenv.h"
/*
 * Pioneer CDJ-2000NXS2 (Renesas SH7724) board.
 *
 * The memory map was worked out by running the device's own firmware, not
 * from a datasheet. Regions that are not modelled are mapped as unimplemented
 * devices so their accesses are logged rather than silently reading zero.
 *
 * Boot: the decompressed MAIN image is loaded at the DRAM base and entered at
 * _start (image offset 0x800) through P2, as the bootloader does. _start sets
 * up no stack, so SP is primed with the bootloader's value.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Front-panel MCU link: SCIF2, driven by DMAC0A ch1 (rx) and ch2 (tx).
 * CDJ_PANEL_RX=0 disables it: SCIF2 becomes a logged region and DEI1 is left
 * unconnected. */
bool cdj_pnl_enabled(void)
{
    const char *e = getenv("CDJ_PANEL_RX");

    return !(e && !strcmp(e, "0"));
}

/*
 * The NXS2 memory map. Area 4 and area 6 are opt-in RAM:
 *
 * SH-4 area 4 (CS4), 0x10000000 up. The firmware uses it as plain shared
 * memory: the DSP transfer driver at 0x083AFDC0 loads a pointer from
 * 0x10DDEFC8, and the SPI1 tx busy flags (0x10DC1510..0x10DC151F) and
 * the word tsk_DSP_startup reads (0x10DBFC78) live here too.
 * CDJ_AREA4=1 backs it with 16 MB of RAM; unset, it stays unmapped.
 *
 * SH-4 area 6 (CS6), 0x18000000 up: the DSP's uPP data window. MAIN DMAs
 * to 0xB8000000 from 0x083262A4 and waits on flg_DSPuPP; MSIOF0 is the
 * control plane. The DSP has no program store of its own, so its boot
 * image arrives here. CDJ_AREA6=1 backs it with RAM so the writes can be
 * dumped; unset, it stays unmapped.
 */
static const CdjRamRegion cdj2000nxs2_extra_ram[] = {
    { "cdj.area4", "CDJ_AREA4", CDJ_AREA4_PHYS, CDJ_AREA4_SIZE, "area 4" },
    { "cdj.area6", "CDJ_AREA6", CDJ_AREA6_PHYS, CDJ_AREA6_SIZE,
      "area 6 (CS6, the DSP window)" },
};

static const CdjBoardDesc cdj2000nxs2_board = {
    .name = "cdj2000nxs2",
    .dram_phys = CDJ_DRAM_PHYS,
    .dram_size = CDJ_DRAM_SIZE,
    .extra_ram = cdj2000nxs2_extra_ram,
    .n_extra_ram = ARRAY_SIZE(cdj2000nxs2_extra_ram),
    /* Geometry from the firmware's sector tables (0x080B0978 addresses,
     * 0x080B0B94 sizes): top-boot 8 MB, 127 x 64 KiB then 8 x 8 KiB, the
     * settings sector being the 8 KiB one at 0x7F6000. The firmware programs
     * it at 0x352CFC. */
    .flash_phys = CDJ_FLASH_PHYS,
    .flash_size = CDJ_FLASH_SIZE,
    .flash = { { 127, 64 * KiB }, { 8, 8 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .fw_entry = CDJ_FW_ENTRY,
    .init_sp = CDJ_INIT_SP,
    .sr_seed_slot = CDJ_SR_SAVE_SLOT,
    .sr_seed = CDJ_SR_SEED,
    /* The firmware programs TCOR0 = 10415 with TPSC = P0/4, which is exactly
     * a 1 kHz RTOS tick at 41.666667 MHz. The GUI link times out if MAIN's
     * tick runs slow. */
    .periph_hz = 41666667,
    .ccn_trace_lo = 0x800,              /* DMAC0A DEI0..DEI3 */
    .ccn_trace_hi = 0x860,
    .exit_report = cdj_intc_exit_report,
};

/* DMAC0A's DEI lines, connected as the firmware needs them (CDJ_DMAC_DEI=0
 * disconnects all, CDJ_DMAC_DEI_ALL=1 connects all four):
 *   DEI0  USB D0FIFO drain, waited on at 0x08235952.
 *   DEI1  panel receive (40-byte report from SCFRDR2 into 0xA9000000);
 *         its ISR 0x083EB080 wakes PnlCom_RcvTASK. On with the panel.
 *   DEI2  panel transmit; its ISR 0x083EB012 only clears CHCR_2, so it
 *         is off unless CDJ_PANEL_DEI2=1.
 * Delivery relies on the sh_intc priority-group fix in patches/. */
static void cdj2000nxs2_dmac(MemoryRegion *sysmem, const qemu_irq *lines)
{
    CdjDmacDei dei[4] = { { 0 } };
    const char *off = getenv("CDJ_DMAC_DEI");
    const char *cap = getenv("CDJ_PANEL_MAX_IRQ");
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(dei); i++) {
        bool want = i == 0
                 || (i == 1 && cdj_pnl_enabled())
                 || (i == 2 && getenv("CDJ_PANEL_DEI2"))
                 || getenv("CDJ_DMAC_DEI_ALL");

        dei[i].irq = (want && !(off && !strcmp(off, "0"))) ? lines[i] : NULL;
        if (i == 1 || i == 2) {
            /* The panel exchanges ~200 frames/s, so the default cap is about
             * 15 minutes; CDJ_PANEL_MAX_IRQ overrides it. */
            dei[i].max = cap ? (unsigned)strtoul(cap, NULL, 0) : 200000;
        }
    }
    cdj_dmac_init(sysmem, "sh7724.dmac", 0xFE008000,
                  CDJ_USB_BASE, CDJ_USB_SIZE, dei);
}

static void cdj2000nxs2_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    SuperHCPU *cpu = cdj_board_init(machine, &cdj2000nxs2_board);

    /* INTC-A (0xA4080000) is modelled by cdj_intc_init(); no unimplemented
     * region there, it would overlap sh_intc's register aliases. */
    cdj_intc_init(sysmem, cpu);
    /* INTC-B: cdj_intc_init() models INTPRI00/INTMSK00/INTMSKCLR00 on top of
     * this low-priority logged region, which still catches ICR0/ICR1/INTREQ00. */
    cdj_unimp("sh7724.intc-b", 0xA4140000, 0x1000);
    cdj_unimp("sh7724.intc-c", 0xA4090000, 0x1000);
    cdj_unimp("sh7724.cmt",    0xA44A0000, 0x1000);  /* compare-match timer */
    cdj_unimp("sh7724.misc-d9", 0xA4D90000, 0x1000); /* referenced by firmware */
    cdj_unimp("sh7724.misc-ce", 0xA4CE0000, 0x1000); /* referenced by firmware */

    cdj_pfc_init(sysmem);                            /* PFC + LED decoding */
    cdj_unimp("sh7724.cpg",   0xA4150000, 0x1000);   /* clock pulse gen  */
    cdj_unimp("sh7724.flctl", 0xA4530000, 0x1000);
    cdj_dsp_init();
    cdj_c6x_init();
    cdj_msiof(sysmem, "sh7724.msiof0", 0xA4C40000, cdj_dsp.present);
    cdj_msiof(sysmem, "sh7724.msiof1", 0xA4C50000, false);
    cdj_ata_init(sysmem);
    {
        /* AL/TACK/WAIT/DTE per channel, in the order cdj_iic expects. Counted
         * so the exit summary shows a line rising without its ISR running.
         * Built even without CDJ_IIC_SLAVE; the device just never raises them. */
        qemu_irq iic0[CDJ_IIC_NR_IRQ] = {
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC0_AL], "IIC0 ALI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC0_TACK], "IIC0 TACKI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC0_WAIT], "IIC0 WAITI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC0_DTE], "IIC0 DTEI"),
        };
        qemu_irq iic1[CDJ_IIC_NR_IRQ] = {
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC1_AL], "IIC1 ALI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC1_TACK], "IIC1 TACKI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC1_WAIT], "IIC1 WAITI"),
            cdj_count_irq(cdj_intc.irqs[CDJ_IIC1_DTE], "IIC1 DTEI"),
        };

        cdj_iic(sysmem, "sh7724.iic0", 0xA4470000, 0, iic0);
        cdj_iic(sysmem, "sh7724.iic1", 0xA4750000, 1, iic1);
    }
    /* 0xA4E30000-0xA4E5FFFF: SCIFA3, SCIFA4 and SCIFA5, one 64 KB slot each.
     * Accesses outside each 0x100 register window stay logged. */
    cdj_unimp("sh7724.scifa-region", 0xA4E30000, 0x30000);

    /* One chardev per channel. SCIFA3 gets serial_hd(0) (stdio with
     * -nographic) as the likely console; without a chardev a channel still
     * works and discards what it sends. */
    cdj_scifa_init(sysmem, "sh7724.scifa3", 0xA4E30000, serial_hd(0));
    cdj_scifa_init(sysmem, "sh7724.scifa4", 0xA4E40000, serial_hd(1));
    cdj_scifa4 = cdj_scifa_last;
    cdj_scifa_init(sysmem, "sh7724.scifa5", 0xA4E50000, serial_hd(2));
    {
        /* DEI lines connected: the channel draining the USB D0FIFO sets
         * CHCR.IE and waits for its completion interrupt. */
        qemu_irq dei[4] = {
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI0], "DMAC0A DEI0"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI1], "DMAC0A DEI1"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI2], "DMAC0A DEI2"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI3], "DMAC0A DEI3"),
        };

        cdj2000nxs2_dmac(sysmem, dei);
    }
    cdj_spilink_init();
    cdj_dirty_init();
    {
        /* DMA1: DMA1_SAR_0 is at 0xFDC08020, so channels start at +0x20
         * (DMA0 starts at +0x00). Channels 0-3 (DMAC1A) are the GUI link.
         * Channels 4 and 5 (DMAC1B) are the DSP's: 0x08325EA8 points ch4 at
         * MSIOF0's tx FIFO and 0x08325EF0 points ch5 at its rx FIFO. They are
         * only connected when the DSP peer is present. */
        qemu_irq dei1[CDJ_DMA1_CHANS] = {
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1A_DEI0], "DMAC1A DEI0"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1A_DEI1], "DMAC1A DEI1"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1A_DEI2], "DMAC1A DEI2"),
            cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1A_DEI3], "DMAC1A DEI3"),
            cdj_dsp.present
                ? cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1B_DEI4], "DMAC1B DEI4")
                : NULL,
            cdj_dsp.present
                ? cdj_count_irq(cdj_intc.irqs[CDJ_DMAC1B_DEI5], "DMAC1B DEI5")
                : NULL,
        };

        cdj_dma1_init(sysmem, dei1);                 /* the link to the GUI */
    }

    /* HPB window: physical area 1 (0x04000000-0x07FFFFFF) is the on-chip
     * peripheral bus. EtherMAC is modelled; the rest of 0x04C00000 is logged. */
    cdj_ether_init(sysmem, cdj_count_irq(cdj_intc.irqs[CDJ_ETHI],
                                        "EtherMAC ETHI"));
    cdj_unimp("sh7724.hpb",   0x04C00000, 0x100000);
    cdj_hpb_probe_init(sysmem);   /* logging/ready-probe for the 0x04CE0000 chip */
    if (!getenv("CDJ_USB_STUB")) {
        /* USI0 is wired so an attach can be signalled; the driver waits on
         * BCHG. */
        cdj_usb_init(sysmem, cdj_intc.irqs[CDJ_USB0]);
    }
    /* Ranges the peer path reaches that nothing else claims -- see cdj_probe. */
    cdj_probe(sysmem, "probe.area0", 0x00800000, 0x03800000);
    cdj_probe(sysmem, "probe.area1", 0x05000000, 0x03000000);

    /* Graphics block. MAIN references the LCDC (0x1E940000, A7 alias) but the
     * display is driven by the GUI processor; logged only. */
    cdj_unimp("sh7724.lcdc",  0xFE940000, 0x10000);
    cdj_unimp("sh7724.beu",   0xFE930000, 0x10000);
    cdj_unimp("sh7724.veu",   0xFE920000, 0x10000);
    cdj_unimp("sh7724.vou",   0xFE960000, 0x10000);

    cdj_unimp("sh7724.dbsc",  0xFEC10000, 0x1000);   /* DDR2 controller  */
    cdj_unimp("sh7724.bsc",   0xFF800000, 0x1000);
    cdj_unimp("sh7724.intc",  0xFFD00000, 0x1000);
    /* TMU0 at 0xFFD80000, TMU1 at 0xFFD90000. QEMU's tmu012 has the SH7724
     * layout (TSTR at +0x04, TCOR/TCNT/TCR per channel from +0x08). TMU0 is
     * the firmware's free-running timebase and the RTOS tick. Delivering the
     * tick depends on the sh_intc priority and SR.IMASK fixes in patches/.
     * CDJ_TMU_IRQ_SINK=1 routes the lines to a sink instead (no tick). */
    if (getenv("CDJ_TMU_IRQ_SINK")) {
        qemu_irq *sink = qemu_allocate_irqs(cdj_irq_sink, NULL, 6);

        info_report("cdj2000nxs2: TMU interrupts sinked -- no preemptive tick");
        cdj_tmu_init(sysmem, 0x1FD80000,
                     cdj_count_irq(sink[0], "TMU0 TUNI0"),
                     cdj_count_irq(sink[1], "TMU0 TUNI1"),
                     cdj_count_irq(sink[2], "TMU0 TUNI2"));
        cdj_tmu_init(sysmem, 0x1FD90000,
                     cdj_count_irq(sink[3], "TMU1 TUNI0"),
                     cdj_count_irq(sink[4], "TMU1 TUNI1"),
                     cdj_count_irq(sink[5], "TMU1 TUNI2"));
    } else {
        cdj_tmu_init(sysmem, 0x1FD80000,
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI0], "TMU0 TUNI0"),
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI1], "TMU0 TUNI1"),
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI2], "TMU0 TUNI2"));
        cdj_tmu_init(sysmem, 0x1FD90000,
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI0], "TMU1 TUNI0"),
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI1], "TMU1 TUNI1"),
                     cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI2], "TMU1 TUNI2"));
    }
    cdj_irqcount_exit.notify = cdj_irqcount_dump;
    qemu_add_exit_notifier(&cdj_irqcount_exit);

    /* SCIF0/1 take serial_hd(3)/(4) when given. */
    if (serial_hd(3)) {
        cdj_scif(sysmem, "scif0", CDJ_SCIF0_ADDR, serial_hd(3));
    } else if (!getenv("CDJ_SCIF0_UNIMP")) {
        /*
         * SCIF0 is the DSP's serial link. The driver polls SCFSR for
         * TDFE/TEND; without a device it times out and the boot ends in
         * E-7010 DSP DEVICE ERROR. sh_serial on a null backend is enough.
         * CDJ_SCIF0_UNIMP=1 maps it as a logged region instead.
         */
        cdj_scif(sysmem, "scif0", CDJ_SCIF0_ADDR,
                 qemu_chr_new("scif0-dsp", "null", NULL));
    } else {
        cdj_unimp("sh7724.scif0", CDJ_SCIF0_ADDR, 0x1000);
    }
    if (serial_hd(4)) {
        cdj_scif(sysmem, "scif1", CDJ_SCIF1_ADDR, serial_hd(4));
    } else {
        cdj_unimp("sh7724.scif1", CDJ_SCIF1_ADDR, 0x1000);
    }
    /* SCIF2 is the front-panel MCU link. A chardev on serial_hd(5) takes
     * precedence over the modelled panel; CDJ_PANEL_RX=0 leaves a logged
     * region. */
    if (serial_hd(5)) {
        cdj_scif(sysmem, "scif2", CDJ_SCIF2_ADDR, serial_hd(5));
    } else if (cdj_pnl_enabled()) {
        cdj_pnl_init(sysmem, CDJ_SCIF2_ADDR);
    } else {
        cdj_unimp("sh7724.scif2", CDJ_SCIF2_ADDR, 0x1000);
    }

    cdj_board_load(machine);

    if (getenv("CDJ_IVT_WATCH")) {
        cdj_ivtw_init(sysmem);
    }
    cdj_irq5_probe_init();
    cdj_audio_drain_init();
    cdj_audio_live_arm();
    cdj_dspau_arm();
    cdj_panel_init();
    cdj_vclock_init();

    cdj_pcring_exit.notify = cdj_pcring_dump;
    qemu_add_exit_notifier(&cdj_pcring_exit);

    cdj_ivt_exit.notify = cdj_ivt_dump;
    qemu_add_exit_notifier(&cdj_ivt_exit);

    cdj_console_exit.notify = cdj_console_dump;
    qemu_add_exit_notifier(&cdj_console_exit);

    cdj_board_start(cpu);
}

static void cdj2000nxs2_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-2000NXS2 (Renesas SH7724)";
    mc->init = cdj2000nxs2_init;
    /* The SH7724 is SH-4A: the interrupt handler uses icbi and synco, which
     * QEMU only accepts on a CPU with SH_FEATURE_SH4A, i.e. the SH7785. */
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = CDJ_DRAM_SIZE;
}

DEFINE_MACHINE("cdj2000nxs2", cdj2000nxs2_machine_init)
