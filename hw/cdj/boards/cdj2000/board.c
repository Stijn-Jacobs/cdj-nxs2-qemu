/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
#include "cdj_ether.h"
#include "cdj_ata.h"
#include "usb_r8a66597.h"
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

#define CDJ2000_USBH_BASE   0xFE400000

static const CdjBoardDesc cdj2000nxs_board = {
    .name = "cdj2000nxs",
    /* .data is copied to 0x0A799760 and the stack starts at 0xAC000000;
     * the bootloader's memory test covers 0xA4000000..0xABFFEF00. */
    .dram_phys = 0x04000000,
    .dram_size = 128 * MiB,
    /* Sector tables at 0x04075FF8 (addresses) and 0x04076114 (sizes). The
     * firmware never sends an ID or CFI query, so the NXS2's IDs do. */
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .fw_entry = 0xA4000800,
    .init_sp = 0xAC000000,
    /* The bootloader enters the image in register bank 0. From the reset
     * value's bank 1, the interrupt-disable at 0x043688B0 saves the old SR in
     * bank 1's r0 and returns bank 0's (zero), and the matching restore drops
     * to user mode: an illegal-instruction fault on the next stc sr. */
    .init_sr = 0x500000F0,              /* MD=1, BL=1, IMASK=0xF, RB=0 */
    /* The NXS2's RTOS quirk, byte for byte: SR is restored from this global
     * (0x04368888, 0x04368894) before 0x04368878 first writes it. */
    .sr_seed_slot = 0x04D13694,
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

#ifdef _WIN32
/*
 * Windows sleeps in whole timer periods, so the main loop, which runs the
 * TMU's timers, wakes every 15.6 ms (2-4 ms with a 1 ms period requested).
 * The 1 kHz tick's underflows then arrive in bursts that the held UNF line
 * merges into one interrupt each: the RTOS counted ~70 ticks a second. A
 * high-resolution waitable timer wakes the loop every 250 us instead.
 */
#define SH7763_KICK_100NS 2500

static QemuThread sh7763_kicker;

static void *sh7763_kick_main_loop(void *opaque)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    LARGE_INTEGER due = { .QuadPart = -SH7763_KICK_100NS };

    for (;;) {
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
        qemu_notify_event();
    }
    return NULL;
}
#endif

/* What the check at 0x0410FF00 needs from the auth chip. */
static const CdjAuthAnswer nxs_auth_answers[] = {
    { 0x00, 0x05 },
    { 0x01, 0x01 },
};

void sh7763_board_init(MachineState *machine, const CdjBoardDesc *desc,
                       const CdjDspWires *(*dsp_init)(MemoryRegion *, hwaddr),
                       const Cdj2000Display *display_desc,
                       bool auth_chip)
{
    MemoryRegion *sysmem = get_system_memory();
    SuperHCPU *cpu = cdj_board_init(machine, desc);
    qemu_irq *irq;
    bool display;
    static const CdjRegInit cpg[] = {
        { 0x00, SH7763_FRQCR_BOOT },
        { 0 },
    };

#ifdef _WIN32
    qemu_thread_create(&sh7763_kicker, "tmu-kick", sh7763_kick_main_loop,
                       NULL, QEMU_THREAD_DETACHED);
#endif
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
    cdj_add_exit_report(&cdj_irqcount_exit);

    /* The same SH-4A DMAC as the NXS2's, at another base, with its resource
     * selectors (DMARS) at +0x1000. The USB host's FIFO window is its DREQ
     * source: a sector read arms the channel (source 0xFE400180) before the
     * data arrives, so it must wait for the host's request. */
    {
        CdjDmacDei dei[4] = {
            { cdj_count_irq(irq[S63_DMTE0], "DMAC DMTE0") },
            { cdj_count_irq(irq[S63_DMTE1], "DMAC DMTE1") },
            { cdj_count_irq(irq[S63_DMTE2], "DMAC DMTE2") },
            { cdj_count_irq(irq[S63_DMTE3], "DMAC DMTE3") },
        };

        cdj_dmac_init(sysmem, "sh7763.dmac", 0xFF608000,
                      A7ADDR(CDJ2000_USBH_BASE), CDJ_USB_SIZE, dei);
        /* The CDJ-900's MAIN moves each sector block into the DSP window on
         * channel 4 (the CDJ-2000's uses channel 3) and waits for its end. */
        cdj_dmac_dei_connect(4, cdj_count_irq(irq[S63_DMTE4], "DMAC DMTE4"));
    }

    /* The flash is 4 MB by its own sector table (71 sectors at 0x04075FF8,
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
    /* The DMA-fed port to the front-panel microcontroller (M16C); a display
     * with a link_byte shares it. */
    cdj2000_panel_init(sysmem, 0xFFE20000, display_desc);
    /* The BF531 display processor, its own window; off unless asked for. */
    display = cdj2000_display_init(display_desc);

    /* Same SH7724 fast-EtherC/E-DMAC layout as the NXS2's, at this SoC's own
     * base; its MDIO read routine needs one extra turnaround lead-in bit
     * (static trace only, not yet confirmed against a live MDIO read). */
    cdj_ether_init(sysmem, "sh7763.ether", 0xFEF00000,
                  cdj_count_irq(irq[S63_GETHER0], "GETHER0"), 1);
    cdj2000_sdhi_init(sysmem);
    if (auth_chip) {
        cdj2000_iic_init(sysmem, nxs_auth_answers,
                         ARRAY_SIZE(nxs_auth_answers));
    }
    /* The ATAPI (CD drive) task file and control block, not GPIO: the
     * IDENTIFY sequence (0x042971EC) programs this range with the SH7724
     * ATAPI_CONTROL* layout, offset for offset. GPIO/PFC is the next 64 KiB.
     * Its ISR 0x04109180 is registered as INTEVT H'C00 (T_CISR 0x0405D31C),
     * enabled through INT2PRI6 bits 31-24 and INT2MSKCR bit 20. */
    cdj_ata_init(sysmem, "sh7763.atapi", 0xFFF00000,
                 cdj_count_irq(irq[S63_ATAPI], "ATAPI"));
    cdj_unimp("sh7763.gpio",  0xFFF10000, 0x10000);
    /* Created after the board's RAM so that the largest RAM block sits at
     * offset 0: a migration's dirty-bitmap sync clears each block's range
     * from 0, and QEMU asserts when that range spans two blocks. */
    cdj2000_latch_init(sysmem, dsp_init(sysmem, 0x0C000000), display);
    /* On-chip USB host (the front stick port): the NXS2's R8A66597 host
     * controller at its own base. usbh_load()'s boot pass is read-modify-write
     * at that chip's SYSCFG0, FIFOSEL, INTENB, BRDYENB/NRDYENB/BEMPENB and
     * PIPESEL/PIPEBUF offsets. */
    cdj_usb_init(sysmem, CDJ2000_USBH_BASE,
                 cdj_count_irq(irq[S63_USBH], "USBH"), true);
    /* The display link's receive and transmit DMA blocks. Without the
     * display processor they are plain read-back registers, which the
     * driver's init (0x042A38FC) never branches on. */
    if (display) {
        cdj2000_display_link_init(sysmem,
            cdj_count_irq(irq[S63_DISP_RX_DMA], "display rx DMA"),
            cdj_count_irq(irq[S63_DISP_TX_DMA], "display tx DMA"),
            cdj_count_irq(irq[S63_DISP_TX_SER], "display tx serial"));
    } else {
        cdj_regs(sysmem, "sh7763.blk-ff40", 0xFF400000, 0x10000, NULL);
        cdj_regs(sysmem, "sh7763.blk-ff50", 0xFF500000, 0x10000, NULL);
    }
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
    /* The NXS's DSP is a C674x behind its host port, addressed through HPIA.
     * fw 1.44's DSP application polls in a main loop at 0xC004CB8C, its
     * stack at B15 0x11805AE0 on entry. */
    cdj_c6747_set_idle_loop(0xC004CB8C, 0x11804AE0, 0x11805C00);
    sh7763_board_init(machine, &cdj2000nxs_board,
                      cdj_c6747_init,
                      &cdj2000nxs_display, true);
}

static void cdj2000_init(MachineState *machine)
{
    /* The CDJ-2000's is a C6727 behind a full-address host port: its
     * uncached area-3 window at 0x0C0C0000 (454 references in the image) is
     * DSP memory. fw 4.33's DSP application polls in a main loop at
     * 0x80047B80, its stack at B15 0x100064B0 on entry. */
    cdj_c6727_set_idle_loop(0x80047B80, 0x10005000, 0x10006600);
    sh7763_board_init(machine, &cdj2000_board,
                      cdj_c6727_init,
                      &cdj2000_display, false);
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
