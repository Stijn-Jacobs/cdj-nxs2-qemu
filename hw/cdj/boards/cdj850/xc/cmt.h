/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 interrupt + compare-match timer core (xc/cmt.c). */
#ifndef CDJ850_CMT_H
#define CDJ850_CMT_H
#include "cdj_common.h"

void cdj850_cmt_init(MemoryRegion *sysmem, SuperHCPU *cpu);
/* Queue an interrupt request: SH-2A vector number and priority level. */
void cdj850_irq_raise(SuperHCPU *cpu, uint16_t vec, uint8_t level);
/* Level-sensitive sources: is a request for vec queued, or presented and
 * not yet taken? Withdraw it when the source's line drops. */
bool cdj850_irq_pending(uint16_t vec);
void cdj850_irq_withdraw(SuperHCPU *cpu, uint16_t vec);
#endif
