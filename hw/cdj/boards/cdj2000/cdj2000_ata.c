/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pioneer CDJ-2000 / CDJ-2000NXS disc drive: the SH7764 ATAPI host controller
 * with its DMA engine, and the slot-in drive behind it.
 *
 * cdj2000_ata.h has the register map.  Answering IDENTIFY is enough to clear
 * E-7001, but MAIN mounts a disc only through two paths a generic ATAPI
 * CD-ROM does not have.  Function names and addresses here are from the
 * CDJ-2000NXS MAIN symbols:
 *
 *  - The drive's mechanism is not visible through MMC at all.
 *    `cd_drive_state_machine_task` (0x041b7dd0) polls every 50 ms with the
 *    vendor packet 0xE0 (subcode '<', 4 bytes back: {'<', 0, 1, state}) and
 *    turns the state into the DISC events of `disc_media_state_handler`:
 *    3 = loading in (DISC-LOADING), 9..12 = disc in and spinning (DISC detected,
 *    analysis starts), 1 = loading out (LOADOUTING), 2 = no disc (EJECTED).
 *    Before announcing a disc it also asks the drive's service channel "?D"
 *    (0xE3 writes the text, 0xE4 reads the reply; reply[2] bit 3 set means a
 *    firmware-update disc).  A drive that rejects 0xE0 with ILLEGAL REQUEST
 *    never produces a state, so no disc is ever announced.
 *  - Reads are DMA through the controller, not PIO.  `atapi_read_cd_command`
 *    sets FEATURES bit 0, then `drive_read_request_dispatch` programs
 *    DMA_START_ADR/DMA_TRANS_CNT one block at a time (2048, or 0x940 for
 *    READ CD with the formatted Q sub-channel) and writes COMMAND 0x25; each
 *    finished count raises STATUS bit 1, and the device's INTRQ (bit 4) must
 *    not arrive while a count is still active or the ISR aborts the read.
 *
 * So the drive is modelled here: task file + packet protocol (PIO and DMA),
 * the MMC commands MAIN sends (TEST UNIT READY, REQUEST SENSE, INQUIRY,
 * START STOP UNIT, READ(10), READ TOC formats 0/1/2/5, READ CD, READ CD MSF
 * with the 16-byte formatted Q), the vendor 0xE0/0xE3/0xE4 commands, and the
 * slot mechanism.  Media: an ISO (one MODE1/2048 data track) or a CUE sheet
 * over BINARY files (AUDIO, MODE1/2352, MODE1/2048, MODE2/2352 tracks).
 *
 * Unknown packets answer CHECK CONDITION, ILLEGAL REQUEST and are always
 * logged, so a command the firmware needs and this model lacks shows up in
 * the run's log rather than as a silent hang.  CDJ_ATA_TRACE=1 logs every
 * packet; CDJ_DISC_READ_BPS paces DMA data in guest bytes/second (unset: a
 * fixed 10 us per block).
 *
 * Copyright (C) 2026 LycheeAPPF
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"

#include "cdj2000_ata.h"
#include "cdj_common.h"
#include "cdj_getenv.h"
#include "hw/boards.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

/* ------------------------------------------------------------------ media */

#define CD_RAW          2352
#define CD_DATA         2048
#define CD_MSF_OFFSET   150         /* LBA 0 is MSF 00:02:00 */
#define CD_LEADOUT      0xaa

typedef struct CdjDiscTrack {
    uint8_t number;
    uint8_t control;            /* TOC/Q CONTROL nibble: 0 audio, 4 data */
    uint16_t sector;            /* bytes per sector in its file */
    bool mode2;
    int file;
    uint64_t file_offset;       /* byte offset of `index0` in the file */
    uint32_t index0;            /* first LBA of the track (INDEX 00 or 01) */
    uint32_t start;             /* LBA of INDEX 01 */
} CdjDiscTrack;

typedef struct CdjDisc {
    char *path;
    GMappedFile *file[99];
    int nfiles;
    CdjDiscTrack track[99];
    int ntracks;
    uint32_t leadout;           /* LBA */
} CdjDisc;

static void cdj_disc_free(CdjDisc *d)
{
    int i;

    if (!d) {
        return;
    }
    for (i = 0; i < d->nfiles; i++) {
        g_mapped_file_unref(d->file[i]);
    }
    g_free(d->path);
    g_free(d);
}

static bool cdj_disc_parse_msf(const char *text, uint32_t *frames)
{
    unsigned m, s, f;

    if (!text || sscanf(text, "%u:%u:%u", &m, &s, &f) != 3 || s > 59 || f > 74) {
        return false;
    }
    *frames = (m * 60 + s) * 75 + f;
    return true;
}

static uint32_t cdj_disc_file_sectors(CdjDisc *d, int file, unsigned sector)
{
    return g_mapped_file_get_length(d->file[file]) / sector;
}

/* An ISO image, or a CUE sheet; NULL with *err set when it cannot be used. */
static CdjDisc *cdj_disc_open(const char *path, char **err)
{
    CdjDisc *d = g_new0(CdjDisc, 1);
    GError *gerr = NULL;

    d->path = g_strdup(path);
    if (!g_str_has_suffix(path, ".cue") && !g_str_has_suffix(path, ".CUE")) {
        d->file[0] = g_mapped_file_new(path, FALSE, &gerr);
        if (!d->file[0]) {
            goto gerror;
        }
        d->nfiles = 1;
        d->ntracks = 1;
        d->track[0] = (CdjDiscTrack){ .number = 1, .control = 4,
                                      .sector = CD_DATA };
        d->leadout = cdj_disc_file_sectors(d, 0, CD_DATA);
        if (d->leadout == 0) {
            *err = g_strdup_printf("%s: empty image", path);
            goto fail;
        }
        return d;
    }

    {
        g_autofree char *text = NULL;
        g_autofree char *dir = g_path_get_dirname(path);
        g_auto(GStrv) lines = NULL;
        uint32_t file_base = 0;
        unsigned file_sector = 0;
        int i;

        if (!g_file_get_contents(path, &text, NULL, &gerr)) {
            goto gerror;
        }
        lines = g_strsplit_set(text, "\r\n", -1);
        for (i = 0; lines[i]; i++) {
            g_auto(GStrv) w = NULL;
            char *line = g_strstrip(lines[i]);
            CdjDiscTrack *t = d->ntracks ? &d->track[d->ntracks - 1] : NULL;

            if (!*line) {
                continue;
            }
            if (g_str_has_prefix(line, "FILE ")) {
                char *open = strchr(line, '"');
                char *close = open ? strrchr(line, '"') : NULL;
                g_autofree char *name = NULL;
                g_autofree char *full = NULL;

                if (!open || close == open || !g_str_has_suffix(line, "BINARY")
                    || d->nfiles == G_N_ELEMENTS(d->file)) {
                    *err = g_strdup_printf("%s:%d: only FILE \"name\" BINARY "
                                           "is supported", path, i + 1);
                    goto fail;
                }
                if (d->nfiles && !file_sector) {
                    *err = g_strdup_printf("%s:%d: FILE has no tracks", path,
                                           i + 1);
                    goto fail;
                }
                if (d->nfiles) {
                    file_base += cdj_disc_file_sectors(d, d->nfiles - 1,
                                                       file_sector);
                }
                name = g_strndup(open + 1, close - open - 1);
                full = g_path_is_absolute(name) ? g_strdup(name)
                                                : g_build_filename(dir, name, NULL);
                d->file[d->nfiles] = g_mapped_file_new(full, FALSE, &gerr);
                if (!d->file[d->nfiles]) {
                    goto gerror;
                }
                d->nfiles++;
                file_sector = 0;
                continue;
            }
            w = g_strsplit(line, " ", -1);
            if (!strcmp(w[0], "TRACK") && w[1] && w[2]) {
                if (!d->nfiles || d->ntracks == G_N_ELEMENTS(d->track)) {
                    *err = g_strdup_printf("%s:%d: TRACK before FILE", path, i + 1);
                    goto fail;
                }
                t = &d->track[d->ntracks++];
                t->number = atoi(w[1]);
                t->file = d->nfiles - 1;
                t->index0 = t->start = UINT32_MAX;
                if (!strcmp(w[2], "AUDIO")) {
                    t->sector = CD_RAW;
                } else if (!strcmp(w[2], "MODE1/2352")) {
                    t->sector = CD_RAW;
                    t->control = 4;
                } else if (!strcmp(w[2], "MODE2/2352")) {
                    t->sector = CD_RAW;
                    t->control = 4;
                    t->mode2 = true;
                } else if (!strcmp(w[2], "MODE1/2048")) {
                    t->sector = CD_DATA;
                    t->control = 4;
                } else {
                    *err = g_strdup_printf("%s:%d: track type %s is not "
                                           "supported", path, i + 1, w[2]);
                    goto fail;
                }
                if (file_sector && file_sector != t->sector) {
                    *err = g_strdup_printf("%s:%d: mixed sector sizes in one "
                                           "FILE", path, i + 1);
                    goto fail;
                }
                file_sector = t->sector;
            } else if (!strcmp(w[0], "INDEX") && t && w[1] && w[2]) {
                uint32_t rel;

                if (!cdj_disc_parse_msf(w[2], &rel)) {
                    *err = g_strdup_printf("%s:%d: bad INDEX time", path, i + 1);
                    goto fail;
                }
                if (atoi(w[1]) == 0) {
                    t->index0 = file_base + rel;
                    t->file_offset = (uint64_t)rel * t->sector;
                } else if (atoi(w[1]) == 1) {
                    t->start = file_base + rel;
                    if (t->index0 == UINT32_MAX) {
                        t->index0 = t->start;
                        t->file_offset = (uint64_t)rel * t->sector;
                    }
                }
            } else if (!strcmp(w[0], "PREGAP") || !strcmp(w[0], "POSTGAP")) {
                /* not supported: gaps that are not in the file */
                *err = g_strdup_printf("%s:%d: %s is not supported", path,
                                       i + 1, w[0]);
                goto fail;
            }
        }
        if (!d->ntracks || !file_sector) {
            *err = g_strdup_printf("%s: no tracks", path);
            goto fail;
        }
        for (i = 0; i < d->ntracks; i++) {
            if (d->track[i].start == UINT32_MAX ||
                d->track[i].number != (d->ntracks ? d->track[0].number + i : 0) ||
                (i && d->track[i].index0 < d->track[i - 1].start)) {
                *err = g_strdup_printf("%s: track %d has no INDEX 01 or is out "
                                       "of order", path, d->track[i].number);
                goto fail;
            }
        }
        d->leadout = file_base + cdj_disc_file_sectors(d, d->nfiles - 1,
                                                       file_sector);
        if (d->leadout <= d->track[d->ntracks - 1].start) {
            *err = g_strdup_printf("%s: the last track is empty", path);
            goto fail;
        }
        return d;
    }

gerror:
    *err = g_strdup(gerr->message);
    g_error_free(gerr);
fail:
    cdj_disc_free(d);
    return NULL;
}

/* The track an LBA falls in (pregap included), NULL past the program area. */
static CdjDiscTrack *cdj_disc_track(CdjDisc *d, uint32_t lba)
{
    int i;

    if (lba >= d->leadout) {
        return NULL;
    }
    for (i = d->ntracks - 1; i >= 0; i--) {
        if (lba >= d->track[i].index0) {
            return &d->track[i];
        }
    }
    return NULL;
}

static const uint8_t *cdj_disc_sector(CdjDisc *d, CdjDiscTrack *t, uint32_t lba)
{
    uint64_t off = t->file_offset + (uint64_t)(lba - t->index0) * t->sector;
    GMappedFile *f = d->file[t->file];

    if (off + t->sector > g_mapped_file_get_length(f)) {
        return NULL;
    }
    return (const uint8_t *)g_mapped_file_get_contents(f) + off;
}

static uint8_t bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}

static void lba_to_msf(uint32_t frames, uint8_t *msf)
{
    msf[0] = frames / (60 * 75);
    msf[1] = frames / 75 % 60;
    msf[2] = frames % 75;
}

/* All 2352 bytes of a sector; a 2048-byte data track gets sync and header. */
static bool cdj_disc_raw(CdjDisc *d, uint32_t lba, uint8_t *out)
{
    CdjDiscTrack *t = cdj_disc_track(d, lba);
    const uint8_t *src = t ? cdj_disc_sector(d, t, lba) : NULL;
    uint8_t msf[3];

    if (!src) {
        return false;
    }
    if (t->sector == CD_RAW) {
        memcpy(out, src, CD_RAW);
        return true;
    }
    /* not supported: EDC/ECC stay zero; nothing in MAIN checks them on a read */
    memset(out, 0, CD_RAW);
    memset(out + 1, 0xff, 10);
    lba_to_msf(lba + CD_MSF_OFFSET, msf);
    out[12] = bcd(msf[0]);
    out[13] = bcd(msf[1]);
    out[14] = bcd(msf[2]);
    out[15] = 1;
    memcpy(out + 16, src, CD_DATA);
    return true;
}

/* The 2048 user bytes of a data sector. */
static bool cdj_disc_data(CdjDisc *d, uint32_t lba, uint8_t *out)
{
    CdjDiscTrack *t = cdj_disc_track(d, lba);
    const uint8_t *src = t && (t->control & 4) ? cdj_disc_sector(d, t, lba) : NULL;

    if (!src) {
        return false;
    }
    memcpy(out, t->sector == CD_RAW ? src + (t->mode2 ? 24 : 16) : src, CD_DATA);
    return true;
}

/* CRC-16-CCITT (x^16 + x^12 + x^5 + 1), as the Q sub-channel carries it. */
static uint16_t cdj_q_crc(const uint8_t *q, int len)
{
    uint16_t crc = 0;
    int i, b;

    for (i = 0; i < len; i++) {
        crc ^= q[i] << 8;
        for (b = 0; b < 8; b++) {
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

/*
 * The 16-byte formatted Q of READ CD sub-channel 010b, exactly what
 * `maybe_cd_subcode_verify_and_index` (0x041cc910) checks: ADR 1 in the low
 * nibble of byte 0, BCD track/index/relative/absolute times, and the inverted
 * CRC big-endian in bytes 10..11 (a frame with both CRC bytes zero is skipped).
 */
static void cdj_disc_q(CdjDisc *d, uint32_t lba, uint8_t *q)
{
    CdjDiscTrack *t = cdj_disc_track(d, lba);
    uint8_t rel[3], abs[3];
    uint16_t crc;

    memset(q, 0, 16);
    lba_to_msf(lba + CD_MSF_OFFSET, abs);
    if (!t) {
        q[0] = 0x01 | (d->track[d->ntracks - 1].control << 4);
        q[1] = CD_LEADOUT;
        q[2] = 1;
        lba_to_msf(lba - d->leadout, rel);
    } else {
        q[0] = 0x01 | (t->control << 4);
        q[1] = bcd(t->number);
        q[2] = lba < t->start ? 0 : 1;
        lba_to_msf(lba < t->start ? t->start - lba : lba - t->start, rel);
    }
    q[3] = bcd(rel[0]);
    q[4] = bcd(rel[1]);
    q[5] = bcd(rel[2]);
    q[7] = bcd(abs[0]);
    q[8] = bcd(abs[1]);
    q[9] = bcd(abs[2]);
    crc = ~cdj_q_crc(q, 10);
    q[10] = crc >> 8;
    q[11] = crc;
}

/* ------------------------------------------------------------ the device */

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DSC  0x10
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define DEVCTL_NIEN 0x02
#define DEVCTL_SRST 0x04

/* Interrupt reason (sector count) bits */
#define IR_COD  0x01
#define IR_IO   0x02

/* Controller registers, as word indices into `chip` */
#define CTL_COMMAND     (0x00 / 4)
#define CTL_STATUS      (0x04 / 4)
#define CTL_INT_EN      (0x08 / 4)
#define CTL_DMA_ADDR    (0x1c / 4)
#define CTL_DMA_COUNT   (0x20 / 4)
#define CTL_BYTE_SWAP   (0x3c / 4)

#define CS_ACTIVE       0x001
#define CS_DMA_DONE     0x002
#define CS_INTRQ        0x010
#define CS_LATCHED      0x106       /* what STATUS write-0 clears */

/* E0 '<': the mechanism, as cd_drive_state_machine_task reads it */
#define MECH_LOADING_OUT    1
#define MECH_EMPTY          2
#define MECH_LOADING_IN     3
#define MECH_READY          9

#define MECH_LOAD_NS        (1500 * SCALE_MS)   /* slot pull-in + spin-up */
#define MECH_EJECT_NS       (1000 * SCALE_MS)
#define PACKET_NS           (20 * SCALE_US)
#define CHUNK_NS            (10 * SCALE_US)
#define DONE_NS             (50 * SCALE_US)

#define BUF_MAX             (128 * 1024)

typedef enum {
    P_IDLE, P_CDB, P_PIO_IN, P_PIO_OUT, P_DMA_IN, P_BUSY,
} CdjAtaPhase;

typedef enum {
    DO_NOTHING, DO_PACKET, DO_CHUNK, DO_COMPLETE, DO_OUT_DONE, DO_RESET,
} CdjAtaAction;

typedef struct CdjAta {
    Object parent_obj;
    MemoryRegion taskfile, chip;
    qemu_irq irq;
    bool trace;
    uint64_t bps;

    /* controller */
    uint32_t reg[CDJ_ATA_CHIP_SIZE / 4];
    uint32_t latched;
    bool dma_active;
    uint32_t dma_addr, dma_left;
    QEMUTimer *dma_timer;

    /* task file */
    uint8_t feature, error, nsector, sector, lcyl, hcyl, select, status, devctl;
    bool intrq;

    /* the command in flight */
    CdjAtaPhase phase;
    bool packet, dma;
    uint8_t cdb[12];
    unsigned cdb_len;
    uint8_t *buf;
    uint32_t len, pos, chunk_left, limit;
    uint8_t sense_key, asc, ascq;
    CdjAtaAction action;
    QEMUTimer *cmd_timer;

    /* the mechanism */
    CdjDisc *disc;              /* loaded (or loading) */
    CdjDisc *mouth;             /* ejected, still in the slot */
    char *default_path;         /* CDJ_DISC, what a bare `disc insert` loads */
    int mech;
    QEMUTimer *mech_timer;
    char svmode[64];
    GByteArray *disc_path, *mouth_path;     /* only while saving or loading */
} CdjAta;

#define TYPE_CDJ2000_ATAPI "cdj2000-atapi"

/* CDJ_DISC_READ_BPS, plain decimal bytes per second; unset (0) paces nothing. */
static uint64_t ata_read_rate(void)
{
    const char *spec = getenv("CDJ_DISC_READ_BPS");
    uint64_t value = 0;

    if (!spec) {
        return 0;
    }
    for (; *spec; spec++) {
        if (*spec < '0' || *spec > '9' ||
            value > (UINT64_MAX - (*spec - '0')) / 10) {
            error_report("cdj2000: CDJ_DISC_READ_BPS: invalid bytes per second");
            exit(1);
        }
        value = value * 10 + (*spec - '0');
    }
    return value;
}

static void ata_update_irq(CdjAta *s)
{
    uint32_t status = s->latched | (s->dma_active ? CS_ACTIVE : 0) |
                      (s->intrq && !(s->devctl & DEVCTL_NIEN) ? CS_INTRQ : 0);

    if (s->irq) {
        qemu_set_irq(s->irq, (status & s->reg[CTL_INT_EN]) != 0);
    }
}

static void ata_raise(CdjAta *s)
{
    s->intrq = true;
    ata_update_irq(s);
}

static void ata_later(CdjAta *s, CdjAtaAction action, int64_t ns)
{
    s->action = action;
    timer_mod(s->cmd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void ata_signature(CdjAta *s)
{
    s->nsector = 1;
    s->sector = 1;
    s->lcyl = 0x14;
    s->hcyl = 0xeb;
}

static void ata_reset(CdjAta *s)
{
    timer_del(s->cmd_timer);
    s->phase = P_IDLE;
    s->action = DO_NOTHING;
    s->error = 1;
    s->select &= 0xf0;
    s->intrq = false;
    ata_signature(s);
    s->status = 0;
    ata_update_irq(s);
}

static void ata_check(CdjAta *s, uint8_t key, uint8_t asc, uint8_t ascq)
{
    s->sense_key = key;
    s->asc = asc;
    s->ascq = ascq;
}

/* ATAPI completion: status phase, INTRQ. */
static void ata_complete(CdjAta *s)
{
    bool check = s->sense_key != 0;

    s->phase = P_IDLE;
    s->nsector = IR_IO | IR_COD;
    s->status = ST_DRDY | ST_DSC | (check ? ST_ERR : 0);
    s->error = check ? s->sense_key << 4 : 0;
    ata_raise(s);
}

/* The next PIO DRQ block of a data-in command. */
static void ata_pio_chunk(CdjAta *s)
{
    uint32_t n = MIN(s->len - s->pos, s->limit);

    s->chunk_left = n + (n & 1);
    s->lcyl = s->chunk_left;
    s->hcyl = s->chunk_left >> 8;
    s->nsector = s->packet ? IR_IO : s->nsector;
    s->status = ST_DRDY | ST_DSC | ST_DRQ;
    s->phase = P_PIO_IN;
    ata_raise(s);
}

/* ---------------------------------------------------------- controller DMA */

static void ctl_dma_pump(CdjAta *s);

static void ata_dma_check_done(CdjAta *s)
{
    if (s->phase == P_DMA_IN && s->pos >= s->len) {
        s->phase = P_BUSY;
        ata_later(s, DO_COMPLETE, DONE_NS);
    }
}

static void ctl_dma_timer(void *opaque)
{
    CdjAta *s = opaque;

    if (!s->dma_active) {
        return;
    }
    if (s->dma_left) {
        ctl_dma_pump(s);
        return;
    }
    s->dma_active = false;
    s->latched |= CS_DMA_DONE;
    if (s->trace) {
        qemu_log("cdj2000-ata: %.6f DMA count done\n",
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    }
    ata_update_irq(s);
    ata_dma_check_done(s);
}

/*
 * Move what the device has into guest memory, up to the programmed count.
 * The count finishes (STATUS bit 1) one CHUNK_NS -- or bytes / BPS -- after
 * the data moved, never inside the register write that started it.
 */
static void ctl_dma_pump(CdjAta *s)
{
    uint32_t n = 0;

    if (!s->dma_active || timer_pending(s->dma_timer)) {
        return;
    }
    if (s->dma_left && s->phase == P_DMA_IN) {
        n = MIN(s->dma_left, s->len - s->pos);
        address_space_write(&address_space_memory, s->dma_addr,
                            MEMTXATTRS_UNSPECIFIED, s->buf + s->pos, n);
        s->pos += n;
        s->dma_addr += n;
        s->dma_left -= n;
    }
    if (s->dma_left == 0) {
        int64_t ns = s->bps ? MAX(CHUNK_NS, (int64_t)n * NANOSECONDS_PER_SECOND
                                            / s->bps) : CHUNK_NS;

        timer_mod(s->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    } else if (s->phase == P_DMA_IN && s->pos >= s->len) {
        qemu_log("cdj2000-ata: DMA wants %u more bytes than the command has\n",
                 s->dma_left);
    }
}

/* ----------------------------------------------------------- the commands */

static void put16(uint8_t *p, unsigned v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* A TOC address: LBA, or 0:M:S:F with `msf`. */
static void toc_addr(uint8_t *p, uint32_t lba, bool msf)
{
    if (msf) {
        p[0] = 0;
        lba_to_msf(lba + CD_MSF_OFFSET, p + 1);
    } else {
        put32(p, lba);
    }
}

static uint32_t cmd_read_toc(CdjAta *s, uint8_t *out)
{
    CdjDisc *d = s->disc;
    bool msf = s->cdb[1] & 2;
    unsigned format = s->cdb[2] & 0x0f, start = s->cdb[6];
    uint8_t *p = out + 4;
    int i;

    if (format == 0) {
        if (start > d->track[d->ntracks - 1].number && start != CD_LEADOUT) {
            ata_check(s, 5, 0x24, 0);
            return 0;
        }
        for (i = 0; i < d->ntracks; i++) {
            if (d->track[i].number >= start) {
                p[1] = 0x10 | d->track[i].control;
                p[2] = d->track[i].number;
                toc_addr(p + 4, d->track[i].start, msf);
                p += 8;
            }
        }
        p[1] = 0x10 | d->track[d->ntracks - 1].control;
        p[2] = CD_LEADOUT;
        toc_addr(p + 4, d->leadout, msf);
        p += 8;
    } else if (format == 1) {
        p[1] = 0x10 | d->track[0].control;
        p[2] = d->track[0].number;
        toc_addr(p + 4, d->track[0].start, msf);
        p += 8;
        out[2] = out[3] = 1;
        put16(out, p - out - 2);
        return p - out;
    } else if (format == 2) {
        static const uint8_t points[] = { 0xa0, 0xa1, 0xa2 };

        for (i = 0; i < 3 + d->ntracks; i++, p += 11) {
            CdjDiscTrack *t = i < 3 ? NULL : &d->track[i - 3];
            uint8_t control = t ? t->control : d->track[0].control;

            memset(p, 0, 11);
            p[0] = 1;
            p[1] = 0x10 | control;
            p[3] = t ? t->number : points[i];
            if (t) {
                lba_to_msf(t->start + CD_MSF_OFFSET, p + 8);
            } else if (i == 0) {
                p[8] = d->track[0].number;
                p[9] = d->track[0].mode2 ? 0x20 : 0;
            } else if (i == 1) {
                p[8] = d->track[d->ntracks - 1].number;
            } else {
                lba_to_msf(d->leadout + CD_MSF_OFFSET, p + 8);
            }
        }
        out[2] = out[3] = 1;
        put16(out, p - out - 2);
        return p - out;
    } else if (format == 5) {
        /* No CD-TEXT on the disc: a header and no packs. */
        put16(out, 2);
        out[2] = out[3] = 0;
        return 4;
    } else {
        ata_check(s, 5, 0x24, 0);
        return 0;
    }
    out[2] = d->track[0].number;
    out[3] = d->track[d->ntracks - 1].number;
    put16(out, p - out - 2);
    return p - out;
}

/*
 * READ CD (0xBE) and READ CD MSF (0xB9): per frame the parts byte 9 asks for
 * (0xF8 all 2352 bytes, 0x10 user data only; C2 bits add 294 zeros) and the
 * sub-channel byte 10 selects (2 = formatted Q, 16 bytes; 1/4 = 96 raw bytes,
 * given as zero -- nothing MAIN builds asks for them).
 */
static uint32_t cmd_read_cd(CdjAta *s, uint8_t *out, uint32_t lba, uint32_t n)
{
    CdjDisc *d = s->disc;
    uint8_t flags = s->cdb[9], sub = s->cdb[10] & 7;
    unsigned type = (s->cdb[1] >> 2) & 7;
    uint8_t raw[CD_RAW];
    uint32_t len = 0, i;

    for (i = 0; i < n; i++, lba++) {
        CdjDiscTrack *t = cdj_disc_track(d, lba);
        bool audio;

        if (!t || !cdj_disc_raw(d, lba, raw)) {
            ata_check(s, 5, 0x21, 0);       /* LBA out of range */
            return len;
        }
        audio = !(t->control & 4);
        if ((type == 1 && !audio) || (type >= 2 && audio)) {
            ata_check(s, 5, 0x64, 0);       /* illegal mode for this track */
            return len;
        }
        if (len + CD_RAW + 294 + 96 > BUF_MAX) {
            ata_check(s, 5, 0x24, 0);
            return len;
        }
        if (audio) {
            if (flags & 0xf8) {
                memcpy(out + len, raw, CD_RAW);
                len += CD_RAW;
            }
        } else {
            if (flags & 0x80) {
                memcpy(out + len, raw, 12);
                len += 12;
            }
            if (flags & 0x20) {
                memcpy(out + len, raw + 12, 4);
                len += 4;
            }
            if (flags & 0x10) {
                unsigned skip = t->mode2 ? 8 : 0;
                memcpy(out + len, raw + 16 + skip, CD_DATA);
                len += CD_DATA;
            }
            if (flags & 0x08) {
                memcpy(out + len, raw + 16 + CD_DATA, 288);
                len += 288;
            }
        }
        if (flags & 0x06) {
            memset(out + len, 0, (flags & 0x06) == 0x04 ? 296 : 294);
            len += (flags & 0x06) == 0x04 ? 296 : 294;
        }
        if (sub == 2) {
            cdj_disc_q(d, lba, out + len);
            len += 16;
        } else if (sub == 1 || sub == 4) {
            memset(out + len, 0, 96);
            len += 96;
        }
    }
    return len;
}

static uint32_t cmd_identify(CdjAta *s, uint8_t *out)
{
    uint16_t w[256] = { 0 };
    static const char serial[] = "CDJEMU00000000000001";
    static const char revision[] = "1.00    ";
    /* A neutral identity; MAIN only checks word 0 bit 15 here. */
    static const char model[] = "PIONEER CD-ROM DRIVE                    ";
    int i;

    w[0] = 0x85c0;                      /* ATAPI, CD-ROM, removable, 12-byte */
    for (i = 0; i < 10; i++) {
        w[10 + i] = serial[2 * i] << 8 | serial[2 * i + 1];
    }
    for (i = 0; i < 4; i++) {
        w[23 + i] = revision[2 * i] << 8 | revision[2 * i + 1];
    }
    for (i = 0; i < 20; i++) {
        w[27 + i] = model[2 * i] << 8 | model[2 * i + 1];
    }
    w[49] = 0x0300;                     /* DMA, LBA */
    w[53] = 0x0006;
    w[63] = 0x0007;                     /* multiword DMA 0-2 */
    w[64] = 0x0003;                     /* PIO 3-4 */
    w[65] = w[66] = w[67] = w[68] = 120;
    w[80] = 0x001e;
    for (i = 0; i < 256; i++) {
        out[2 * i] = w[i];
        out[2 * i + 1] = w[i] >> 8;
    }
    return 512;
}

static const char *cmd_svmode_reply(CdjAta *s)
{
    /*
     * The drive's service console.  Only "?D" is used outside service mode
     * (cd_drive_poll_media_flag): reply[2] bit 3 would flag an update disc.
     * "TS" waits for 'R'.  Not supported: the real drive's other replies are
     * not known; "R" stands in for them.
     */
    if (!strncmp(s->svmode, "?D", 2)) {
        return "0000\r";
    }
    return "R\r";
}

/* Vendor E0 status, including the empty slot used during boot/browse. */
static void ata_state_reply(uint8_t *out, uint8_t subcode, int mech)
{
    out[0] = subcode;
    out[1] = 0;
    out[2] = 1;
    out[3] = subcode == '<' ? mech : 0;
}

/* Run the CDB.  Fills buf/len; leaves sense set on CHECK CONDITION. */
static void cmd_packet(CdjAta *s)
{
    uint8_t *c = s->cdb, *out = s->buf;
    CdjDisc *d = s->disc;
    bool ready = d && s->mech == MECH_READY;
    uint32_t alloc = 0, lba, n, i;
    bool out_dir = false;

    s->len = s->pos = 0;
    memset(out, 0, 256);
    if (c[0] == 0x03) {                         /* REQUEST SENSE */
        /* the sense reported is the previous command's */
        out[0] = 0x70;
        out[2] = s->sense_key;
        out[7] = 10;
        out[12] = s->asc;
        out[13] = s->ascq;
        s->len = MIN(18, c[4]);
        ata_check(s, 0, 0, 0);
        return;
    }
    ata_check(s, 0, 0, 0);

    switch (c[0]) {
    case 0x00:                                  /* TEST UNIT READY */
    case 0x1e:                                  /* PREVENT ALLOW */
    case 0x2b:                                  /* SEEK */
        break;
    case 0x12:                                  /* INQUIRY */
        out[0] = 0x05;
        out[1] = 0x80;
        out[3] = 0x21;
        out[4] = 31;
        memcpy(out + 8, "PIONEER CD-ROM DRIVE    1.00", 28);
        alloc = c[4];
        s->len = 36;
        break;
    case 0x1b:                                  /* START STOP UNIT */
        if ((c[4] & 3) == 2) {
            if (s->disc && s->mech != MECH_LOADING_OUT) {
                s->mech = MECH_LOADING_OUT;
                timer_mod(s->mech_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                          + MECH_EJECT_NS);
                qemu_log("cdj2000-ata: eject by START STOP UNIT\n");
            }
        } else if ((c[4] & 3) == 3 && !s->disc && s->mouth) {
            s->disc = s->mouth;
            s->mouth = NULL;
            s->mech = MECH_LOADING_IN;
            timer_mod(s->mech_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                      + MECH_LOAD_NS);
        }
        return;
    case 0x25:                                  /* READ CAPACITY */
        alloc = 8;
        s->len = 8;
        break;
    case 0x28:                                  /* READ(10) */
    case 0x43:                                  /* READ TOC */
    case 0xb9:                                  /* READ CD MSF */
    case 0xbe:                                  /* READ CD */
        break;
    case 0xe0:                                  /* vendor: drive status */
        ata_state_reply(out, c[2], s->mech);
        alloc = c[8] << 8 | c[9];
        s->len = 4;
        if (s->trace && c[2] != '<') {
            qemu_log("cdj2000-ata: E0 subcode %#x\n", c[2]);
        }
        break;
    case 0xe3:                                  /* vendor: service text in */
        s->len = c[8] << 8 | c[9];
        out_dir = true;
        alloc = s->len;
        break;
    case 0xe4:                                  /* vendor: service text out */
        n = strlen(cmd_svmode_reply(s));
        memcpy(out, cmd_svmode_reply(s), n);
        alloc = c[8] << 8 | c[9];
        s->len = MAX(n, alloc);
        break;
    default:
        error_report("cdj2000-ata: unsupported packet %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x", c[0], c[1], c[2],
                 c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
        ata_check(s, 5, 0x20, 0);
        return;
    }

    /* Everything below needs the disc in and spinning. */
    if (c[0] == 0x00 || c[0] == 0x25 || c[0] == 0x28 || c[0] == 0x43 ||
        c[0] == 0xb9 || c[0] == 0xbe || c[0] == 0x2b) {
        if (!d || s->mech == MECH_LOADING_OUT) {
            ata_check(s, 2, 0x3a, 0);           /* medium not present */
            return;
        }
        if (!ready) {
            ata_check(s, 2, 0x04, 0x01);        /* becoming ready */
            return;
        }
    }

    switch (c[0]) {
    case 0x25:
        put32(out, d->leadout - 1);
        put32(out + 4, CD_DATA);
        break;
    case 0x28:
        lba = get32(c + 2);
        n = c[7] << 8 | c[8];
        if ((uint64_t)n * CD_DATA > BUF_MAX) {
            ata_check(s, 5, 0x24, 0);
            return;
        }
        for (i = 0; i < n; i++) {
            if (!cdj_disc_data(d, lba + i, out + i * CD_DATA)) {
                ata_check(s, 5, cdj_disc_track(d, lba + i) ? 0x64 : 0x21, 0);
                break;
            }
        }
        s->len = i * CD_DATA;
        alloc = s->len;
        break;
    case 0x43:
        s->len = cmd_read_toc(s, out);
        alloc = c[7] << 8 | c[8];
        break;
    case 0xbe:
        lba = get32(c + 2);
        n = c[6] << 16 | c[7] << 8 | c[8];
        s->len = cmd_read_cd(s, out, lba, n);
        alloc = s->len;
        break;
    case 0xb9: {
        uint32_t from = (c[3] * 60 + c[4]) * 75 + c[5];
        uint32_t to = (c[6] * 60 + c[7]) * 75 + c[8];

        if (from < CD_MSF_OFFSET || to < from) {
            ata_check(s, 5, 0x24, 0);
            return;
        }
        s->len = cmd_read_cd(s, out, from - CD_MSF_OFFSET, to - from);
        alloc = s->len;
        break;
    }
    }

    if (!out_dir) {
        s->len = MIN(s->len, alloc);
    }
}

static void ata_packet_done(CdjAta *s)
{
    uint8_t op = s->cdb[0];

    cmd_packet(s);
    if (s->trace || s->sense_key) {
        if (s->trace || op != 0x00) {
            qemu_log("cdj2000-ata: %.4f packet %02x %02x %02x %02x %02x %02x %02x "
                     "%02x %02x %02x %02x -> %u bytes%s, sense %x/%02x/%02x\n",
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, op, s->cdb[1], s->cdb[2], s->cdb[3], s->cdb[4], s->cdb[5],
                     s->cdb[6], s->cdb[7], s->cdb[8], s->cdb[9], s->cdb[10],
                     s->len, s->dma ? " DMA" : "", s->sense_key, s->asc,
                     s->ascq);
        }
    }
    if (op == 0xe3 && !s->sense_key && s->len) {
        /* Data out: the host writes the text, then the command completes. */
        s->len = MIN(s->len, sizeof(s->svmode) - 1);
        s->chunk_left = s->len + (s->len & 1);
        s->lcyl = s->chunk_left;
        s->hcyl = s->chunk_left >> 8;
        s->nsector = 0;
        s->status = ST_DRDY | ST_DSC | ST_DRQ;
        s->phase = P_PIO_OUT;
        ata_raise(s);
        return;
    }
    if (s->len == 0 || s->sense_key) {
        ata_complete(s);
        return;
    }
    if (s->dma) {
        s->status = ST_DRDY | ST_DSC | ST_DRQ;
        s->nsector = IR_IO;
        s->phase = P_DMA_IN;
        ctl_dma_pump(s);
        return;
    }
    ata_pio_chunk(s);
}

static void ata_cmd_timer(void *opaque)
{
    CdjAta *s = opaque;
    CdjAtaAction action = s->action;

    s->action = DO_NOTHING;
    switch (action) {
    case DO_PACKET:
        ata_packet_done(s);
        break;
    case DO_CHUNK:
        ata_pio_chunk(s);
        break;
    case DO_COMPLETE:
        if (s->dma_active) {
            /* A count is still running (the host's closing zero-length one):
               the device's INTRQ would make the ISR abort it. */
            ata_later(s, DO_COMPLETE, DONE_NS);
            break;
        }
        if (s->packet) {
            if (s->trace && s->dma) {
                qemu_log("cdj2000-ata: %.4f DMA command done\n",
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
            }
            ata_complete(s);
        } else {
            s->phase = P_IDLE;
            s->status = ST_DRDY | ST_DSC;
        }
        break;
    case DO_OUT_DONE:
        memcpy(s->svmode, s->buf, s->len);
        s->svmode[s->len] = 0;
        if (s->trace) {
            qemu_log("cdj2000-ata: service text \"%.*s\"\n",
                     (int)strcspn(s->svmode, "\r"), s->svmode);
        }
        ata_complete(s);
        break;
    case DO_RESET:
        ata_reset(s);
        break;
    case DO_NOTHING:
        break;
    }
}

static void ata_command(CdjAta *s, uint8_t cmd)
{
    if (s->select & 0x10) {
        return;                                 /* no slave */
    }
    s->packet = false;
    s->intrq = false;
    switch (cmd) {
    case 0xa0:                                  /* PACKET */
        s->packet = true;
        s->dma = s->feature & 1;
        s->limit = (s->hcyl << 8 | s->lcyl) & ~1u;
        if (s->limit == 0) {
            s->limit = 0xfffe;
        }
        s->cdb_len = 0;
        s->nsector = IR_COD;
        s->status = ST_DRDY | ST_DSC | ST_DRQ;
        s->phase = P_CDB;
        break;
    case 0xa1:                                  /* IDENTIFY PACKET DEVICE */
        s->len = cmd_identify(s, s->buf);
        s->pos = 0;
        s->limit = 512;
        ata_pio_chunk(s);
        break;
    case 0x08:                                  /* DEVICE RESET */
        ata_reset(s);
        ata_raise(s);
        break;
    case 0xef:                                  /* SET FEATURES */
        s->status = ST_DRDY | ST_DSC;
        ata_raise(s);
        break;
    default:                                    /* ATA commands: abort */
        if (s->trace || cmd != 0xec) {
            qemu_log("cdj2000-ata: ATA command %#x aborted\n", cmd);
        }
        ata_signature(s);
        s->error = 0x04;
        s->status = ST_DRDY | ST_ERR;
        ata_raise(s);
        break;
    }
    ata_update_irq(s);
}

/* --------------------------------------------------------------- MMIO */

static uint64_t ata_tf_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjAta *s = opaque;
    uint32_t value = 0;

    if ((s->select & 0x10) && offset != 0x18) {
        return 0;                               /* nothing on the slave side */
    }
    switch (offset) {
    case 0x00:
        if (s->phase == P_PIO_IN && s->chunk_left) {
            value = (s->pos < s->len ? s->buf[s->pos] : 0) |
                    (s->pos + 1 < s->len ? s->buf[s->pos + 1] : 0) << 8;
            s->pos = MIN(s->pos + 2, s->len);
            s->chunk_left -= 2;
            if (s->chunk_left == 0) {
                s->status = ST_BSY;
                s->phase = P_BUSY;
                ata_later(s, s->pos < s->len ? DO_CHUNK : DO_COMPLETE,
                          CHUNK_NS);
            }
        }
        break;
    case 0x04: value = s->error; break;
    case 0x08: value = s->nsector; break;
    case 0x0c: value = s->sector; break;
    case 0x10: value = s->lcyl; break;
    case 0x14: value = s->hcyl; break;
    case 0x18: value = s->select | 0xa0; break;
    case 0x1c:
        value = s->status;
        if (s->intrq) {
            s->intrq = false;
            ata_update_irq(s);
        }
        break;
    case 0x38: value = s->status; break;
    }
    return value;
}

static void ata_tf_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    CdjAta *s = opaque;

    switch (offset) {
    case 0x00:
        if (s->phase == P_CDB) {
            if (s->cdb_len < sizeof(s->cdb)) {
                s->cdb[s->cdb_len++] = value;
                s->cdb[s->cdb_len++] = value >> 8;
            }
            if (s->cdb_len == sizeof(s->cdb)) {
                s->status = ST_BSY;
                s->phase = P_BUSY;
                ata_later(s, DO_PACKET, PACKET_NS);
            }
        } else if (s->phase == P_PIO_OUT && s->chunk_left) {
            uint32_t at = s->len + (s->len & 1) - s->chunk_left;

            s->buf[at] = value;
            s->buf[at + 1] = value >> 8;
            s->chunk_left -= 2;
            if (s->chunk_left == 0) {
                s->status = ST_BSY;
                s->phase = P_BUSY;
                ata_later(s, DO_OUT_DONE, CHUNK_NS);
            }
        }
        break;
    case 0x04: s->feature = value; break;
    case 0x08: s->nsector = value; break;
    case 0x0c: s->sector = value; break;
    case 0x10: s->lcyl = value; break;
    case 0x14: s->hcyl = value; break;
    case 0x18: s->select = value; break;
    case 0x1c: ata_command(s, value); break;
    case 0x38:
        if ((value & DEVCTL_SRST) && !(s->devctl & DEVCTL_SRST)) {
            timer_del(s->cmd_timer);
            s->status = ST_BSY;
            s->phase = P_BUSY;
        } else if (!(value & DEVCTL_SRST) && (s->devctl & DEVCTL_SRST)) {
            ata_later(s, DO_RESET, CHUNK_NS);
        }
        s->devctl = value;
        ata_update_irq(s);
        break;
    }
}

static const MemoryRegionOps ata_tf_ops = {
    .read = ata_tf_read,
    .write = ata_tf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t ata_chip_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjAta *s = opaque;
    unsigned i = offset / 4;

    if (i == CTL_STATUS) {
        return s->latched | (s->dma_active ? CS_ACTIVE : 0) |
               (s->intrq && !(s->devctl & DEVCTL_NIEN) ? CS_INTRQ : 0);
    }
    return s->reg[i];
}

static void ata_chip_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjAta *s = opaque;
    unsigned i = offset / 4;

    if (i == CTL_STATUS) {
        s->latched &= value;
        ata_update_irq(s);
        return;
    }
    s->reg[i] = value;
    if (i == CTL_BYTE_SWAP && value) {
        qemu_log_mask(LOG_UNIMP, "cdj2000-ata: BYTE_SWAP %#" PRIx64
                      " not modelled, DMA data stays in bus order\n", value);
    }
    if (i == CTL_COMMAND) {
        if (value & 0x80) {
            /* Device hard reset, held while bit 7 is set (atapi_reset). */
            timer_del(s->cmd_timer);
            s->status = ST_BSY;
            s->phase = P_BUSY;
            ata_later(s, DO_RESET, 2 * SCALE_MS);
        }
        if (value & 0x02) {
            s->dma_active = false;
            timer_del(s->dma_timer);
        } else if (value & 0x01) {
            if (s->trace) {
                qemu_log("cdj2000-ata: %.6f DMA start %u bytes\n",
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                         s->reg[CTL_DMA_COUNT]);
            }
            s->dma_active = true;
            s->dma_addr = s->reg[CTL_DMA_ADDR];
            s->dma_left = s->reg[CTL_DMA_COUNT];
            timer_del(s->dma_timer);
            ctl_dma_pump(s);
        }
    }
    ata_update_irq(s);
}

static const MemoryRegionOps ata_chip_ops = {
    .read = ata_chip_read,
    .write = ata_chip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* ----------------------------------------------------------- mechanism */

static void ata_mech_timer(void *opaque)
{
    CdjAta *s = opaque;

    if (s->mech == MECH_LOADING_IN) {
        s->mech = MECH_READY;
        qemu_log("cdj2000-ata: disc ready (%d tracks, %u sectors)\n",
                 s->disc->ntracks, s->disc->leadout);
    } else if (s->mech == MECH_LOADING_OUT) {
        s->mech = MECH_EMPTY;
        cdj_disc_free(s->mouth);
        s->mouth = s->disc;
        s->disc = NULL;
        qemu_log("cdj2000-ata: disc out, in the slot mouth\n");
    }
}

static bool ata_disc_load(CdjAta *s, const char *path, char *note, size_t len)
{
    char *err = NULL;
    CdjDisc *d = cdj_disc_open(path, &err);

    if (!d) {
        snprintf(note, len, "%s", err);
        g_free(err);
        return false;
    }
    cdj_disc_free(s->mouth);
    s->mouth = NULL;
    s->disc = d;
    s->mech = MECH_LOADING_IN;
    timer_mod(s->mech_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + MECH_LOAD_NS);
    qemu_log("cdj2000-ata: disc in: %s\n", path);
    return true;
}

/*
 * `disc insert [FILE]`: push a disc into the slot (FILE, else the one in the
 * slot mouth, else CDJ_DISC).  `disc eject`: the drive's own eject, which
 * MAIN sees as LOADOUTING it did not ask for ("+DRV自律?").  `disc remove`:
 * take an ejected disc out of the mouth.
 */
static bool ata_disc_control(void *opaque, const char *action,
                             const char *path, char *note, size_t len)
{
    CdjAta *s = opaque;
    static const char *const names[] = {
        [MECH_LOADING_OUT] = "ejecting", [MECH_EMPTY] = "empty",
        [MECH_LOADING_IN] = "loading", [MECH_READY] = "ready",
    };

    if (!strcmp(action, "insert")) {
        if (s->disc) {
            snprintf(note, len, "a disc is already in");
            return false;
        }
        if (path) {
            if (!ata_disc_load(s, path, note, len)) {
                return false;
            }
        } else if (s->mouth) {
            s->disc = s->mouth;
            s->mouth = NULL;
            s->mech = MECH_LOADING_IN;
            timer_mod(s->mech_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                      + MECH_LOAD_NS);
        } else if (!s->default_path ||
                   !ata_disc_load(s, s->default_path, note, len)) {
            if (!note[0]) {
                snprintf(note, len, "no disc to insert");
            }
            return false;
        }
    } else if (!strcmp(action, "eject")) {
        if (!s->disc || s->mech == MECH_LOADING_OUT) {
            snprintf(note, len, "no disc in");
            return false;
        }
        s->mech = MECH_LOADING_OUT;
        timer_mod(s->mech_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + MECH_EJECT_NS);
    } else if (!strcmp(action, "remove")) {
        cdj_disc_free(s->mouth);
        s->mouth = NULL;
    } else if (strcmp(action, "state")) {
        snprintf(note, len, "disc <insert [FILE]|eject|remove|state>");
        return false;
    }
    snprintf(note, len, "%s%s%s%s", names[s->mech],
             s->disc ? " " : s->mouth ? " mouth=" : "",
             s->disc ? s->disc->path : s->mouth ? s->mouth->path : "",
             "");
    return true;
}

static char *ata_disc_get(Object *obj, Error **errp)
{
    char note[PATH_MAX + 64] = "";

    ata_disc_control(obj, "state", NULL, note, sizeof(note));
    return g_strdup(note);
}

static void ata_disc_set(Object *obj, const char *value, Error **errp)
{
    const char *action, *path = NULL;
    char note[PATH_MAX + 64] = "";

    if (!strcmp(value, "insert")) {
        action = "insert";
    } else if (g_str_has_prefix(value, "insert ") && value[7]) {
        action = "insert";
        path = value + 7;
    } else if (!strcmp(value, "eject") || !strcmp(value, "remove")) {
        action = value;
    } else {
        error_setg(errp, "disc expects insert [PATH], eject or remove");
        return;
    }
    if (!ata_disc_control(obj, action, path, note, sizeof(note))) {
        error_setg(errp, "%s", note);
    }
}

/*
 * The snapshot holds the discs' paths, not their images: a restore opens
 * them again and fails if one is gone.
 */
static int ata_pre_save(void *opaque)
{
    CdjAta *s = opaque;

    if (s->disc) {
        s->disc_path = g_byte_array_append(g_byte_array_new(),
                                           (const guint8 *)s->disc->path,
                                           strlen(s->disc->path));
    }
    if (s->mouth) {
        s->mouth_path = g_byte_array_append(g_byte_array_new(),
                                            (const guint8 *)s->mouth->path,
                                            strlen(s->mouth->path));
    }
    return 0;
}

static int ata_post_save(void *opaque)
{
    CdjAta *s = opaque;

    g_clear_pointer(&s->disc_path, g_byte_array_unref);
    g_clear_pointer(&s->mouth_path, g_byte_array_unref);
    return 0;
}

static bool ata_reopen(GByteArray *saved, CdjDisc **d)
{
    g_autofree char *path = NULL;
    char *err = NULL;

    *d = NULL;
    if (!saved || !saved->len) {
        return true;
    }
    path = g_strndup((const char *)saved->data, saved->len);
    *d = cdj_disc_open(path, &err);
    if (!*d) {
        error_report("cdj2000-ata: the saved disc cannot be opened: %s", err);
        g_free(err);
        return false;
    }
    return true;
}

static int ata_post_load(void *opaque, int version_id)
{
    CdjAta *s = opaque;
    bool ok;

    cdj_disc_free(s->disc);
    cdj_disc_free(s->mouth);
    ok = ata_reopen(s->disc_path, &s->disc) &&
         ata_reopen(s->mouth_path, &s->mouth);
    ata_post_save(s);
    return ok ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_cdj2000_ata = {
    .name = "cdj2000-atapi",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = ata_pre_save,
    .post_save = ata_post_save,
    .post_load = ata_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, CdjAta, CDJ_ATA_CHIP_SIZE / 4),
        VMSTATE_UINT32(latched, CdjAta),
        VMSTATE_BOOL(dma_active, CdjAta),
        VMSTATE_UINT32(dma_addr, CdjAta),
        VMSTATE_UINT32(dma_left, CdjAta),
        VMSTATE_TIMER_PTR(dma_timer, CdjAta),
        CDJ_VMSTATE_SPAN(CdjAta, feature, devctl),
        VMSTATE_BOOL(intrq, CdjAta),
        VMSTATE_UINT32(phase, CdjAta),
        VMSTATE_BOOL(packet, CdjAta),
        VMSTATE_BOOL(dma, CdjAta),
        VMSTATE_UINT8_ARRAY(cdb, CdjAta, 12),
        VMSTATE_UINT32(cdb_len, CdjAta),
        VMSTATE_UINT32(len, CdjAta),
        VMSTATE_UINT32(pos, CdjAta),
        VMSTATE_UINT32(chunk_left, CdjAta),
        VMSTATE_UINT32(limit, CdjAta),
        VMSTATE_VBUFFER_UINT32(buf, CdjAta, 0, NULL, len),
        VMSTATE_UINT8(sense_key, CdjAta),
        VMSTATE_UINT8(asc, CdjAta),
        VMSTATE_UINT8(ascq, CdjAta),
        VMSTATE_UINT32(action, CdjAta),
        VMSTATE_TIMER_PTR(cmd_timer, CdjAta),
        VMSTATE_INT32(mech, CdjAta),
        VMSTATE_TIMER_PTR(mech_timer, CdjAta),
        CDJ_VMSTATE_SPAN(CdjAta, svmode, svmode),
        CDJ_VMSTATE_BYTES(disc_path, CdjAta),
        CDJ_VMSTATE_BYTES(mouth_path, CdjAta),
        VMSTATE_END_OF_LIST()
    }
};

static void ata_instance_init(Object *obj)
{
    CdjAta *s = (CdjAta *)obj;

    s->trace = getenv("CDJ_ATA_TRACE") != NULL;
    s->bps = ata_read_rate();
    s->buf = g_malloc0(BUF_MAX);
    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ata_cmd_timer, s);
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ctl_dma_timer, s);
    s->mech_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ata_mech_timer, s);
    s->mech = MECH_EMPTY;
    ata_signature(s);
    object_property_add_str(obj, "disc", ata_disc_get, ata_disc_set);
    object_property_set_description(obj, "disc",
        "Slot media: insert [PATH], eject, remove; read mechanism state");
}

static void ata_instance_finalize(Object *obj)
{
    CdjAta *s = (CdjAta *)obj;

    timer_free(s->cmd_timer);
    timer_free(s->dma_timer);
    timer_free(s->mech_timer);
    cdj_disc_free(s->disc);
    cdj_disc_free(s->mouth);
    g_free(s->default_path);
    g_free(s->buf);
}

static const TypeInfo ata_type_info = {
    .name = TYPE_CDJ2000_ATAPI,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(CdjAta),
    .instance_init = ata_instance_init,
    .instance_finalize = ata_instance_finalize,
};

static void ata_register_types(void)
{
    type_register_static(&ata_type_info);
}
type_init(ata_register_types)

void cdj2000_ata_init(MemoryRegion *system, qemu_irq irq)
{
    Object *obj = object_new(TYPE_CDJ2000_ATAPI);
    CdjAta *s = (CdjAta *)obj;
    const char *path = getenv("CDJ_DISC");

    object_property_add_child(OBJECT(qdev_get_machine()), "cdj-atapi", obj);
    object_unref(obj);
    s->irq = irq;
    vmstate_register_any(NULL, &vmstate_cdj2000_ata, s);
    memory_region_init_io(&s->taskfile, obj, &ata_tf_ops, s,
                          "cdj2000.ata", CDJ_ATA_SIZE);
    memory_region_add_subregion(system, CDJ_ATA_BASE, &s->taskfile);
    memory_region_init_io(&s->chip, obj, &ata_chip_ops, s,
                          "cdj2000.atapi-ctl", CDJ_ATA_CHIP_SIZE);
    memory_region_add_subregion(system, CDJ_ATA_CHIP_BASE, &s->chip);

    if (path && *path) {
        char note[PATH_MAX + 64] = "";

        s->default_path = g_strdup(path);
        if (!ata_disc_load(s, path, note, sizeof(note))) {
            error_report("cdj2000-ata: CDJ_DISC: %s", note);
            exit(1);
        }
    }
    qemu_log("cdj2000-ata: drive present, %s\n",
             s->disc ? s->disc->path : "no disc");
}
