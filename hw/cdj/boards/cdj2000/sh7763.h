/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SH7763's interrupt sources, for the CDJ-2000 / CDJ-2000NXS board. */
#ifndef CDJ_SH7763_H
#define CDJ_SH7763_H
#include "cdj_common.h"

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
    S63_USBH, S63_USBF0, S63_USBF1,
    S63_MMCIF0, S63_MMCIF1, S63_MMCIF2, S63_MMCIF3,
    S63_GPIO0, S63_GPIO1, S63_GPIO2, S63_GPIO3,
    S63_IRQ0, S63_IRQ1, S63_IRQ2, S63_IRQ3, S63_IRQ4, S63_IRQ5, S63_IRQ6, S63_IRQ7,
    /* groups: one mask bit and one priority field each */
    S63_TMU012, S63_TMU345, S63_DMAC, S63_SCIF0, S63_SCIF1, S63_SCIF2,
    S63_GETHER, S63_RTC, S63_USBF, S63_MMCIF, S63_GPIO,
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

void cdj2000_latch_init(MemoryRegion *sysmem, const CdjDspWires *dsp);

/* The CDJ-2000NXS's C6747-class DSP, behind its host port on MAIN's area 3;
 * returns its end of the latch wires. */
const CdjDspWires *cdj_c6747_init(MemoryRegion *sysmem, hwaddr hpi_base);
/* The CDJ-2000's C6727, behind a full-address host port on area 3. */
const CdjDspWires *cdj_c6727_init(MemoryRegion *sysmem, hwaddr hpi_base);
#endif
