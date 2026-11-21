/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "../dsp56300.h"
#include "dsp_shi.h"
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
 * This file observes the framing and logs each segment under CDJ850_TRACE.
 * All bytes are forwarded to dsp56300.c. The interpreter executes the
 * uploaded program and drives DSPDREQ through its Port E GPIO model;
 * there is no bootstrap handshake HLE.

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
} shi;

static void dsp_shi_segment_done(void)
{
    bool reenter = shi.prev == DSP_JMP_ABS && shi.last == DSP_BOOT_ROM;

    shi.segs++;
    if (cdj850_trace()) {
        info_report("cdj850: dsp shi segment %u: %u words @P:%06x, %s "
                    "(%" PRIu64 " ms)", shi.segs, shi.count, shi.addr,
                    reenter ? "re-enters bootstrap ROM" : "application",
                    cdj850_vms());
    }
    shi.phase = reenter ? SHI_COUNT : SHI_RUNNING;
}

uint8_t cdj850_dsp_shi_rx(void)
{
    return dsp56300_shi_rx();
}

void cdj850_dsp_shi_tx(uint8_t b)
{
    uint32_t w;

    dsp56300_shi_tx(b);
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

static const VMStateDescription vmstate_cdj850_dsp_shi = {
    .name = "cdj850-dsp-shi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        CDJ_VMSTATE_SPAN(typeof(shi), acc, segs),
        VMSTATE_END_OF_LIST()
    }
};

static void dsp_shi_reset(void *opaque)
{
    (void)opaque;
    memset(&shi, 0, sizeof(shi));
}

void cdj850_dsp_shi_init(void)
{
    qemu_register_reset(dsp_shi_reset, NULL);
    vmstate_register_any(NULL, &vmstate_cdj850_dsp_shi, &shi);
}
