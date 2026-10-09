/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj2000/sh7763.h"
#include "display_m16c.h"
/*
 * Pioneer CDJ-900 MAIN board (Renesas SH7764, R5S77641) -- bring-up.
 *
 * Its MAIN (update v4.32) is the CDJ-2000's (v4.33) rebuilt for this deck:
 * the same flash layout, entry, memory map and peripheral addresses, and the
 * same two C6727 images byte for byte at 0x04001000. So the board is the
 * CDJ-2000's; what differs is where the linker put the firmware's globals.
 * The GUI processor is an M16C/63, not the CDJ-2000's BF531 (display_m16c.c):
 * it shares SCIF2 with the panel, and MAIN boots without a display unless
 * CDJ_M16C_GUI names its flash image.
 */

/* MAIN's exchange routine (0x04247F44) addresses device 0 by clearing bits 4
 * and 2 of the latch register at +0x48 (and the display driver at 0x0424A976
 * clears bit 2 the same way), and device 1, the panel, by setting bit 4; each
 * deselects by setting its own chip-select bit again. */
#define LATCH_LINK  0x48
#define LINK_PANEL  (1u << 4)
#define LINK_GUI_CS (1u << 2)

static bool cdj900_gui_selected(void)
{
    return !(cdj2000_latch_get(LATCH_LINK) & (LINK_PANEL | LINK_GUI_CS));
}

/* The chip reads the same wire on its P6 bit 4: high while MAIN has not
 * selected it, which is when its link restart (0x0CDFD0 in the GUI image)
 * re-arms the receive and transmit DMA for the next frame. */
bool cdj900_gui_link_idle(void)
{
    return cdj2000_latch_get(LATCH_LINK) & LINK_GUI_CS;
}

/* MAIN's answer handler (0x0421543x) reads the chip's answer-valid output as
 * bit 3 of the same latch register. */
#define LINK_GUI_ANSWER (1u << 3)

static void cdj900_latch_watch(unsigned off)
{
    if (off == LATCH_LINK) {
        cdj900_gui_link_end();
        cdj900_gui_sync();
    }
}

static const Cdj2000Display cdj900_display = {
    .init = cdj900_gui_init,
    .link_selected = cdj900_gui_selected,
    .link_byte = cdj900_gui_link_byte,
};

static const CdjBoardDesc cdj900_board = {
    .name = "cdj900",
    .dram_phys = 0x04000000,
    .dram_size = 64 * MiB,
    /* Sector tables at 0x040697AC and 0x040698C8, the CDJ-2000's 71
     * sectors. */
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .fw_entry = 0xA4000800,
    .init_sp = 0xA7FFFFC8,
    .init_sr = 0x500000F0,
    /* Written at 0x042EB9D0, restored at 0x042EB9E0 and 0x042EB9EC. */
    .sr_seed_slot = 0x04FC287C,
    .sr_seed = 0x400000F0,
    .periph_hz = 54000000,
    .ccn_trace_lo = 0x640,
    .ccn_trace_hi = 0x6A0,
    .exit_report = cdj_sh7763_intc_report,
};

static void cdj900_init(MachineState *machine)
{
    cdj2000_latch_watch(cdj900_latch_watch);
    cdj2000_latch_answer_pin(cdj900_gui_answer_valid, LINK_GUI_ANSWER);
    sh7763_board_init(machine, &cdj900_board,
                      cdj_c6727_init,
                      &cdj900_display, false);
}

static void cdj900_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-900 (Renesas SH7764), bring-up";
    mc->init = cdj900_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;
    mc->default_ram_size = 64 * MiB;
}

DEFINE_MACHINE("cdj900", cdj900_machine_init)
