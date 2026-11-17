/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared bring-up logging helpers for the CDJ-850 xc peripheral models. */
#ifndef CDJ850_TRACE_H
#define CDJ850_TRACE_H
#include "cdj_common.h"

/* CDJ850_TRACE=1 logs every access to the modelled devices with the guest
 * PC and virtual time, for bring-up gap analysis. */
static inline bool cdj850_trace(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *e = getenv("CDJ850_TRACE");

        trace = e && *e != '0';
    }
    return trace;
}

static inline uint64_t cdj850_vms(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
}
#endif
