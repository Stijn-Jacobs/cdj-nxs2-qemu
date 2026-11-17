/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "dsp_shi.h"
#include "intc.h"
#include "trace.h"

/*
 * Audio DSP (IC301, DSP56371) bootstrap over SHI, seen from MAIN's SSU0.
 *
 * SSU channel 0 is the DSP's serial host interface, not the panel link:
 * service manual 10.2 MAIN ASSY 1/4 routes IC103 pin 169 PF0/SSCK0 to
 * AUDIO_SCK, 167 PF1/SSI0 to AUDIO_SI, 166 PF2/SSO0 to AUDIO_SO and 165
 * PF3/SCS0 to DSPCS. The DSP is strapped to boot mode 5 (SHI slave, SPI),
 * where the bootstrap ROM reads 24-bit words: a word count, a load
 * address, then that many program words, and jumps to the load address.
 *
 * The 1140 bytes the boot driver at 0x0C077C3C sends are exactly that
 * framing. MAIN holds four segments, back to back in its image, listed
 * in the table at 0x0C1C1AE8 (pointer, expected DSPDREQ level, retries):
 *   1  0x0C155B28     9 words @P:0, ends JMP $FF0000   DSPDREQ 0
 *   2  0x0C155B49    82 words @P:0, ends JMP $FF0000   DSPDREQ 1
 *   3  0x0C155C45   896 words @P:0, ends JMP $FF0000   DSPDREQ 0
 *   4  0x0C1566CB  3824 words @P:0 (the application)   DSPDREQ 1
 * JMP $FF0000 re-enters the bootstrap ROM, so each stage hands back to the
 * loader for the next one. After each segment the driver waits, samples
 * PE11 (DSPDREQ, PEPRL bit 11 at 0x0C077E9A) and compares it with the
 * table; a mismatch resends the segment and gives up after four tries.
 * With the pin stuck at either level the upload never gets past segment
 * 2, which is why only segments 1 and 2 were ever seen on the wire.
 *
 * This file parses the framing, always (it only observes), and logs each
 * segment under CDJ850_TRACE. With CDJ850_DSP_BOOT_HLE=1 (default OFF) it
 * also answers the handshake: DSPDREQ toggles when a segment completes,
 * starting from its pulled-up idle high, which yields the 0,1,0,1 the
 * table expects. That is a stand-in for the DSP code doing it (each stage
 * drives the pin itself); the levels come from MAIN's table, not from
 * executing the uploaded program, and nothing after the bootstrap is
 * answered. A DSP56300 core running the four segments replaces it.
 */
#define DSP_JMP_ABS     0x0AF080
#define DSP_BOOT_ROM    0xFF0000

static struct {
    uint32_t acc;
    unsigned nbytes;
    enum { SHI_COUNT, SHI_ADDR, SHI_DATA, SHI_RUNNING } phase;
    uint32_t count, left, addr;
    uint32_t prev, last;
    unsigned segs;
    int hle;
} shi = { .hle = -1 };

static void dsp_shi_segment_done(void)
{
    bool reenter = shi.prev == DSP_JMP_ABS && shi.last == DSP_BOOT_ROM;

    shi.segs++;
    if (shi.hle < 0) {
        const char *e = getenv("CDJ850_DSP_BOOT_HLE");

        shi.hle = e && *e == '1';
    }
    if (cdj850_trace() || shi.hle) {
        info_report("cdj850: dsp shi segment %u: %u words @P:%06x, %s "
                    "(%" PRIu64 " ms)", shi.segs, shi.count, shi.addr,
                    reenter ? "re-enters bootstrap ROM" : "application",
                    cdj850_vms());
    }
    if (shi.hle) {
        cdj850_intc_set_pin(7, !cdj850_intc_get_pin(7));
    }
    shi.phase = reenter ? SHI_COUNT : SHI_RUNNING;
}

void cdj850_dsp_shi_tx(uint8_t b)
{
    uint32_t w;

    if (shi.phase == SHI_RUNNING) {
        return;
    }
    shi.acc = (shi.acc << 8) | b;
    if (++shi.nbytes < 3) {
        return;
    }
    w = shi.acc & 0xFFFFFF;
    shi.acc = 0;
    shi.nbytes = 0;

    switch (shi.phase) {
    case SHI_COUNT:
        /* PRAM is far smaller than this; anything larger is not a
         * bootstrap header (e.g. a 16-bit command pair), so skip it. */
        if (w > 0x10000) {
            break;
        }
        shi.count = shi.left = w;
        shi.phase = SHI_ADDR;
        break;
    case SHI_ADDR:
        shi.addr = w;
        shi.prev = shi.last = 0;
        shi.phase = shi.left ? SHI_DATA : SHI_COUNT;
        break;
    case SHI_DATA:
        shi.prev = shi.last;
        shi.last = w;
        if (--shi.left == 0) {
            dsp_shi_segment_done();
        }
        break;
    default:
        break;
    }
}
