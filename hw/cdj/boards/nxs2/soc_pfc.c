/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * Pin Function Controller (0xA4050000), with error-LED decoding.
 *
 * The bootloader and firmware signal faults by blinking bit 3 of the port
 * byte at 0xA4050130; each edge is logged with the time since the last one.
 */
/* Input pins driven by peers that are not modelled; the register file only
 * returns what the firmware wrote, so these are forced. The bring-up routine
 * at 0x08214cf8 waits on two lines of the SCIFA4 peer with no timeout:
 *
 *   PUDR bit 1 (0xA4050162)  ready, polled at 0x08214d28; may read high
 *                            from the start
 *   PEDR bit 6 (0xA4050128)  done, polled at 0x08214d6a after the ~54 KB
 *                            SCIFA4 download; raised only once the download
 *                            has gone out (cdj_pfc_done_ready), otherwise the
 *                            firmware restarts it
 */
static const struct { hwaddr off; uint8_t bits; } cdj_pfc_ready[] = {
    { 0x162, 0x02 },   /* PUDR -- peer ready to receive */
    { 0x12E, 0x10 },   /* bit 4: presence line polled by 0x0844F4FC; low
                        * forces app mode 2 (0x0844F514) and no UI starts */
};

#define CDJ_PEDR_OFF        0x128
#define CDJ_PEDR_DONE_BIT   0x40
/* A threshold below the full 54,728 bytes: the last 64 are only sent after
 * the firmware sees DONE. */
#define CDJ_PEER_BLOB_LEN   54000

static bool cdj_pfc_done_ready(void)
{
    return cdj_peer_tx_bytes() >= CDJ_PEER_BLOB_LEN;
}

/* PUDR bit 3 (+0x162) is the stand-in DSP's ready line, answered by MAIN's
 * ack on +0x164 bit 1. It is a two-wire handshake, not a level:
 *
 *     wait +0x162 bit 3 high      DSP ready
 *     set  +0x164 bit 1           MAIN acknowledges
 *     wait +0x162 bit 3 low       DSP acknowledges
 *     clear +0x164 bit 1
 *
 * Ready is raised only after ~9 KB of I2C image has gone out.
 *   CDJ_DSP_READY=1  the handshake above
 *   CDJ_DSP_READY=2  held high regardless of the ack (the second wait fails)
 * Default off; with the C66x core running, cdj_c6x_pfc_read() drives the pins. */
#define CDJ_DSP_RDY_OFF     0x162
#define CDJ_DSP_RDY_BIT     0x08
#define CDJ_DSP_ACK_OFF     0x164
#define CDJ_DSP_ACK_BIT     0x02
#define CDJ_DSP_I2C_BLOB    9000

static int cdj_dsp_ready_knob = -1;

static bool cdj_dsp_ready(void)
{
    if (cdj_dsp_ready_knob < 0) {
        const char *e = getenv("CDJ_DSP_READY");

        cdj_dsp_ready_knob = e && *e ? (int)strtoul(e, NULL, 0) : 0;
    }
    return cdj_dsp_ready_knob &&
           cdj_dsp_i2c_tx_bytes() >= CDJ_DSP_I2C_BLOB;
}

/* PUDR is read once per peer download attempt; log PC/PR for the first few
 * to name the caller that retries it. */
static unsigned cdj_pudr_logged;

/* CDJ_PFC_RD_DEBUG: log each distinct (offset, guest PC) read once, to find
 * input pins the firmware polls. */
static struct { hwaddr off; uint32_t pc; } cdj_pfc_rd_seen[512];
static unsigned cdj_pfc_rd_seen_n;
static int cdj_pfc_rd_dbg = -1;
static int cdj_pfc_allhigh = -1;

/* Active-low overcurrent line from the USB power switch. */
#define CDJ_USB_OC_OFF  0x12E
#define CDJ_USB_OC_BIT  0x02
static int cdj_usb_oc = -1;

static uint64_t cdj_pfc_read(void *opaque, hwaddr off, unsigned size)
{
    CdjPfcState *s = opaque;
    uint64_t v = 0;

    if (cdj_pfc_rd_dbg < 0) {
        cdj_pfc_rd_dbg = getenv("CDJ_PFC_RD_DEBUG") ? 1 : 0;
    }
    if (cdj_pfc_rd_dbg && current_cpu) {
        uint32_t pc = SUPERH_CPU(current_cpu)->env.pc;
        unsigned j;
        for (j = 0; j < cdj_pfc_rd_seen_n; j++) {
            if (cdj_pfc_rd_seen[j].off == off && cdj_pfc_rd_seen[j].pc == pc) {
                break;
            }
        }
        if (j == cdj_pfc_rd_seen_n && cdj_pfc_rd_seen_n < ARRAY_SIZE(cdj_pfc_rd_seen)) {
            cdj_pfc_rd_seen[cdj_pfc_rd_seen_n].off = off;
            cdj_pfc_rd_seen[cdj_pfc_rd_seen_n].pc = pc;
            cdj_pfc_rd_seen_n++;
            qemu_log("pfc-rd: +0x%03x pc 0x%08x\n", (unsigned)off, pc);
        }
    }

    if (off == 0x162 && current_cpu && cdj_pudr_logged < 12) {
        CPUSH4State *e = &SUPERH_CPU(current_cpu)->env;

        cdj_pudr_logged++;
        qemu_log("pudr-poll[%u]: pc 0x%08x pr 0x%08x r14 0x%08x tx %" PRIu64 "\n",
                 cdj_pudr_logged, e->pc, e->pr, e->gregs[14],
                 cdj_peer_tx_bytes());
    }
    for (unsigned i = 0; i < size && off + i < CDJ_PFC_SIZE; i++) {
        uint8_t b = s->reg[off + i];
        for (unsigned k = 0; k < ARRAY_SIZE(cdj_pfc_ready); k++) {
            if (off + i == cdj_pfc_ready[k].off) {
                b |= cdj_pfc_ready[k].bits;
            }
        }
        if (off + i == CDJ_PEDR_OFF && cdj_pfc_done_ready()) {
            b |= CDJ_PEDR_DONE_BIT;
        }
        if (cdj_c6x_on()) {
            b = cdj_c6x_pfc_read(off + i, b);
        } else if (off + i == CDJ_DSP_RDY_OFF && cdj_dsp_ready() &&
            (cdj_dsp_ready_knob >= 2 ||
             !(s->reg[CDJ_DSP_ACK_OFF] & CDJ_DSP_ACK_BIT))) {
            b |= CDJ_DSP_RDY_BIT;
        }
        /* CDJ_USB_OC=1: hold /OC high. 0x08450958 polls it and, when low,
         * posts GUI message 0x92 "USB Error. Remove the device." */
        if (cdj_usb_oc < 0) {
            const char *e = getenv("CDJ_USB_OC");
            cdj_usb_oc = e && *e ? (int)strtoul(e, NULL, 0) : 0;
        }
        if (cdj_usb_oc && off + i == CDJ_USB_OC_OFF) {
            b |= CDJ_USB_OC_BIT;
        }
        /* CDJ_PFC_ALLHIGH=<byte>: OR the byte into every port-data read in
         * [0x120, 0x1a0), to test for an unknown input gate. */
        if (cdj_pfc_allhigh < 0) {
            const char *e = getenv("CDJ_PFC_ALLHIGH");
            cdj_pfc_allhigh = e && *e ? (int)strtoul(e, NULL, 0) : 0;
        }
        if (cdj_pfc_allhigh && off + i >= 0x120 && off + i < 0x1a0) {
            b |= (uint8_t)cdj_pfc_allhigh;
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void cdj_pfc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjPfcState *s = opaque;

    for (unsigned i = 0; i < size && off + i < CDJ_PFC_SIZE; i++) {
        uint8_t nv = (val >> (8 * i)) & 0xFF;
        if (!s->wr_count[off + i] || s->reg[off + i] != nv) {
            s->distinct[off + i]++;
        }
        s->wr_count[off + i]++;
        s->reg[off + i] = nv;
    }
    if (cdj_c6x_on()) {
        cdj_c6x_pfc_write(s->reg);
    }

    /* PWDR (+0x166) is written about once per timer tick; log the first 64
     * writes with their PC. */
    if (off == 0x166 && s->wr_count[0x166] <= 64) {
        qemu_log("pw: [%u] = 0x%02x from pc 0x%08x\n",
                 s->wr_count[0x166], (unsigned)(val & 0xFF),
                 current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }

    if (off == CDJ_LED_OFF) {
        int led = !!(val & CDJ_LED_BIT);
        if (led != s->last_led) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            int64_t dt = s->last_ns ? (now - s->last_ns) / 1000 : 0;
            uint32_t pc = current_cpu ?
                SUPERH_CPU(current_cpu)->env.pc : 0;
            qemu_log("LED %-3s  edge %4u  +%" PRId64 " us  pc 0x%08x\n",
                     led ? "ON" : "OFF", ++s->edges, dt, pc);
            s->last_led = led;
            s->last_ns = now;
        }
    }
}

/* Exit summary: writes and distinct values per port byte. */
static Notifier cdj_pfc_exit;
CdjPfcState *cdj_pfc_singleton;

static void cdj_pfc_summary(Notifier *n, void *unused)
{
    CdjPfcState *s = cdj_pfc_singleton;
    unsigned i;

    if (!s) {
        return;
    }
    for (i = 0; i < CDJ_PFC_SIZE; i++) {
        if (s->wr_count[i]) {
            qemu_log("pfc: +0x%03x writes=%-7u distinct=%-5u final=0x%02x\n",
                     i, s->wr_count[i], s->distinct[i], s->reg[i]);
        }
    }
}

static const MemoryRegionOps cdj_pfc_ops = {
    .read = cdj_pfc_read,
    .write = cdj_pfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_pfc_init(MemoryRegion *sysmem)
{
    CdjPfcState *s = g_new0(CdjPfcState, 1);

    MemoryRegion *alias = g_new(MemoryRegion, 1);

    s->last_led = -1;
    cdj_pfc_singleton = s;
    cdj_pfc_exit.notify = cdj_pfc_summary;
    qemu_add_exit_notifier(&cdj_pfc_exit);
    memory_region_init_io(&s->iomem, NULL, &cdj_pfc_ops, s,
                          "sh7724.pfc", CDJ_PFC_SIZE);
    memory_region_add_subregion(sysmem, CDJ_PFC_BASE, &s->iomem);

    /* A region can only be a subregion once; the A7 view needs an alias. */
    memory_region_init_alias(alias, NULL, "sh7724.pfc-a7", &s->iomem,
                             0, CDJ_PFC_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(CDJ_PFC_BASE), alias);
}

