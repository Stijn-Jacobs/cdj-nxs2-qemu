/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the browser page reads from MAIN beyond common/cdj_web.c: the
 * instruction count and the DSP's cycle counter and boot line.
 */
#include "cdj.h"
#include "exec/icount.h"
#include <emscripten.h>

/* Guest instructions executed so far; 0 without -icount. */
EMSCRIPTEN_KEEPALIVE double cdj_wasm_insns(void)
{
    return icount_enabled() ? (double)icount_get_raw() : 0;
}

EMSCRIPTEN_KEEPALIVE double cdj_wasm_dsp_cycles(void)
{
    return cdj_c6x.core ? (double)c66x_get_cycle(cdj_c6x.core) : 0;
}

/* 1 once the DSP has pulled GPIO25 low ("booted") since its last reset. */
EMSCRIPTEN_KEEPALIVE int cdj_wasm_dsp_booted(void)
{
    return qatomic_read(&cdj_c6x.booted_latch);
}
