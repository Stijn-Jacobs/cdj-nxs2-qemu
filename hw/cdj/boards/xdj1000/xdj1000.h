/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XDJ1000_H
#define XDJ1000_H
#include "../nxs2/cdj.h"
#include "../cdj2000/sh7763.h"

/* The pin function controller, with MAIN's two wires to the DSP on it. */
void xdj1000_pfc_init(MemoryRegion *sysmem, const CdjDspWires *dsp);
void xdj1000_lcdc_init(MemoryRegion *sysmem, qemu_irq irq);
void xdj1000_gfx_init(MemoryRegion *sysmem, qemu_irq gfx2d_irq);

#endif
