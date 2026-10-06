/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xdj1000.h"
#include "cdj_getenv.h"
#include "cdj_ata.h"
#include "cdj_ether.h"
#include "usb_r8a66597.h"
#include "cdj_pnl_link.h"
#include "cdj_gui_keys.h"
#include "hw/char/serial.h"
/*
 * Pioneer XDJ-1000 MAIN board (Renesas SH7724), in bring-up.
 *
 * The same SoC as the CDJ-2000NXS2 and the same RTOS image layout: the
 * decompressed MAIN image goes to the DRAM base and is entered at +0x800
 * through P2. The XDJ draws its own screen, so the graphics blocks the NXS2
 * leaves to its display processor are the ones to model here; for now they
 * are logged regions, like the other XDJ-only blocks, so the boot log lists
 * what the firmware touches first.
 */

/*
 * Front-panel link on SCIF2: DMA ch1/ch2 move one frame per exchange (the
 * firmware programs TCR 32 on the XDJ-1000 and 24 on the CDJ-900NXS), and MAIN
 * ends each frame with a checksum and the sync byte 0x8F, as on the CDJ-2000.
 */
#define SH7724_PNL_SYNC  0x8F

/*
 * The XDJ-1000's stick port is the second of the SH7724's two USB controllers:
 * MAIN sets DCFM (host mode) in the SYSCFG of the block at 0xA4D90000 and never
 * touches the first one. The CDJ-900NXS drives the first, at 0xA4D80000.
 */
#define XDJ1000_USB_BASE 0x04D90000

/*
 * INTC-A interrupt mask registers IMR0..IMR12 (0xA4080080, one byte every
 * four). A set bit masks the source, and a read returns the mask, so a
 * driver can mask one source with IMR |= bit. sh_intc keeps the enabled
 * sources instead, and its read would hand that back: the XDJ's LCDC driver
 * does IMR4 |= 0x01, which then masked the TMU0 tick along with the LCDC.
 * This overlay answers reads with the complement and passes writes through.
 */
#define IMR_BASE        0xA4080080
#define IMR_SIZE        0x34

static MemoryRegion imr_overlay;

static uint64_t imr_read(void *opaque, hwaddr off, unsigned size)
{
    uint64_t enabled = 0;

    if (off & 3) {
        return 0;
    }
    memory_region_dispatch_read(&cdj_intc.iomem, A7ADDR(IMR_BASE) + off,
                                &enabled, MO_UB, MEMTXATTRS_UNSPECIFIED);
    return ~enabled & 0xff;
}

static void imr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    memory_region_dispatch_write(&cdj_intc.iomem, A7ADDR(IMR_BASE) + off,
                                 val, MO_UB, MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps imr_ops = {
    .read = imr_read,
    .write = imr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void xdj1000_imr_init(MemoryRegion *sysmem)
{
    memory_region_init_io(&imr_overlay, NULL, &imr_ops, NULL,
                          "sh7724.intc-imr", IMR_SIZE);
    memory_region_add_subregion_overlap(sysmem, A7ADDR(IMR_BASE),
                                        &imr_overlay, 1);
}

/*
 * Two 16550-style UARTs, byte registers on a 4-byte pitch. The firmware
 * enables the FIFO and polls IIR for 0xC0 in bits 7, 6 and 4 before it uses
 * either one, and spins on that wait while they are absent.
 */
static const hwaddr xdj1000_uart_base[] = { 0xA4470000, 0xA4750000 };

static CdjBoardDesc xdj1000_board = {
    .name = "xdj1000",
    .dram_phys = CDJ_DRAM_PHYS,
    .dram_size = CDJ_DRAM_SIZE,
    /* 128 uniform sectors of 128 KiB: the firmware's sector table (0x0807DC54)
     * runs 0x0 to 0xFE0000 in steps of 0x20000. */
    .flash_phys = CDJ_FLASH_PHYS,
    .flash_size = 16 * MiB,
    .flash = { { 64, 128 * KiB }, { 64, 128 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2221, 0x2201 },
    .fw_entry = CDJ_FW_ENTRY,
    .init_sp = CDJ_INIT_SP,
    .init_sr = CDJ_INIT_SR,
    .periph_hz = 41666667,
    .ccn_trace_lo = 0x800,
    .ccn_trace_hi = 0x860,
    .exit_report = cdj_intc_exit_report,
};

/*
 * The decks built on this board. The XDJ-1000 and the CDJ-900NXS share the
 * SoC, the DSP link and the display engine; a deck that needs a different
 * peripheral value gets a field here.
 */
typedef struct Sh7724Deck {
    const char *name;
    const char *desc;
    hwaddr usb_base;
    int usb_irq;
    unsigned pnl_frame;
    int auth_iic;               /* IIC channel of the auth chip, or -1 */
} Sh7724Deck;

/*
 * The window's keys: the report bits that load and play a track on all three
 * decks (USB, seven rotary pushes, PLAY), laid out as on the CDJ-2000NXS.
 */
static const CdjGuiKey sh7724_deck_keys[] = {
    { Q_KEY_CODE_SPC,      0x10, 0x01, "PLAY/PAUSE",  true },
    { Q_KEY_CODE_RET,      0x11, 0x01, "ROTARY PUSH", true },
    { Q_KEY_CODE_KP_ENTER, 0x11, 0x01, "ROTARY PUSH", true },
    { Q_KEY_CODE_RIGHT,    0x11, 0x01, "ROTARY PUSH", true },
    { Q_KEY_CODE_U,        0x13, 0x04, "USB",         true },
};

static void sh7724_deck_init(MachineState *machine, const Sh7724Deck *deck)
{
    MemoryRegion *sysmem = get_system_memory();
    SuperHCPU *cpu;

    xdj1000_board.name = deck->name;
    cpu = cdj_board_init(machine, &xdj1000_board);
    qemu_irq dei[4];
    const CdjDspWires *dsp;

    cdj_intc_init(sysmem, cpu);
    xdj1000_imr_init(sysmem);
    cdj_unimp("sh7724.intc-b", 0xA4140000, 0x1000);
    cdj_unimp("sh7724.intc-c", 0xA4090000, 0x1000);
    cdj_unimp("sh7724.cmt",    0xA44A0000, 0x1000);
    cdj_unimp("sh7724.cpg",    0xA4150000, 0x1000);
    cdj_unimp("sh7724.flctl",  0xA4530000, 0x1000);
    cdj_unimp("sh7724.dbsc",   0xFEC10000, 0x1000);
    cdj_unimp("sh7724.bsc",    0xFF800000, 0x1000);
    cdj_unimp("sh7724.intc",   0xFFD00000, 0x1000);
    cdj_unimp("sh7724.msiof0", 0xA4C40000, 0x1000);
    cdj_unimp("sh7724.msiof1", 0xA4C50000, 0x1000);
    cdj_usb_init(sysmem, deck->usb_base, cdj_intc.irqs[deck->usb_irq], false);
    cdj_unimp("sh7724.misc-e4", 0xFFE40000, 0x1000);
    cdj_unimp("sh7724.ceu",    0xFE910000, 0x10000);
    cdj_unimp("sh7724.veu",    0xFE920000, 0x10000);
    cdj_unimp("sh7724.beu",    0xFE930000, 0x10000);
    xdj1000_lcdc_init(sysmem, cdj_count_irq(cdj_intc.irqs[CDJ_LCDCI], "LCDC LCDCI"));
    xdj1000_gfx_init(sysmem, cdj_count_irq(cdj_intc.irqs[CDJ_2DG_TRI], "2DG TRI"));
    cdj_unimp("sh7724.vou",    0xFE960000, 0x10000);
    cdj_unimp("sh7724.ceu-fea0", 0xFEA00000, 0x10000);
    cdj_unimp("sh7724.hpb",    0x04C00000, 0x100000);

    /* The DSP's host port is on area 6, four registers 256 KiB apart. */
    dsp = cdj_c6747_init(sysmem, CDJ_AREA6_PHYS);
    xdj1000_pfc_init(sysmem, dsp);

    cdj_ata_init(sysmem, "sh7724.atapi", A7ADDR(0xA4DA2100), NULL);
    cdj_ether_init(sysmem, "sh7724.ether", 0x04600000,
                   cdj_count_irq(cdj_intc.irqs[CDJ_ETHI], "EtherMAC ETHI"), 0);

    cdj_tmu_init(sysmem, 0x1FD80000,
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI0], "TMU0 TUNI0"),
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI1], "TMU0 TUNI1"),
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU0_TUNI2], "TMU0 TUNI2"));
    cdj_tmu_init(sysmem, 0x1FD90000,
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI0], "TMU1 TUNI0"),
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI1], "TMU1 TUNI1"),
                 cdj_count_irq(cdj_intc.irqs[CDJ_TMU1_TUNI2], "TMU1 TUNI2"));
    cdj_irqcount_exit.notify = cdj_irqcount_dump;
    qemu_add_exit_notifier(&cdj_irqcount_exit);

    dei[0] = cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI0], "DMAC0A DEI0");
    dei[1] = cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI1], "DMAC0A DEI1");
    dei[2] = cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI2], "DMAC0A DEI2");
    dei[3] = cdj_count_irq(cdj_intc.irqs[CDJ_DMAC0A_DEI3], "DMAC0A DEI3");
    cdj_dmac_init(sysmem, "sh7724.dmac", 0xFE008000, deck->usb_base,
                  CDJ_USB_SIZE,
                  (CdjDmacDei[]) { { dei[0] }, { dei[1] }, { dei[2] }, { dei[3] } });

    cdj_scif(sysmem, "scif0", CDJ_SCIF0_ADDR,
             serial_hd(0) ?: qemu_chr_new("scif0-null", "null", NULL));
    cdj_scif(sysmem, "scif1", CDJ_SCIF1_ADDR,
             serial_hd(1) ?: qemu_chr_new("scif1-null", "null", NULL));
    if (deck->auth_iic >= 0) {
        cdj_sh7724_iic_init(sysmem, deck->auth_iic);
    } else {
        for (int i = 0; i < ARRAY_SIZE(xdj1000_uart_base); i++) {
            g_autofree char *null_id = g_strdup_printf("uart%d-null", i);

            serial_mm_init(sysmem, A7ADDR(xdj1000_uart_base[i]), 2, NULL,
                           115200,
                           serial_hd(2 + i) ?: qemu_chr_new(null_id, "null",
                                                            NULL),
                           DEVICE_LITTLE_ENDIAN);
        }
    }
    cdj_pnl_link_init(sysmem, CDJ_SCIF2_ADDR, "sh7724.scif2-panel",
                      deck->pnl_frame, SH7724_PNL_SYNC, NULL, 0);
    cdj_gui_keys_init(sh7724_deck_keys, ARRAY_SIZE(sh7724_deck_keys),
                      deck->name);

    cdj_board_load(machine);
    cdj_pcring_exit.notify = cdj_pcring_dump;
    qemu_add_exit_notifier(&cdj_pcring_exit);
    cdj_board_start(cpu);
}

static const Sh7724Deck xdj1000_deck = {
    .name = "xdj1000",
    .desc = "Pioneer XDJ-1000 (Renesas SH7724), bring-up",
    .usb_base = XDJ1000_USB_BASE,
    .usb_irq = CDJ_USB1,
    .pnl_frame = 32,
    .auth_iic = -1,
};

static const Sh7724Deck cdj900nxs_deck = {
    .name = "cdj900nxs",
    .desc = "Pioneer CDJ-900NXS (Renesas SH7724), bring-up",
    .usb_base = CDJ_USB_BASE,
    .usb_irq = CDJ_USB0,
    .pnl_frame = 24,
    .auth_iic = 0,
};

static const Sh7724Deck xdj700_deck = {
    .name = "xdj700",
    .desc = "Pioneer XDJ-700 (Renesas SH7724), bring-up",
    .usb_base = CDJ_USB_BASE,
    .usb_irq = CDJ_USB0,
    .pnl_frame = 32,
    .auth_iic = 0,
};

static void xdj1000_init(MachineState *machine)
{
    sh7724_deck_init(machine, &xdj1000_deck);
}

static void xdj700_init(MachineState *machine)
{
    sh7724_deck_init(machine, &xdj700_deck);
}

static void cdj900nxs_init(MachineState *machine)
{
    sh7724_deck_init(machine, &cdj900nxs_deck);
}

static void sh7724_deck_machine_init(MachineClass *mc, const Sh7724Deck *deck,
                                     void (*init)(MachineState *))
{
    mc->desc = deck->desc;
    mc->init = init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = CDJ_DRAM_SIZE;
}

static void xdj1000_machine_init(MachineClass *mc)
{
    sh7724_deck_machine_init(mc, &xdj1000_deck, xdj1000_init);
}

static void cdj900nxs_machine_init(MachineClass *mc)
{
    sh7724_deck_machine_init(mc, &cdj900nxs_deck, cdj900nxs_init);
}

static void xdj700_machine_init(MachineClass *mc)
{
    sh7724_deck_machine_init(mc, &xdj700_deck, xdj700_init);
}

DEFINE_MACHINE("xdj1000", xdj1000_machine_init)
DEFINE_MACHINE("cdj900nxs", cdj900nxs_machine_init)
DEFINE_MACHINE("xdj700", xdj700_machine_init)
