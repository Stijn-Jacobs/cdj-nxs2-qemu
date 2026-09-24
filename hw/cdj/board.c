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
 * its reset stub (image offset 0x100) through P2, as the bootloader does. The
 * stub sets up no stack, so SP is primed with the bootloader's value.
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

typedef struct CdjResetData {
    SuperHCPU *cpu;
    uint32_t pc;
    uint32_t sp;
} CdjResetData;

unsigned cdj_reset_count;

static void cdj_cpu_reset(void *opaque)
{
    CdjResetData *s = opaque;
    CPUSH4State *env = &s->cpu->env;

    /* A reset looks like a firmware loop from the PC alone; log it. */
    if (++cdj_reset_count > 1) {
        qemu_log("MACHINE RESET #%u (pc was 0x%08x)\n",
                 cdj_reset_count, env->pc);
    }
    cpu_reset(CPU(s->cpu));
    env->pc = s->pc;
    env->gregs[15] = s->sp;
}

/* Map a device region at both its architectural address and its A7 alias, so it
 * is reachable whichever segment the firmware uses. */
static void cdj_unimp(const char *name, hwaddr addr, hwaddr size)
{
    create_unimplemented_device(name, addr, size);
    if (A7ADDR(addr) != addr) {
        g_autofree char *alias = g_strdup_printf("%s-a7", name);
        create_unimplemented_device(alias, A7ADDR(addr), size);
    }
}

static void cdj2000nxs2_init(MachineState *machine)
{
    SuperHCPU *cpu;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *dram = g_new(MemoryRegion, 1);
    CdjResetData *reset_info;
    DriveInfo *dinfo;
    ssize_t fwsize;

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    memory_region_init_ram(dram, NULL, "cdj.dram", CDJ_DRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, CDJ_DRAM_PHYS, dram);

    /*
     * SH-4 area 4 (CS4), 0x10000000 up. The firmware uses it as plain shared
     * memory: the DSP transfer driver at 0x083AFDC0 loads a pointer from
     * 0x10DDEFC8, and the SPI1 tx busy flags (0x10DC1510..0x10DC151F) and
     * the word tsk_DSP_startup reads (0x10DBFC78) live here too.
     * CDJ_AREA4=1 backs it with 16 MB of RAM; unset, it stays unmapped.
     */
    if (getenv("CDJ_AREA4")) {
        MemoryRegion *area4 = g_new(MemoryRegion, 1);

        memory_region_init_ram(area4, NULL, "cdj.area4", CDJ_AREA4_SIZE,
                               &error_fatal);
        memory_region_add_subregion(sysmem, CDJ_AREA4_PHYS, area4);
        info_report("cdj2000nxs2: area 4 backed with %u MB RAM at 0x%08x",
                    (unsigned)(CDJ_AREA4_SIZE / MiB), CDJ_AREA4_PHYS);
    }

    /*
     * SH-4 area 6 (CS6), 0x18000000 up: the DSP's uPP data window. MAIN DMAs
     * to 0xB8000000 from 0x083262A4 and waits on flg_DSPuPP; MSIOF0 is the
     * control plane. The DSP has no program store of its own, so its boot
     * image arrives here. CDJ_AREA6=1 backs it with RAM so the writes can be
     * dumped; unset, it stays unmapped.
     */
    if (getenv("CDJ_AREA6")) {
        MemoryRegion *area6 = g_new(MemoryRegion, 1);

        memory_region_init_ram(area6, NULL, "cdj.area6", CDJ_AREA6_SIZE,
                               &error_fatal);
        memory_region_add_subregion(sysmem, CDJ_AREA6_PHYS, area6);
        info_report("cdj2000nxs2: area 6 (CS6, the DSP window) backed with "
                    "%u MB RAM at 0x%08x",
                    (unsigned)(CDJ_AREA6_SIZE / MiB), CDJ_AREA6_PHYS);
    }

    /* NOR flash as a CFI device, not RAM: the firmware programs its settings
     * sectors at runtime with the AMD command set (0xAA/0x55 unlock, 0xA0
     * program, then DQ7/DQ5 polling, at 0x352CFC). The 0x55 goes to byte
     * 0x554, i.e. word 0x2AA on a 16-bit bus.
     *
     * Geometry from the firmware's sector tables (0x080B0978 addresses,
     * 0x080B0B94 sizes): top-boot 8 MB, 127 x 64 KiB then 8 x 8 KiB. The
     * small sectors matter: with a uniform 64 KiB layout, erasing one 8 KiB
     * sector wipes its neighbours, including the settings sector at 0x7F6000.
     * No backing drive means erased (0xFF). CDJ_FLASH_UNIFORM=1 selects the
     * uniform layout.
     */
    dinfo = drive_get(IF_PFLASH, 0, 0);
    {
        DeviceState *fl = qdev_new(TYPE_PFLASH_CFI02);
        bool uniform = getenv("CDJ_FLASH_UNIFORM") != NULL;

        if (dinfo) {
            qdev_prop_set_drive(fl, "drive", blk_by_legacy_dinfo(dinfo));
        }
        if (uniform) {
            qdev_prop_set_uint32(fl, "num-blocks", CDJ_FLASH_SIZE / (64 * KiB));
            qdev_prop_set_uint32(fl, "sector-length", 64 * KiB);
        } else {
            qdev_prop_set_uint32(fl, "num-blocks0", 127);
            qdev_prop_set_uint32(fl, "sector-length0", 64 * KiB);
            qdev_prop_set_uint32(fl, "num-blocks1", 8);
            qdev_prop_set_uint32(fl, "sector-length1", 8 * KiB);
        }
        qdev_prop_set_uint8(fl, "width", 2);
        qdev_prop_set_uint8(fl, "mappings", 1);
        qdev_prop_set_uint8(fl, "big-endian", 0);
        qdev_prop_set_uint16(fl, "id0", 0x0001);
        qdev_prop_set_uint16(fl, "id1", 0x227e);
        qdev_prop_set_uint16(fl, "id2", 0x2220);
        qdev_prop_set_uint16(fl, "id3", 0x2200);
        qdev_prop_set_uint16(fl, "unlock-addr0", 0x555);
        qdev_prop_set_uint16(fl, "unlock-addr1", 0x2aa);
        qdev_prop_set_string(fl, "name", "cdj.flash");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(fl), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(fl), 0, CDJ_FLASH_PHYS);
        info_report("cdj2000nxs2: NOR flash %s", uniform
                    ? "uniform 128 x 64 KiB (CDJ_FLASH_UNIFORM)"
                    : "top-boot 127 x 64 KiB + 8 x 8 KiB");
    }

    /* CCN, the SH-4 core control block (0xFF000000..0xFF000028). QEMU only
     * provides it through the SH7750 SoC model, which this board does not
     * use; without it the firmware spins re-reading EXPEVT. */
    cdj_ccn_init(sysmem, cpu);
    cdj_unimp("sh4.ubc",      0xFF200000, 0x1000);

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

        cdj_dmac_init(sysmem, dei);
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

    if (!machine->kernel_filename) {
        error_report("cdj2000nxs2: pass the decompressed MAIN image with "
                     "-kernel main_unpacked.bin");
        exit(1);
    }

    /* Flat binary, not an ELF. */
    fwsize = load_image_targphys(machine->kernel_filename,
                                 CDJ_DRAM_PHYS, CDJ_DRAM_SIZE);
    if (fwsize < 0) {
        error_report("cdj2000nxs2: cannot load '%s'", machine->kernel_filename);
        exit(1);
    }
    info_report("cdj2000nxs2: %zd bytes at 0x%08x, entry 0x%08x, sp 0x%08x",
                fwsize, CDJ_DRAM_PHYS, CDJ_FW_ENTRY, CDJ_INIT_SP);

    /* Seed the RTOS's SR save slot (see CDJ_SR_SAVE_SLOT). Done after the
     * image load so the image cannot overwrite it. */
    {
        uint32_t sr_seed = cpu_to_le32(CDJ_SR_SEED);
        cpu_physical_memory_write(CDJ_SR_SAVE_SLOT, &sr_seed, sizeof(sr_seed));
    }

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

    /* PC/SP are applied on reset; setting env->pc here would be overwritten
     * by the CPU's own reset. */
    reset_info = g_new0(CdjResetData, 1);
    reset_info->cpu = cpu;
    reset_info->pc = CDJ_FW_ENTRY;
    reset_info->sp = CDJ_INIT_SP;
    qemu_register_reset(cdj_cpu_reset, reset_info);
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
