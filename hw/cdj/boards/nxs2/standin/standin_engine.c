/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "cdj_getenv.h"
/*
 * Behavioural DSP stand-in, from before the real C6655 was emulated. Every knob
 * here is off by default.
 *
 * CDJ_AUDIO_DRAIN=<spec>[,<spec>...]   up to 4 specs, each
 *     <addr>[:<per_tick>[:<tick_ms>[:<floor>[:<per_ship>[:<cap>]]]]]
 *     defaults per_tick 280, tick_ms 10, floor 0, per_ship 0, cap 0
 *
 * Every tick_ms, *addr -= per_tick down to floor, as a DSP consuming its queue.
 * With per_ship, each DMA1 ch3 audio ship adds per_ship (clamped to cap), so
 * the queue fills on ship and drains on time.
 *
 * The block sender in tsk_DJcontInBufFile throttles at 0x082ED6E8 on
 *     while (*(0x0994D3C8) == 0 && *(0x09947454) >= 2 * step) wait(10);
 * 0x09947454 (record+0x388) is republished by tsk_DJcontDSPoutCTL from
 * 0x09944170, so drain the source 0x09944170, not the copy.
 * e.g. CDJ_AUDIO_DRAIN=0x09944170:1:10:0:235:470 paces one 38,452-byte block
 * per ~2.35 s, about real time for a 128 kb/s track.
 *
 * The drain is timer-paced rather than tied to DMA completion; draining on
 * completion deadlocks (level only falls when audio moves and vice versa).
 */
#define CDJ_AUDIO_DRAIN_MAX 4

typedef struct {
    uint32_t addr;
    uint32_t per_tick;
    uint32_t floor;
    int64_t tick_ms;
    uint64_t ticks;
    uint64_t drained;
    /* Queue mode: added per ch3 audio ship, clamped to cap. 0 = drain only. */
    uint32_t per_ship;
    uint32_t cap;
    uint64_t filled;
    uint64_t ships;
    QEMUTimer *timer;
} CdjAudioDrain;

static struct {
    bool parsed;
    unsigned n;
    CdjAudioDrain t[CDJ_AUDIO_DRAIN_MAX];
} cdj_audio_drain;

static void cdj_audio_drain_tick(void *opaque)
{
    CdjAudioDrain *d = opaque;
    uint32_t level, out;

    cpu_physical_memory_read(A7ADDR(d->addr), &level, sizeof(level));
    level = le32_to_cpu(level);
    /*
     * 0xFFFFFFFF is the firmware's "not filled in yet" marker; e.g. 0x08496898
     * only initialises *(0x0B5125E8) while it reads -1. Never decrement it.
     */
    if (level == 0xFFFFFFFFu) {
        d->ticks++;
        timer_mod(d->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + d->tick_ms * 1000000);
        return;
    }
    /* In queue mode clamp the firmware's initial 0x0AF0 to cap at once. */
    if (d->per_ship && d->cap && level > d->cap) {
        level = d->cap;
        out = cpu_to_le32(level);
        cpu_physical_memory_write(A7ADDR(d->addr), &out, sizeof(out));
    }
    if (level > d->floor) {
        uint32_t sub = d->per_tick;

        level = (level > d->floor + sub) ? level - sub : d->floor;
        out = cpu_to_le32(level);
        cpu_physical_memory_write(A7ADDR(d->addr), &out, sizeof(out));
        d->drained += sub;
    }
    d->ticks++;
    timer_mod(d->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + d->tick_ms * 1000000);
}

/*
 * CDJ_DSP_ENGINE=<bytes per second>   default 16185 (~129 kb/s)
 *
 * A decode-position model fed the real ch3 stream: it separates the boot table
 * from the audio and advances a position at real time, never past the audio
 * actually held:
 *
 *     position_ms = min(time since the first audio byte,
 *                       audio bytes held / bytes per second)
 */
CdjDspEng cdj_dsp_eng;

/*
 * CDJ_DSP_ENGINE_GATE=<addr>[:<value>[:<mask>]]
 *
 * The position accrues only while (*addr & mask) == value, i.e. while the
 * firmware's own state says the deck plays. Sampled on the publish tick.
 *   0x0B054D44:3                          deck state 3 = playing
 *   0x0B058C10:0x00010000:0x00FF0000      the play flag byte
 */
static void cdj_dsp_engine_gate_parse(void)
{
    const char *e = getenv("CDJ_DSP_ENGINE_GATE");

    if (!e || !*e) {
        return;
    }
    cdj_dsp_eng.gate_addr = (uint32_t)strtoul(e, (char **)&e, 0);
    cdj_dsp_eng.gate_val = 0;
    cdj_dsp_eng.gate_mask = 0xFFFFFFFF;
    if (*e == ':') {
        cdj_dsp_eng.gate_val = (uint32_t)strtoul(e + 1, (char **)&e, 0);
    }
    if (*e == ':') {
        cdj_dsp_eng.gate_mask = (uint32_t)strtoul(e + 1, (char **)&e, 0);
    }
    if (!cdj_dsp_eng.gate_addr) {
        return;
    }
    cdj_dsp_eng.gate_on = true;
    cdj_dsp_eng.gate_last_ms = -1;
    info_report("dsp engine: GATED on *(0x%08x) & 0x%08x == 0x%x -- the "
                "position advances only while that holds",
                cdj_dsp_eng.gate_addr, cdj_dsp_eng.gate_mask,
                cdj_dsp_eng.gate_val);
}

/* A gate that opens and shuts within one publish tick is not seen. */
static void cdj_dsp_engine_gate_tick(void)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint32_t v = 0;
    bool pass;

    if (!cdj_dsp_eng.gate_on) {
        return;
    }
    cpu_physical_memory_read(A7ADDR(cdj_dsp_eng.gate_addr), &v, sizeof(v));
    v = le32_to_cpu(v);
    pass = (v & cdj_dsp_eng.gate_mask) == cdj_dsp_eng.gate_val;
    if (pass) {
        if (!cdj_dsp_eng.gate_pass) {
            info_report("dsp engine: PLAY -- the gate opened at %" PRId64 " ms",
                        now);
        }
        cdj_dsp_eng.gate_pass++;
        if (cdj_dsp_eng.gate_last_ms >= 0 && now > cdj_dsp_eng.gate_last_ms) {
            cdj_dsp_eng.play_ms += now - cdj_dsp_eng.gate_last_ms;
        }
    } else {
        cdj_dsp_eng.gate_fail++;
    }
    cdj_dsp_eng.gate_last_ms = now;
}

/*
 * Publish the decode position into MAIN's DJcont record 0x099470CC. MAIN has no
 * producer for these fields; REMAIN is total - position (0x084E6F52) and the
 * centre waveform scrolls on the same pair.
 *
 *     +0x3AC  position, 1/150 s (0x09947478)
 *     +0x3B0  the alternate view 0x08429336 selects when FUN_0844750E() != 0
 *     +0x3B4  sibling
 *
 * All three get the same value. Nothing is written before the first audio byte.
 *
 *   CDJ_DSP_ENGINE_POS=<record base>   e.g. 0x099470CC
 *   CDJ_DSP_ENGINE_HZ=<n>              refresh rate, default 50 (not measured)
 *   CDJ_DSP_ENGINE_RAW=1               treat POS as one bare address
 */
static QEMUTimer *cdj_dsp_eng_timer;

#define CDJ_DSP_ENG_POS_OFF   0x3AC
#define CDJ_DSP_ENG_POS_ALT   0x3B0
#define CDJ_DSP_ENG_POS_ALT2  0x3B4

/*
 * CDJ_JOGDRIVE=<bend_pct>[:<scrub_ms>][:<mov>][:<fwd>][:<touch>]
 *   defaults 80, 40, 0x0B54CB3C, 0x0B54CB40, 0x0B54CB38
 *
 * The panel decoder (0x0844D4AE/C4/DA) tests report[0x0F] bits 0x20/0x40/0x80
 * and FUN_084E1780/94/A8 store touch, forward and moving into 0x0B54CB38,
 * 0x0B54CB40 and 0x0B54CB3C (+ deck * 0x2CC). The position is this model's,
 * so the jog is applied here, reading MAIN's decoded slots:
 *
 *   moving, not touched   bend: bend_pct% faster or slower while it turns
 *   touched               the record is held
 *   moving and touched    scrub: scrub_ms per 20 ms
 *
 * Forward bit set = forward, clear = reverse.
 */
static void cdj_jog_parse(void)
{
    const char *e = getenv("CDJ_JOGDRIVE");
    char *end;

    cdj_dsp_eng.jog_parsed = true;
    cdj_dsp_eng.jog_last_ms = -1;
    cdj_dsp_eng.jog_last_state = -1;
    cdj_dsp_eng.jog_mov_addr = 0x0B54CB3C;
    cdj_dsp_eng.jog_fwd_addr = 0x0B54CB40;
    cdj_dsp_eng.jog_touch_addr = 0x0B54CB38;
    cdj_dsp_eng.jog_bend_pct = 80;
    cdj_dsp_eng.jog_scrub_ms = 40;
    if (!e || !*e) {
        return;
    }
    cdj_dsp_eng.jog_bend_pct = (int32_t)strtol(e, &end, 0);
    if (*end == ':') {
        cdj_dsp_eng.jog_scrub_ms = (int32_t)strtol(end + 1, &end, 0);
    }
    if (*end == ':') {
        cdj_dsp_eng.jog_mov_addr = (uint32_t)strtoul(end + 1, &end, 0);
    }
    if (*end == ':') {
        cdj_dsp_eng.jog_fwd_addr = (uint32_t)strtoul(end + 1, &end, 0);
    }
    if (*end == ':') {
        cdj_dsp_eng.jog_touch_addr = (uint32_t)strtoul(end + 1, &end, 0);
    }
    cdj_dsp_eng.jog_on = true;
    info_report("jog drive: the platter moves the track -- bend %+d%%, scrub "
                "%d ms per 20 ms, from MAIN's own slots mov 0x%08x fwd 0x%08x "
                "touch 0x%08x. The EMULATOR implements this; MAIN has no "
                "producer for the play position at all",
                cdj_dsp_eng.jog_bend_pct, cdj_dsp_eng.jog_scrub_ms,
                cdj_dsp_eng.jog_mov_addr, cdj_dsp_eng.jog_fwd_addr,
                cdj_dsp_eng.jog_touch_addr);
}

/* bit 0 moving, bit 1 forward, bit 2 touched -- MAIN's own decoded state. */
static int cdj_jog_state(void)
{
    uint32_t v = 0;
    int st = 0;

    cpu_physical_memory_read(A7ADDR(cdj_dsp_eng.jog_mov_addr), &v, sizeof(v));
    st |= le32_to_cpu(v) ? 1 : 0;
    cpu_physical_memory_read(A7ADDR(cdj_dsp_eng.jog_fwd_addr), &v, sizeof(v));
    st |= le32_to_cpu(v) ? 2 : 0;
    cpu_physical_memory_read(A7ADDR(cdj_dsp_eng.jog_touch_addr), &v, sizeof(v));
    st |= le32_to_cpu(v) ? 4 : 0;
    return st;
}

/* One publish tick of the platter; integrated so a rate change keeps history. */
static void cdj_jog_tick(void)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    int64_t dt;
    int st, dir;

    if (!cdj_dsp_eng.jog_parsed) {
        cdj_jog_parse();
    }
    if (!cdj_dsp_eng.jog_on) {
        return;
    }
    st = cdj_jog_state();
    cdj_dsp_eng.jog_held = (st & 4) != 0;
    dt = (cdj_dsp_eng.jog_last_ms >= 0 && now > cdj_dsp_eng.jog_last_ms)
         ? now - cdj_dsp_eng.jog_last_ms : 0;
    cdj_dsp_eng.jog_last_ms = now;
    if (st != cdj_dsp_eng.jog_last_state) {
        cdj_dsp_eng.jog_last_state = st;
        info_report("jog drive: MAIN's platter is mov=%d fwd=%d touch=%d "
                    "at %" PRId64 " ms", st & 1, (st >> 1) & 1, (st >> 2) & 1,
                    now);
    }
    if (!(st & 1) || !dt) {
        return;                      /* the platter is not turning */
    }
    dir = (st & 2) ? 1 : -1;
    if (st & 4) {
        cdj_dsp_eng.jog_off_ms +=
            dir * (double)cdj_dsp_eng.jog_scrub_ms * (double)dt / 20.0;
        cdj_dsp_eng.jog_scrub_ticks++;
    } else {
        cdj_dsp_eng.jog_off_ms +=
            dir * (double)cdj_dsp_eng.jog_bend_pct * (double)dt / 100.0;
        cdj_dsp_eng.jog_bend_ticks++;
    }
}

static void cdj_dsp_engine_publish(void *opaque)
{
    uint32_t v;
    uint64_t pos;

    if (!cdj_dsp_eng.pos_addr) {
        return;
    }
    cdj_dsp_engine_gate_tick();
    cdj_jog_tick();
    /* Write nothing before the first audio byte. */
    if (cdj_dsp_eng.first_audio_ms < 0) {
        goto rearm;
    }
    pos = cdj_dsp_engine_pos_ms();
    v = cpu_to_le32((uint32_t)(pos * 3 / 20));   /* ms -> 1/150 s */

    if (cdj_dsp_eng.raw) {
        cpu_physical_memory_write(A7ADDR(cdj_dsp_eng.pos_addr), &v, sizeof(v));
    } else {
        uint32_t base = cdj_dsp_eng.pos_addr;

        cpu_physical_memory_write(A7ADDR(base + CDJ_DSP_ENG_POS_OFF),
                                  &v, sizeof(v));
        cpu_physical_memory_write(A7ADDR(base + CDJ_DSP_ENG_POS_ALT),
                                  &v, sizeof(v));
        cpu_physical_memory_write(A7ADDR(base + CDJ_DSP_ENG_POS_ALT2),
                                  &v, sizeof(v));
    }
    cdj_dsp_eng.published++;
    {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (now - cdj_dsp_eng.last_pub_report_ms >= 1000) {
            cdj_dsp_eng.last_pub_report_ms = now;
            info_report("dsp engine: t=%" PRId64 " ms, position %" PRIu64
                        ".%03u s%s", now, pos / 1000, (unsigned)(pos % 1000),
                        cdj_dsp_eng.gate_on
                        ? (cdj_dsp_eng.gate_pass ? " (playing)" : " (gate shut)")
                        : "");
        }
    }
rearm:
    timer_mod(cdj_dsp_eng_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + 1000000000LL / cdj_dsp_eng.hz);
}

void cdj_dsp_engine_init(void)
{
    const char *e = getenv("CDJ_DSP_ENGINE");

    if (!e) {
        return;
    }
    cdj_dsp_eng.on = true;
    cdj_dsp_eng.rate = *e ? (unsigned)strtoul(e, NULL, 0) : 0;
    if (!cdj_dsp_eng.rate) {
        cdj_dsp_eng.rate = 16185;
    }
    cdj_dsp_eng.first_audio_ms = -1;

    e = getenv("CDJ_DSP_ENGINE_POS");
    if (e) {
        cdj_dsp_eng.pos_addr = (uint32_t)strtoul(e, NULL, 0);
        cdj_dsp_eng.raw = getenv("CDJ_DSP_ENGINE_RAW") != NULL;
        e = getenv("CDJ_DSP_ENGINE_HZ");
        cdj_dsp_eng.hz = e ? (unsigned)strtoul(e, NULL, 0) : 50;
        if (!cdj_dsp_eng.hz) {
            cdj_dsp_eng.hz = 50;
        }
        cdj_dsp_engine_gate_parse();
        cdj_jog_parse();
        cdj_dsp_eng_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         cdj_dsp_engine_publish, NULL);
        timer_mod(cdj_dsp_eng_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + 1000000000LL / cdj_dsp_eng.hz);
        info_report("dsp engine: publishing the decode position %s 0x%08x "
                    "at %u Hz, in 1/150 s units, not before the first audio byte",
                    cdj_dsp_eng.raw ? "to the bare address"
                                    : "into the status block at record",
                    cdj_dsp_eng.pos_addr, cdj_dsp_eng.hz);
    }
    info_report("dsp engine: on, %u bytes/s", cdj_dsp_eng.rate);
}

/*
 * CDJ_CUEKEY=1[:<cue_ms>][:<cue_off>:<cue_mask>][:<play_off>:<play_mask>]
 *   defaults: cue_ms 0, cue = report[0x10] bit 0x02, play = report[0x10] bit 0x01
 *
 * In the stand-in era the cue key word 0x0B54CB24 had no consumer (setter
 * FUN_084E1A0E, getter FUN_084E23C8, read only by the console key printer
 * FUN_0844B566), so cue is applied to this model's position: a cue press parks
 * the decoder on the cue point and play resumes from it.
 */
static void cdj_dsp_engine_cue_parse(void)
{
    const char *e = getenv("CDJ_CUEKEY");
    char *end;

    cdj_dsp_eng.cue_parsed = true;
    cdj_dsp_eng.cue_off = 0x10;
    cdj_dsp_eng.cue_mask = 0x02;
    cdj_dsp_eng.play_off = 0x10;
    cdj_dsp_eng.play_mask = 0x01;
    if (!e || !*e || !strtoul(e, &end, 0)) {
        return;
    }
    cdj_dsp_eng.cue_ms = (*end == ':') ? strtoll(end + 1, &end, 0) : 0;
    if (*end == ':') {
        cdj_dsp_eng.cue_off = strtoul(end + 1, &end, 0);
        cdj_dsp_eng.cue_mask = (*end == ':') ? strtoul(end + 1, &end, 0) : 0x02;
    }
    if (*end == ':') {
        cdj_dsp_eng.play_off = strtoul(end + 1, &end, 0);
        cdj_dsp_eng.play_mask = (*end == ':') ? strtoul(end + 1, &end, 0) : 0x01;
    }
    cdj_dsp_eng.cue_on = true;
    info_report("cue key: report[0x%02x] bit 0x%02x parks the decoder on "
                "%" PRId64 " ms and report[0x%02x] bit 0x%02x resumes from it "
                "-- the EMULATOR implements this, the firmware has no consumer "
                "for the cue key at all",
                cdj_dsp_eng.cue_off, cdj_dsp_eng.cue_mask, cdj_dsp_eng.cue_ms,
                cdj_dsp_eng.play_off, cdj_dsp_eng.play_mask);
}

/* Rebase the position clock onto the cue point. */
static void cdj_dsp_engine_resume_from_cue(int64_t now)
{
    /* Fold in any platter movement made while parked, exactly once. */
    double start = (double)cdj_dsp_eng.cue_ms + cdj_dsp_eng.jog_off_ms;
    int64_t from = start > 0.0 ? (int64_t)start : 0;

    cdj_dsp_eng.jog_off_ms = 0.0;
    cdj_dsp_eng.cued = false;
    cdj_dsp_eng.play_ms = from;
    if (cdj_dsp_eng.first_audio_ms >= 0) {
        cdj_dsp_eng.first_audio_ms = now - from;
    }
}

static bool cdj_dsp_engine_advancing(int64_t now)
{
    return !cdj_dsp_eng.cued && now - cdj_dsp_eng.last_move_ms < 150;
}

/*
 * Cue button behaviour:
 *
 *   CUE while playing           back to the cue point and pause
 *   CUE while paused elsewhere  the cue point moves to the current position
 *   CUE held on the cue point   preview while held, back to the cue on release
 *   PLAY while previewing       keep playing after the release
 *
 * The firmware still owns the transport, so wherever this starts or stops the
 * deck it returns 1 and the caller injects a PLAY toggle.
 *
 * A held key arrives as repeated presses; a press within cue_until_ms of the
 * last is the same hold, and silence past it (cdj_dsp_engine_key_poll) is the
 * release.
 */
static int cdj_dsp_engine_cue_release(int64_t now)
{
    cdj_dsp_eng.cue_down = false;
    if (!cdj_dsp_eng.previewing) {
        return 0;
    }
    cdj_dsp_eng.previewing = false;
    cdj_dsp_eng.cued = true;
    cdj_dsp_eng.jog_off_ms = 0.0;
    info_report("cue key: released at %" PRId64 " ms -- back to the cue point "
                "%" PRId64 " ms", now, cdj_dsp_eng.cue_ms);
    return 1;
}

int cdj_dsp_engine_key(unsigned off, unsigned mask, int64_t dur_ms,
                              bool held, int64_t now)
{
    if (!cdj_dsp_eng.cue_parsed) {
        cdj_dsp_engine_cue_parse();
    }
    /* Ignore cue before any audio, so boot presses inject no PLAY toggles. */
    if (!cdj_dsp_eng.cue_on || !cdj_dsp_eng.on
        || cdj_dsp_eng.first_audio_ms < 0) {
        return 0;
    }
    if (off == cdj_dsp_eng.cue_off && (mask & cdj_dsp_eng.cue_mask)) {
        int64_t until = held ? INT64_MAX : now + (dur_ms ? dur_ms : 120) + 60;

        if (cdj_dsp_eng.cue_down && now <= cdj_dsp_eng.cue_until_ms) {
            cdj_dsp_eng.cue_until_ms = until;       /* the same hold */
            return 0;
        }
        cdj_dsp_eng.cue_down = true;
        cdj_dsp_eng.cue_until_ms = until;
        cdj_dsp_eng.cue_presses++;
        if (cdj_dsp_eng.cued) {
            cdj_dsp_engine_resume_from_cue(now);
            cdj_dsp_eng.previewing = true;
            info_report("cue key: CUE held on the cue point at %" PRId64
                        " ms -- previewing from %" PRId64 " ms",
                        now, cdj_dsp_eng.cue_ms);
            return 1;
        }
        if (cdj_dsp_engine_advancing(now)) {
            cdj_dsp_eng.cued = true;
            cdj_dsp_eng.jog_off_ms = 0.0;
            info_report("cue key: CUE while playing at %" PRId64 " ms -- back "
                        "to %" PRId64 " ms and paused (press %" PRIu64 ")",
                        now, cdj_dsp_eng.cue_ms, cdj_dsp_eng.cue_presses);
            return 1;
        }
        cdj_dsp_eng.cue_ms = (int64_t)cdj_dsp_engine_pos_ms();
        cdj_dsp_eng.jog_off_ms = 0.0;      /* already inside the position */
        cdj_dsp_eng.cued = true;
        info_report("cue key: CUE while paused at %" PRId64 " ms -- the cue "
                    "point is now %" PRId64 " ms", now, cdj_dsp_eng.cue_ms);
        return 0;
    }
    if (off == cdj_dsp_eng.play_off && (mask & cdj_dsp_eng.play_mask)) {
        cdj_dsp_eng.play_presses++;
        if (cdj_dsp_eng.previewing) {
            /* The firmware takes this press as a pause; undo it so the deck
             * keeps playing once the cue key comes up. */
            cdj_dsp_eng.previewing = false;
            info_report("cue key: PLAY during the preview at %" PRId64
                        " ms -- the deck keeps playing", now);
            return 1;
        }
        if (cdj_dsp_eng.cued) {
            cdj_dsp_engine_resume_from_cue(now);
            info_report("cue key: PLAY at %" PRId64 " ms -- resuming from "
                        "%" PRId64 " ms", now, cdj_dsp_eng.cue_ms);
        }
    }
    return 0;
}

/* A released cue key: an explicit "rel", or silence past the last re-send. */
int cdj_dsp_engine_key_up(unsigned off, unsigned mask, int64_t now)
{
    if (cdj_dsp_eng.cue_down && off == cdj_dsp_eng.cue_off
        && (mask & cdj_dsp_eng.cue_mask)) {
        return cdj_dsp_engine_cue_release(now);
    }
    return 0;
}

int cdj_dsp_engine_key_poll(int64_t now)
{
    if (cdj_dsp_eng.cue_down && now > cdj_dsp_eng.cue_until_ms) {
        return cdj_dsp_engine_cue_release(now);
    }
    return 0;
}

/*
 * The position a listener would hear: not capped by the held audio, jog offset
 * included. The live decoder follows this; last_move_ms marks it advancing.
 */
static void cdj_dsp_engine_note_heard(double heard_ms)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (heard_ms < 0.0) {
        heard_ms = 0.0;
    }
    if ((uint64_t)heard_ms != cdj_dsp_eng.last_pos_ms) {
        cdj_dsp_eng.last_pos_ms = (uint64_t)heard_ms;
        cdj_dsp_eng.last_move_ms = now;
    }
    cdj_dsp_eng.heard_ms = heard_ms;
}

/*
 * Current rate of the heard position in track seconds per second: pitch factor
 * unless the platter is held, plus jog bend or scrub. Negative when scrubbing
 * backwards.
 */
double cdj_dsp_engine_rate(void)
{
    double f = 1.0 + (double)cdj_pitch_pct() / 10000.0;
    double r;
    int st = cdj_dsp_eng.jog_last_state;

    if (cdj_dsp_eng.cued && !cdj_dsp_eng.jog_on) {
        return 0.0;
    }
    if (f < 0.01) {
        f = 0.01;
    }
    r = (cdj_dsp_eng.cued || cdj_dsp_eng.jog_held) ? 0.0 : f;
    if (cdj_dsp_eng.jog_on && st > 0 && (st & 1)) {
        int dir = (st & 2) ? 1 : -1;

        r += (st & 4) ? dir * (double)cdj_dsp_eng.jog_scrub_ms / 20.0
                      : dir * (double)cdj_dsp_eng.jog_bend_pct / 100.0;
    }
    return r;
}

uint64_t cdj_dsp_engine_pos_ms(void)
{
    int64_t now, wall;
    uint64_t held;
    double scrubbed;

    if (!cdj_dsp_eng.on || cdj_dsp_eng.first_audio_ms < 0) {
        return 0;
    }
    if (cdj_dsp_eng.cued) {
        /* Parked: report the cue point and re-park the integrator on it. */
        double parked = (double)cdj_dsp_eng.cue_ms + cdj_dsp_eng.jog_off_ms;

        cdj_dsp_eng.pos_acc_ms = (double)cdj_dsp_eng.cue_ms;
        cdj_dsp_eng.pos_init = false;
        /* A cued deck can still be scrubbed. */
        cdj_dsp_engine_note_heard(parked);
        return parked > 0.0 ? (uint64_t)parked : 0;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    wall = cdj_dsp_eng.gate_on ? cdj_dsp_eng.play_ms
                               : now - cdj_dsp_eng.first_audio_ms;
    if (wall < 0) {
        wall = 0;
    }
    /*
     * Integrate the advance at the pitch rate in force for each interval, so a
     * fader move changes speed without jumping the position.
     */
    if (!cdj_dsp_eng.pos_init) {
        cdj_dsp_eng.pos_init = true;
        cdj_dsp_eng.pos_acc_ms = (double)wall;
        cdj_dsp_eng.pos_base_ms = wall;
    }
    if (wall > cdj_dsp_eng.pos_base_ms) {
        double f = 1.0 + (double)cdj_pitch_pct() / 10000.0;

        if (f < 0.01) {
            f = 0.01;
        }
        /*
         * A touched platter holds the record. The base clock still advances,
         * so release resumes rather than catching up.
         */
        if (!cdj_dsp_eng.jog_held) {
            cdj_dsp_eng.pos_acc_ms += (double)(wall - cdj_dsp_eng.pos_base_ms) * f;
        }
    }
    cdj_dsp_eng.pos_base_ms = wall;

    /* Jog offset is added here so a scrub moves the deck even with the gate shut. */
    scrubbed = cdj_dsp_eng.pos_acc_ms + cdj_dsp_eng.jog_off_ms;
    if (scrubbed < 0.0) {
        scrubbed = 0.0;
    }
    cdj_dsp_engine_note_heard(scrubbed);

    /* Never decode audio that has not arrived. */
    held = cdj_dsp_eng.audio_bytes * 1000 / cdj_dsp_eng.rate;
    return (uint64_t)scrubbed < held ? (uint64_t)scrubbed : held;
}

/* One ch3 transfer: the DSP boot table comes from MAIN flash (0x08xxxxxx),
 * audio from RAM. */
void cdj_dsp_engine_ship(unsigned ch, uint32_t sar, uint32_t n)
{
    uint32_t phys = A7ADDR(sar);
    int64_t now;

    if (!cdj_dsp_eng.on || ch != 3 || !n) {
        return;
    }
    if ((phys >> 24) == 0x08) {
        cdj_dsp_eng.boot_bytes += n;
        return;
    }
    cdj_dsp_eng.ships++;
    cdj_dsp_eng.audio_bytes += n;
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (cdj_dsp_eng.first_audio_ms < 0) {
        cdj_dsp_eng.first_audio_ms = now;
        info_report("dsp engine: first audio byte at %" PRId64 " ms "
                    "(boot table %" PRIu64 " bytes already in)",
                    now, cdj_dsp_eng.boot_bytes);
    }
    if (now - cdj_dsp_eng.last_report_ms >= 1000) {
        uint64_t pos = cdj_dsp_engine_pos_ms();

        cdj_dsp_eng.last_report_ms = now;
        info_report("dsp engine: %u ships, %" PRIu64 " audio bytes held "
                    "(%" PRIu64 " ms of audio), decode position %" PRIu64
                    ".%03u s, published %" PRIu64 "x",
                    cdj_dsp_eng.ships, cdj_dsp_eng.audio_bytes,
                    cdj_dsp_eng.audio_bytes * 1000 / cdj_dsp_eng.rate,
                    pos / 1000, (unsigned)(pos % 1000), cdj_dsp_eng.published);
        if (cdj_dsp_eng.gate_on) {
            info_report("dsp engine: gate open %" PRIu64 " ticks, shut %"
                        PRIu64 ", %" PRId64 " ms of PLAYING time accrued",
                        cdj_dsp_eng.gate_pass, cdj_dsp_eng.gate_fail,
                        cdj_dsp_eng.play_ms);
        }
    }
}

void cdj_audio_queue_ship(unsigned ch)
{
    unsigned i;

    /* DMA1 ch3 -> CS6 is the audio ship; nothing else fills the DSP queue. */
    if (ch != 3) {
        return;
    }
    for (i = 0; i < cdj_audio_drain.n; i++) {
        CdjAudioDrain *d = &cdj_audio_drain.t[i];
        uint32_t level, out;

        if (!d->per_ship) {
            continue;                     /* pure-drain spec: old behaviour */
        }
        cpu_physical_memory_read(A7ADDR(d->addr), &level, sizeof(level));
        level = le32_to_cpu(level);
        if (level == 0xFFFFFFFFu) {
            continue;      /* the firmware's unset sentinel -- never touch it */
        }
        level += d->per_ship;
        if (d->cap && level > d->cap) {
            level = d->cap;               /* a queue has a depth */
        }
        out = cpu_to_le32(level);
        cpu_physical_memory_write(A7ADDR(d->addr), &out, sizeof(out));
        d->filled += d->per_ship;
        d->ships++;
    }
}

void cdj_audio_drain_init(void)
{
    const char *e = getenv("CDJ_AUDIO_DRAIN");
    char *end;

    if (cdj_audio_drain.parsed) {
        return;
    }
    cdj_audio_drain.parsed = true;
    if (!e || !*e) {
        return;
    }

    while (*e && cdj_audio_drain.n < CDJ_AUDIO_DRAIN_MAX) {
        CdjAudioDrain *d = &cdj_audio_drain.t[cdj_audio_drain.n];

        d->addr = strtoul(e, &end, 0);
        d->per_tick = (*end == ':') ? strtoul(end + 1, &end, 0) : 280;
        d->tick_ms = (*end == ':') ? strtoll(end + 1, &end, 0) : 10;
        d->floor = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        d->per_ship = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        d->cap = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        if (!d->per_tick) {
            d->per_tick = 280;
        }
        if (d->tick_ms <= 0) {
            d->tick_ms = 10;
        }
        if (!d->addr) {
            warn_report("audio drain: spec with no address -- ignored");
        } else {
            info_report("audio drain: *0x%08x -= %u every %" PRId64 " ms, "
                        "floor %u (the DSP is playing what it was given)",
                        d->addr, d->per_tick, d->tick_ms, d->floor);
            if (d->per_ship) {
                info_report("audio QUEUE: *0x%08x += %u per ch3 ship, cap %u "
                            "-- %.2f s of drain per block",
                            d->addr, d->per_ship, d->cap,
                            (double)d->per_ship * (double)d->tick_ms
                            / (double)d->per_tick / 1000.0);
            }
            d->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    cdj_audio_drain_tick, d);
            timer_mod(d->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                      + d->tick_ms * 1000000);
            cdj_audio_drain.n++;
        }
        while (*end && *end != ',') {
            end++;
        }
        e = (*end == ',') ? end + 1 : end;
    }
}

