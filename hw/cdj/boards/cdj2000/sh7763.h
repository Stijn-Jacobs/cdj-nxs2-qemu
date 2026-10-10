/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SH7763's interrupt sources, for the CDJ-2000 / CDJ-2000NXS board. */
#ifndef CDJ_SH7763_H
#define CDJ_SH7763_H
#include "cdj_auth_chip.h"
#include "cdj_common.h"
#include "cdj_gui_keys.h"

/* One source per interrupt event: sh_intc keeps one vector per source, so a
 * module's events are separate sources and its group shares their mask bit
 * and priority field. */
enum {
    S63_NONE = 0,
    S63_TMU0, S63_TMU1, S63_TMU2, S63_TMU2_TICPI,
    S63_TMU3, S63_TMU4, S63_TMU5,
    S63_DMTE0, S63_DMTE1, S63_DMTE2, S63_DMTE3, S63_DMAE, S63_DMTE4, S63_DMTE5,
    S63_SCIF0_ERI, S63_SCIF0_RXI, S63_SCIF0_BRI, S63_SCIF0_TXI,
    S63_SCIF1_ERI, S63_SCIF1_RXI, S63_SCIF1_BRI, S63_SCIF1_TXI,
    S63_SCIF2_ERI, S63_SCIF2_RXI, S63_SCIF2_BRI, S63_SCIF2_TXI,
    S63_GETHER0, S63_GETHER1, S63_GETHER2,
    S63_RTC0, S63_RTC1, S63_RTC2, S63_WDT,
    S63_IIC0, S63_IIC1, S63_CMT,
    S63_USBH, S63_USBF0, S63_USBF1, S63_ATAPI,
    S63_MMCIF0, S63_MMCIF1, S63_MMCIF2, S63_MMCIF3,
    S63_GPIO0, S63_GPIO1, S63_GPIO2, S63_GPIO3,
    S63_IRQ0, S63_IRQ1, S63_IRQ2, S63_IRQ3, S63_IRQ4, S63_IRQ5, S63_IRQ6, S63_IRQ7,
    S63_DISP_RX_DMA, S63_DISP_RX_SER, S63_DISP_TX_DMA, S63_DISP_TX_SER,
    /* groups: one mask bit and one priority field each */
    S63_TMU012, S63_TMU345, S63_DMAC, S63_SCIF0, S63_SCIF1, S63_SCIF2,
    S63_GETHER, S63_RTC, S63_USBF, S63_MMCIF, S63_GPIO, S63_DISP_TX,
    S63_NR_SOURCES
};

extern struct intc_desc cdj_sh7763_intc;
void cdj_sh7763_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu);
void cdj_sh7763_intc_report(void);

/* The wires between the board's port latch and a DSP: MAIN drives a 2-bit
 * command, the DSP answers on a busy line. */
typedef struct CdjDspWires {
    void *opaque;
    void (*command)(void *opaque, unsigned bits);
    bool (*busy)(void *opaque);
} CdjDspWires;

void cdj2000_latch_init(MemoryRegion *sysmem, const CdjDspWires *dsp,
                        bool display);
/* The 16-bit latch register at off, as MAIN last wrote it. */
uint16_t cdj2000_latch_get(unsigned off);
/* Called with the register's offset just before every latch write, while
 * the old value is still readable. */
void cdj2000_latch_watch(void (*fn)(unsigned off));
/* A display processor that is not the BF531 answers on its own pin: latch
 * +0x48 reads level() in the bits of mask instead of the BF531's PF1. */
void cdj2000_latch_answer_pin(bool (*level)(void), uint16_t mask);

/* The CDJ-2000NXS's C6747-class DSP, behind its host port on MAIN's area 3;
 * returns its end of the latch wires. */
const CdjDspWires *cdj_c6747_init(MemoryRegion *sysmem, hwaddr hpi_base);
/* The DSP application's polling main loop, which the core skips while it is
 * a fixed point (c66x_set_idle_loop): its head and the stack range its
 * passes may store to. Per firmware image; none by default. */
void cdj_c6747_set_idle_loop(uint32_t head, uint32_t stack_lo,
                             uint32_t stack_hi);
/* The CDJ-2000's C6727, behind a full-address host port on area 3. */
const CdjDspWires *cdj_c6727_init(MemoryRegion *sysmem, hwaddr hpi_base);
/* As cdj_c6747_set_idle_loop, for the C6727. */
void cdj_c6727_set_idle_loop(uint32_t head, uint32_t stack_lo,
                             uint32_t stack_hi);

typedef struct Cdj2000Display Cdj2000Display;

/* The front-panel MCU link on SCIF2: a 24-byte DMA-fed exchange, the NXS2's
 * protocol at a shorter frame length and with no touch screen. The idle frame
 * reports the DIRECTION lever in its FWD position. A display with a
 * link_byte shares the port: its bytes bypass the panel. */
void cdj2000_panel_init(MemoryRegion *sysmem, hwaddr addr,
                        const Cdj2000Display *display);

/* The ADSP-BF531 display processor on the other end of SPORT1. Does nothing
 * and returns false unless CDJ_BF531_UPD names a GUI image to boot it with. */
struct Cdj2000Display {
    uint32_t sdram_size;
    const CdjGuiKey *keys;
    size_t key_count;
    /* A board whose display processor is not the BF531 (the CDJ-900's
     * M16C/63) starts its own in init, and shares the panel's SCIF2: the
     * port goes to link_byte while link_selected says so. */
    bool (*init)(void);
    bool (*link_selected)(void);
    uint8_t (*link_byte)(uint8_t tx);
    /* The firmware reads report byte 0x0F bit 0x02 as the DIRECTION lever in
     * its FWD position (1 = forward); see panel.c. */
    bool lever_fwd;
};

extern const Cdj2000Display cdj2000_display, cdj2000nxs_display;
bool cdj2000_display_init(const Cdj2000Display *desc);
/* One MAIN packet for it: the 64-byte fixed part or an extension part. */
void cdj2000_display_send(const uint8_t *pkt, size_t len);
bool cdj2000_display_pf1(void);

/* MAIN's end of that link, the 0xFF400000 (receive) and 0xFF500000
 * (transmit) DMA blocks; the display's answers arrive through
 * cdj2000_display_link_receive(). */
void cdj2000_display_link_init(MemoryRegion *sysmem, qemu_irq rx, qemu_irq tx,
                               qemu_irq tx_ser);
void cdj2000_display_link_receive(const uint8_t *pkt, size_t len);

/* The whole SH7763-family MAIN board around a model's desc, DSP and display;
 * the CDJ-900 (boards/cdj900/) builds on it. */
void sh7763_board_init(MachineState *machine, const CdjBoardDesc *desc,
                       const CdjDspWires *(*dsp_init)(MemoryRegion *, hwaddr),
                       const Cdj2000Display *display_desc, bool auth_chip);

/* The SDHI at 0xFFE40000, with no card in the slot. */
void cdj2000_sdhi_init(MemoryRegion *sysmem);

/* The I2C master at 0xFFE70000 with the authentication chip on it; answers
 * is the chip's per-model reply table (see cdj_auth_chip.h). */
void cdj2000_iic_init(MemoryRegion *sysmem,
                      const CdjAuthAnswer *answers, unsigned n);
#endif
