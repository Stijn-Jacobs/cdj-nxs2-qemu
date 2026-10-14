/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "../dsp56371.h"
#include "port.h"
#include "trace.h"

/*
 * Ports/PFC at 0xFFFE3800: plain storage so configuration readbacks and
 * read-modify-writes see what was written. Undriven input pins read back
 * the last-written value (power-on zero), except the panel-handshake input
 * below, which the firmware parks on.
 *
 * +0x11E panel-handshake input, bit 2 = peer ready (strapped high).
 * Boot parks in an infinite loop at 0x0C08E8F2 (Task 6e): movi20 #0xE391E
 * sign-extends to r0 = 0xFFFE391E, then mov.w @(r0,r11),r6 with r11 = 0
 * reads this word and waits for bit 2 (extu.w; shar; shar; bld #0,r2;
 * r0 = 0xFFFE391E, r11 = 0; CDJ850_TRACE shows the silent
 * port read +0x11e = 0x0 at pc=0x0c08e8f2 from 1587 ms on. The +0x11E
 * word is never written (only the +0x110..+0x116 pin-function neighbours
 * are configured and read back), so the stored zero never releases it.
 *
 * The driver around the loop (0x0C08E8AE, device id 1; sibling 0x0C08E842
 * for id 0 never runs this boot) disables interrupts, writes 0xAA/0x55
 * probe patterns to void 0x20000xxx scratch, restores the status register,
 * delays, then waits for this bit and stores the outcome to SDRAM
 * 0x0CEE34AA plus a flag byte: a send-pattern-and-wait-for-ACK handshake
 * with a peer CPU. The service manual's signal block diagram wires exactly
 * such a link between MAIN (IC103) and the panel CPU (IC1007): TSI/TSO/TSCK
 * serial plus PNL_XBUSY/MAIN_XBUSY handshake GPIOs (MAIN-to-panel connector
 * CN2003). The level is what the driver's own comparison dictates (wait
 * for 1 = peer idle/ready, matching the active-low "XBUSY" naming); no data
 * bytes are invented, and the panel protocol itself stays Task 7's job.
 * The exact SH7263 pin name for +0x11E is unchecked (no hardware manual
 * on file); if a later gap implicates this bit, suspect the mapping first.
 */
#define CDJ850_PORT_BASE    0xFFFE3800
#define CDJ850_PORT_SIZE    0x800
/* The firmware reads the handshake word at +0x11E (big-endian: +0x11E is
 * the high byte, +0x11F the low byte) and waits for bit 2 of the word,
 * i.e. bit 2 of the +0x11F byte (self-caught off-by-one: strapping +0x11E
 * reads back 0x0400, which the shar-shar-bld sequence still sees as 0). */
#define CDJ850_PORT_READY_OFF 0x11F
#define CDJ850_PORT_READY_BIT 0x04

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_PORT_SIZE];
} Cdj850Port;

static uint64_t cdj850_port_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Port *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_PORT_SIZE - 1)];
    }
    /* The handshake input reflects the peer, not the output latch: reads
     * OR in the strapped-ready level on top of the stored value. */
    for (i = 0; i < size; i++) {
        if (((off + i) & (CDJ850_PORT_SIZE - 1)) == CDJ850_PORT_READY_OFF) {
            v |= (uint64_t)CDJ850_PORT_READY_BIT << ((size - 1 - i) * 8);
        }
    }
    /* Servo-gated input levels (Task 8n): PE6/PE8/SRVBUS0 READY only
     * with $CDJ850_SERVO_READY=1 once the peer is alive; SRVBUS nibble
     * with $CDJ850_SERVO_ACK at the sampler PCs. Default OFF = plain
     * loopback/storage (servo state unmasked). Legacy DSP REPLY/PULSE/
     * SEQ knobs remain compat-only; see dsp56371.c (8m: Port-E stream
     * is servo init, not DSP upload). */
    v = cdj850_dsp_adjust_read(off, size, v, s->cpu->env.pc);
    if (cdj850_trace()) {
        info_report("cdj850: port read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_port_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Port *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (CDJ850_PORT_SIZE - 1)] = val >> (8 * i);
    }
    /* Snoop for the DSP bootstrap tracker + upload record. */
    cdj850_dsp_snoop_write(off, val, size, s->cpu->env.pc, cdj850_vms());
    if (cdj850_trace()) {
        info_report("cdj850: port write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_port_ops = {
    .read = cdj850_port_read,
    .write = cdj850_port_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_port = {
    .name = "cdj850-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Port, CDJ850_PORT_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_port_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Port *s = g_new0(Cdj850Port, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_port, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_port_ops, s,
                          "cdj850.port", CDJ850_PORT_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_PORT_BASE, &s->iomem,
                                        1);
}
