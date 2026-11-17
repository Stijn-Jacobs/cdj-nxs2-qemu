# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the real DSP interpreter with synthetic instructions and stub I/O."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

COMMON = r"""
#ifndef DSP_TEST_COMMON
#define DSP_TEST_COMMON
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
typedef struct MemoryRegion MemoryRegion;
typedef struct SuperHCPU SuperHCPU;
typedef struct QEMUTimer { int unused; } QEMUTimer;
#define QEMU_CLOCK_VIRTUAL 0
#define g_new0(t, n) ((t *)calloc(n, sizeof(t)))
#define info_report(...) ((void)0)
static int64_t qemu_clock_get_ns(int c) { return 0; }
static QEMUTimer *timer_new_ns(int c, void (*cb)(void *), void *o) { return NULL; }
static void timer_mod(QEMUTimer *t, int64_t ns) {}
#endif
"""
AUDIO = r"""
typedef struct CdjHostAudio { int unused; } CdjHostAudio;
typedef struct CdjHostAudioCfg { const char *env, *voice, *label, *source; } CdjHostAudioCfg;
static unsigned audio_frames;
static CdjHostAudio *cdj_host_audio_open(const CdjHostAudioCfg *cfg) { return NULL; }
static void cdj_host_audio_put(CdjHostAudio *a, int16_t l, int16_t r) { audio_frames++; }
"""
HARNESS = r"""
#include "DSP_SOURCE"
static bool pin7;
static uint16_t main_pfdrh, main_pfdrl;
uint16_t cdj850_port_pfdrh(void) { return main_pfdrh; }
uint16_t cdj850_port_pfdrl(void) { return main_pfdrl; }
void cdj850_intc_set_pin(unsigned irq, bool level) { pin7 = level; }
bool cdj850_intc_get_pin(unsigned irq) { return pin7; }

static Dsp56300 *fresh(void)
{
    Dsp56300 *d = calloc(1, sizeof(*d));
    for (int i = 0; i < 8; i++) { d->m[i] = DSP_MASK24; }
    dsp = d;
    d->boot_state = BOOT_RUN;
    return d;
}

int main(int argc, char **argv)
{
    Dsp56300 *d = fresh();
    /* MOVEP reads HRX rather than R4, and writes HTX rather than a control register. */
    d->r[4] = 0x123;
    fifo_push(&d->shi_rx, 0xc28000);
    d->p[0] = 0x044494;
    step_one(d);
    assert(d->x0 == 0xc28000 && !d->shi_rx.count);
    d->p[1] = 0x04c493;
    step_one(d);
    assert(dsp56300_shi_rx() == 0xc2);
    assert(dsp56300_shi_rx() == 0x80);
    assert(dsp56300_shi_rx() == 0x00);
    assert(io_read_x(d, 0xffff91) & (1u << 15));
    assert(!(io_read_x(d, 0xffff91) & (1u << 19)));
    /* MOVEC transfers in both directions, including control-to-control. */
    d->p[2] = 0x0461a0; /* M0 -> M1 */
    step_one(d);
    assert(d->m[1] == DSP_MASK24);
    d->r[0] = 0x42;
    d->p[3] = 0x04d0a0; /* R0 -> M0 */
    step_one(d);
    assert(d->m[0] == 0x42);
    /* ESAI control mapping and documented status bit positions. */
    io_write_x(d, 0xffffb5, 0x400001);
    assert(d->x_io[XIO_ESAI_TCR] == 0x400001);
    io_write_x(d, 0xffffb7, 3);
    assert(d->x_io[XIO_ESAI_RCR] == 3);
    assert(io_read_x(d, 0xffffb3) & (1u << 15));
    assert(!(io_read_x(d, 0xffffb3) & (1u << 14)));
    /* A vector JSR stacks the interrupted PC exactly once. */
    d->pc = 0x100;
    d->p[0x38] = 0x0bf080;
    d->p[0x39] = 0x200;
    d->p[0x200] = 0x000004; /* RTI */
    d->p[0x100] = 0x0c0100; /* park */
    d->sr = SR_LF | SR_S0;
    dsp_fire_irq(d, 0x38);
    assert(d->pc == 0x100 && d->sp == 0 && d->sr == (SR_LF | SR_S0));
    free(d);
    d = fresh();
    /* Short fractional immediates differ from integer register portions. */
    d->p[0] = 0x248000; /* #$80 -> X0 */
    d->p[1] = 0x2c0400; /* #4 -> A1 */
    step_one(d); step_one(d);
    assert(d->x0 == 0x800000 && d->a == 0x4000000);
    d->p[2] = 0x020040; /* TCC X0,A */
    d->sr &= ~SR_C;
    step_one(d);
    assert(d->a == pack_acc24(0x800000));
    /* Signed short branch bits are split; $57 encodes +$37. */
    d->pc = 0x100;
    d->sr = SR_N;
    d->p[0x100] = 0x059457;
    step_one(d);
    assert(d->pc == 0x137);
    /* A peripheral bit branch preserves the incoming carry. */
    d->pc = 0;
    d->sr = SR_C;
    d->p[0] = 0x019191; d->p[1] = 4;
    step_one(d);
    assert(d->pc == 4 && (d->sr & SR_C));
    /* Main's PLAY GPIO (PF16 = PFDRH bit 0) is wired to DSP PF10. */
    assert(!(io_read_y(d, 0xfffff5) & 0x400));
    main_pfdrh = 1u;
    assert(io_read_y(d, 0xfffff5) & 0x400);
    /* A dual write takes ALU source values before the arithmetic operation. */
    d->r[0] = 100; d->r[4] = 200; d->x0 = 0x123; d->y0 = 0x456;
    d->pc = 0; d->p[0] = 0xb01813;
    step_one(d);
    assert(d->x[100] == 0x123 && d->y[200] == 0x456);
    assert(d->r[0] == 101 && d->r[4] == 201);
    /* Seven-bit displacement addressing is linear and leaves Rn unchanged. */
    d->r[1] = 100; d->b = pack_acc24(0x123456);
    d->pc = 0; d->p[0] = 0x03f9ef;
    step_one(d);
    assert(d->y[99] == 0x123456 && d->r[1] == 100);
    /* Arithmetic shifts independently select source and destination. */
    d->b = pack_acc24(8); d->a = 0;
    d->pc = 0; d->p[0] = 0x0c1c82;
    step_one(d);
    assert(d->a == pack_acc24(4) && d->b == pack_acc24(8));
    /* REP #0 performs 65536 iterations without recursive execution. */
    d->pc = 0; d->p[0] = 0x0600a0; d->p[1] = 0x000008;
    d->a = 0; d->lc = 77;
    step_one(d);
    for (unsigned i = 0; i < 65536; i++) { step_one(d); }
    assert(d->a == 65536 && d->pc == 2 && d->lc == 77);
    /* DO ends on the last word, even when it is an extension word. */
    d->pc = 0; d->p[0] = 0x060280; d->p[1] = 3;
    d->p[2] = 0x44f400; d->p[3] = 0x123456;
    d->sr = 0; d->la = 10; d->lc = 20;
    step_one(d); step_one(d); step_one(d);
    assert(d->pc == 4 && d->sp == 0 && d->la == 10 && d->lc == 20);
    /* No enabled IRQ, no firmware TX0 writes: host frame count remains zero. */
    d->audio_sink = (CdjHostAudio *)1;
    d->boot_state = BOOT_RUN;
    d->x_io[XIO_ESAI_TCR] = 1;
    d->pc = 0; d->p[0] = 0x0c0000;
    dsp_audio_tick(d);
    assert(audio_frames == 0);
    free(d);
    return 0;
}
"""


def test_dsp_instruction_and_peripheral_paths(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    (tmp_path / "cdj_common.h").write_text(COMMON)
    (tmp_path / "cdj_host_audio.h").write_text(AUDIO)
    (tmp_path / "qemu").mkdir()
    (tmp_path / "qemu/timer.h").write_text("")
    source = ROOT / "hw/cdj/boards/cdj850/dsp56300.c"
    (tmp_path / "harness.c").write_text(HARNESS.replace("DSP_SOURCE", str(source)))
    exe = tmp_path / "dsp-test"
    subprocess.run([cc, "-std=gnu11", "-O1", "-I", str(tmp_path),
                    str(tmp_path / "harness.c"), "-o", str(exe)], check=True,
                   capture_output=True, text=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
