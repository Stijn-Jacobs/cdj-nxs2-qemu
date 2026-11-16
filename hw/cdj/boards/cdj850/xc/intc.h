/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 interrupt controller, external IRQ0-7 part (xc/intc.c). */
#ifndef CDJ850_INTC_H
#define CDJ850_INTC_H
#include "cdj_common.h"

void cdj850_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu);

/* Drive the pin routed to IRQn (0-7). Edges are latched per ICR1. */
void cdj850_intc_set_pin(unsigned irq, bool level);
bool cdj850_intc_get_pin(unsigned irq);
#endif
