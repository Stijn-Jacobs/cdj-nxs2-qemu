/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/*
 * Stand-in audio output, from before the real DSP was emulated. Off by default.
 *
 * MAIN has no PCM of its own: on the board the DSP decodes and the FPGA feeds
 * the DAC. What MAIN ships on DMA1 ch3 is the track's MP3 bitstream verbatim,
 * so this file decodes that stream with minimp3 (CC0) into a QEMU audio voice
 * (CDJ_AUDIO_LIVE) and/or appends it to a playable file (CDJ_AUDIO_OUT).
 */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
/*
 * CDJ_AUDIO_OUT=<path>   append ch3's de-duplicated MP3 frames to a file.
 *
 * Reads the DMA source only; the guest is not touched. MAIN ships windows of
 * about 63,000 bytes whose start advances about 38,452, so consecutive windows
 * overlap and must be de-duplicated before appending.
 */
#define CDJ_AOUT_TAIL   (128 * 1024)
#define CDJ_AOUT_PROBE  64

static struct {
    bool parsed;
    FILE *fp;
    uint8_t *tail;              /* the previous window, for the overlap test */
    uint32_t tail_len;
    uint64_t written, skipped, windows, jumps;
} cdj_aout;

/*
 * De-duplicate by MPEG frame, not by byte window. MAIN sometimes re-ships from
 * a different offset, so a window can have no overlap with the previous one;
 * hashing each self-delimiting frame drops re-shipped audio in any order while
 * a real gap stays a gap. A partial frame at a window edge is carried over.
 */
static void cdj_audio_emit_frame(const uint8_t *f, uint32_t n);

#define CDJ_AOUT_SEEN   8192            /* frames remembered: ~3.5 minutes */
#define CDJ_AOUT_CARRY  4096

static const unsigned cdj_mp3_kbps[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const unsigned cdj_mp3_hz[4] = { 44100, 48000, 32000, 0 };

/* Frame length at buf, or 0 if this is not an MPEG-1 Layer III header. */
static unsigned cdj_mp3_frame_len(const uint8_t *b, uint32_t avail)
{
    unsigned kbps, hz, pad;

    if (avail < 4 || b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return 0;
    }
    if (((b[1] >> 3) & 3) != 3 || ((b[1] >> 1) & 3) != 1) {
        return 0;                       /* not MPEG-1, not Layer III */
    }
    kbps = cdj_mp3_kbps[(b[2] >> 4) & 0xF];
    hz = cdj_mp3_hz[(b[2] >> 2) & 3];
    pad = (b[2] >> 1) & 1;
    if (!kbps || !hz) {
        return 0;
    }
    return 144000 * kbps / hz + pad;
}

static struct {
    uint64_t seen[CDJ_AOUT_SEEN];
    uint32_t n;                         /* ring position                    */
    uint8_t carry[CDJ_AOUT_CARRY];
    uint32_t carry_len;
    uint64_t emitted, dropped, unparsed;
} cdj_frames;

static bool cdj_frame_is_new(const uint8_t *f, unsigned len)
{
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
    unsigned i;

    for (i = 0; i < len; i++) {
        h = (h ^ f[i]) * 1099511628211ULL;
    }
    for (i = 0; i < CDJ_AOUT_SEEN; i++) {
        if (cdj_frames.seen[i] == h) {
            return false;
        }
    }
    cdj_frames.seen[cdj_frames.n++ % CDJ_AOUT_SEEN] = h;
    return true;
}

/* Emit every frame in buf that has not been emitted before. */
static void cdj_frames_emit(const uint8_t *buf, uint32_t n,
                            void (*sink)(const uint8_t *, uint32_t))
{
    g_autofree uint8_t *joined = NULL;
    const uint8_t *b;
    uint32_t len, off = 0;

    if (cdj_frames.carry_len) {
        joined = g_malloc(cdj_frames.carry_len + n);
        memcpy(joined, cdj_frames.carry, cdj_frames.carry_len);
        memcpy(joined + cdj_frames.carry_len, buf, n);
        b = joined;
        len = cdj_frames.carry_len + n;
        cdj_frames.carry_len = 0;
    } else {
        b = buf;
        len = n;
    }
    while (off < len) {
        unsigned flen = cdj_mp3_frame_len(b + off, len - off);

        if (!flen) {
            off++;                      /* resync */
            cdj_frames.unparsed++;
            continue;
        }
        if (off + flen > len) {
            break;                      /* a partial frame: carry it */
        }
        if (cdj_frame_is_new(b + off, flen)) {
            sink(b + off, flen);
            cdj_frames.emitted++;
        } else {
            cdj_frames.dropped++;
        }
        off += flen;
    }
    if (len - off && len - off <= CDJ_AOUT_CARRY) {
        memcpy(cdj_frames.carry, b + off, len - off);
        cdj_frames.carry_len = len - off;
    }
}

/*
 * Live decoder. The audio backend pulls and only then is a frame decoded, so
 * the sound is paced by the sink rather than by MAIN's bursty ships.
 *
 *     CDJ_AUDIO_LIVE=1            enable
 *     CDJ_AUDIO_LIVE_MB=<n>       compressed backlog cap, default 12
 *
 * Pick the device with QEMU's own switch, e.g. -audiodev pa,id=cdj or
 * -audiodev wav,id=cdj,path=/tmp/deck.wav.
 *
 * With CDJ_DSP_ENGINE the decoder follows the engine's position: frame k
 * starts at k * 1152 / 44100 s. A still position gives silence, a drift over
 * ~100 ms seeks, otherwise samples are stepped at the engine's rate
 * (varispeed). Without it, frames play in order.
 */
#define CDJ_ALIVE_MAX_FRAMES  (1 << 17)     /* ~57 minutes of 44.1 kHz audio */
#define CDJ_ALIVE_FRAME_MS    (1152.0 * 1000.0 / 44100.0)
#define CDJ_ALIVE_DRIFT       4             /* frames, ~100 ms               */
#define CDJ_ALIVE_STILL_MS    80      /* > one 50 Hz publish tick */

static struct {
    bool parsed, on, opened, failed;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    mp3dec_t dec;
    uint8_t *cbuf;              /* compressed bitstream, append-only        */
    size_t cap, len;            /* capacity, filled                         */
    uint32_t *foff;             /* byte offset of every frame in cbuf       */
    uint32_t nframes;
    uint32_t cur;               /* the next frame to decode                 */
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_frames, channels;   /* staged sample frames and their layout    */
    double pcm_pos;             /* fractional read point into pcm           */
    double rate;                /* the slewed playback rate                 */
    uint64_t frames, samples, starved, seeks, still;
    int64_t last_report_ms;
} cdj_alive;

/* Decode frame cdj_alive.cur into pcm; false when it has not arrived yet. */
static bool cdj_alive_decode_next(void)
{
    mp3dec_frame_info_t info;
    int got;

    while (cdj_alive.cur < cdj_alive.nframes) {
        uint32_t off = cdj_alive.foff[cdj_alive.cur];

        got = mp3dec_decode_frame(&cdj_alive.dec, cdj_alive.cbuf + off,
                                  (int)(cdj_alive.len - off), cdj_alive.pcm,
                                  &info);
        cdj_alive.cur++;
        if (got > 0) {
            cdj_alive.pcm_frames = got;
            cdj_alive.channels = info.channels;
            cdj_alive.frames++;
            cdj_alive.samples += got;
            return true;
        }
    }
    cdj_alive.starved++;
    return false;
}

static void cdj_audio_live_cb(void *opaque, int avail)
{
    double step = 1.0;
    short out[512];
    int n = 0;

    {
        /* Reported from the pull side: the whole track can be shipped before
         * PLAY, so a report on feed would go quiet once playback starts. */
        int64_t t = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (t - cdj_alive.last_report_ms >= 2000) {
            cdj_alive.last_report_ms = t;
            info_report("audio live: %" PRIu64 " frames decoded, %.1f s of "
                        "sound, frame %u of %u shipped, position %.1f s, "
                        "%" PRIu64 " seeks, %" PRIu64 " still pulls, %" PRIu64
                        " starves", cdj_alive.frames,
                        (double)cdj_alive.samples / 44100.0, cdj_alive.cur,
                        cdj_alive.nframes, cdj_dsp_eng.heard_ms / 1000.0,
                        cdj_alive.seeks, cdj_alive.still, cdj_alive.starved);
        }
    }

    if (cdj_dsp_eng.on && cdj_dsp_eng.first_audio_ms >= 0) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        uint32_t want;

        /*
         * Follow the integrator, not the published position: the published one
         * is capped at the audio held and only moves once per ship.
         */
        cdj_dsp_engine_pos_ms();
        if (now - cdj_dsp_eng.last_move_ms > CDJ_ALIVE_STILL_MS) {
            cdj_alive.still++;
            return;                     /* the deck is not advancing: silence */
        }
        want = (uint32_t)(cdj_dsp_eng.heard_ms / CDJ_ALIVE_FRAME_MS);
        if (want + CDJ_ALIVE_DRIFT < cdj_alive.cur ||
            want > cdj_alive.cur + CDJ_ALIVE_DRIFT) {
            cdj_alive.cur = want < cdj_alive.nframes ? want : cdj_alive.nframes;
            cdj_alive.pcm_frames = 0;
            cdj_alive.pcm_pos = 0;
            cdj_alive.seeks++;
        }
        /*
         * Use the engine's own rate (pitch, jog, scrub) and slew towards it.
         * A rate measured from the position jitters, because the position only
         * steps on the 50 Hz publish tick.
         */
        {
            double target = cdj_dsp_engine_rate();

            if (target <= 0.05) {
                cdj_alive.still++;
                return;                 /* held, parked, or turning backwards */
            }
            if (cdj_alive.rate <= 0.0) {
                cdj_alive.rate = target;
            }
            cdj_alive.rate += (target - cdj_alive.rate) * 0.2;
            step = cdj_alive.rate;
        }
        if (step < 0.25) {
            step = 0.25;
        }
        if (step > 4.0) {
            step = 4.0;
        }
    }

    while (avail >= 4) {
        int idx;

        if ((int)cdj_alive.pcm_pos >= cdj_alive.pcm_frames) {
            cdj_alive.pcm_pos -= cdj_alive.pcm_frames;
            if (cdj_alive.pcm_pos < 0) {
                cdj_alive.pcm_pos = 0;
            }
            if (!cdj_alive_decode_next()) {
                break;                  /* not shipped yet: silence */
            }
            continue;
        }
        idx = (int)cdj_alive.pcm_pos;
        if (cdj_alive.channels == 2) {
            out[n++] = cdj_alive.pcm[idx * 2];
            out[n++] = cdj_alive.pcm[idx * 2 + 1];
        } else {
            out[n++] = cdj_alive.pcm[idx];
            out[n++] = cdj_alive.pcm[idx];
        }
        cdj_alive.pcm_pos += step;
        avail -= 4;
        if (n == (int)ARRAY_SIZE(out)) {
            if (AUD_write(cdj_alive.voice, (uint8_t *)out, n * 2) <= 0) {
                return;
            }
            n = 0;
        }
    }
    if (n) {
        AUD_write(cdj_alive.voice, (uint8_t *)out, n * 2);
    }
}

static void cdj_audio_live_open(void)
{
    struct audsettings as = {
        .freq = 44100,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = 0,
    };
    Error *err = NULL;

    AUD_register_card("cdj2000nxs2", &cdj_alive.card, &err);
    if (err) {
        warn_report_err(err);
        warn_report("audio live: no audio backend -- add -audiodev to the "
                    "command line, e.g. -audiodev pa,id=cdj");
        cdj_alive.failed = true;
        return;
    }
    cdj_alive.voice = AUD_open_out(&cdj_alive.card, cdj_alive.voice, "cdj-dsp",
                                   NULL, cdj_audio_live_cb, &as);
    if (!cdj_alive.voice) {
        warn_report("audio live: the backend refused a 44100 Hz stereo voice");
        cdj_alive.failed = true;
        return;
    }
    AUD_set_active_out(cdj_alive.voice, 1);
    cdj_alive.opened = true;
    info_report("audio live: the model's DSP is decoding ch3 to a 44100 Hz "
                "stereo voice -- this is the track's own bitstream, decoded, "
                "not a tap on any PCM buffer the firmware has");
}

/*
 * Must be called at machine init. QEMU's AudioState only enables a backend on a
 * run-state change, so a card registered after the VM is running is never
 * pulled.
 */
void cdj_audio_live_arm(void)
{
    const char *l = getenv("CDJ_AUDIO_LIVE");
    const char *mb = getenv("CDJ_AUDIO_LIVE_MB");

    if (cdj_alive.on || !l || !atoi(l)) {
        return;
    }
    cdj_alive.cap = (size_t)(mb ? atoi(mb) : 12) * MiB;
    if (cdj_alive.cap < MiB) {
        cdj_alive.cap = MiB;
    }
    cdj_alive.cbuf = g_malloc(cdj_alive.cap);
    cdj_alive.foff = g_new(uint32_t, CDJ_ALIVE_MAX_FRAMES);
    cdj_alive.on = true;
    info_report("audio live: armed, %zu MiB of compressed backlog",
                cdj_alive.cap / MiB);
    cdj_audio_live_open();
}

/* Append-only: a cue press can send the decoder back to any earlier frame. */
static void cdj_audio_live_feed(const uint8_t *buf, uint32_t n)
{
    if (!cdj_alive.on || cdj_alive.failed) {
        return;
    }
    if (!cdj_alive.opened) {
        cdj_audio_live_open();
        if (!cdj_alive.opened) {
            return;
        }
    }
    if (cdj_alive.len + n > cdj_alive.cap
        || cdj_alive.nframes >= CDJ_ALIVE_MAX_FRAMES) {
        return;
    }
    cdj_alive.foff[cdj_alive.nframes++] = (uint32_t)cdj_alive.len;
    memcpy(cdj_alive.cbuf + cdj_alive.len, buf, n);
    cdj_alive.len += n;

}

/* One frame that survived the de-duplicator: to the file and to the decoder. */
static void cdj_audio_emit_frame(const uint8_t *f, uint32_t n)
{
    if (cdj_aout.fp) {
        fwrite(f, 1, n, cdj_aout.fp);
        fflush(cdj_aout.fp);            /* so a player can follow it live */
    }
    cdj_audio_live_feed(f, n);
}

/* memmem() is a GNU extension that mingw-w64 lacks; one implementation keeps
 * the stream cut identical on every host. */
static const uint8_t *cdj_memmem(const uint8_t *hay, size_t haylen,
                                 const uint8_t *needle, size_t nlen)
{
    if (!nlen || haylen < nlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen <= haylen; i++) {
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, nlen)) {
            return hay + i;
        }
    }
    return NULL;
}

void cdj_audio_out(unsigned ch, uint32_t sar, uint32_t bytes)
{
    uint32_t phys = A7ADDR(sar);
    g_autofree uint8_t *buf = NULL;
    uint32_t off = 0;

    if (!cdj_aout.parsed) {
        const char *e = getenv("CDJ_AUDIO_OUT");

        cdj_aout.parsed = true;
        cdj_audio_live_arm();       /* a no-op: machine init already armed it */
        if (e && *e) {
            cdj_aout.fp = fopen(e, "wb");
            if (!cdj_aout.fp) {
                warn_report("audio out: cannot write %s", e);
            } else {
                info_report("audio out: ch3's bitstream -> %s, overlapping "
                            "windows de-duplicated -- this is the track's own "
                            "MP3 data, shipped by the firmware", e);
            }
        }
        /* The overlap buffer serves both sinks. */
        if (cdj_aout.fp || cdj_alive.on) {
            cdj_aout.tail = g_malloc(CDJ_AOUT_TAIL);
        }
    }
    if ((!cdj_aout.fp && !cdj_alive.on) || ch != 3 || !bytes
        || bytes > 1 * MiB) {
        return;
    }
    /* The DSP boot program goes out on the same channel from MAIN flash
     * (0x08xxxxxx); skip it by SAR. */
    if ((phys >> 24) == 0x08) {
        return;
    }
    buf = g_malloc(bytes);
    cpu_physical_memory_read(phys, buf, bytes);
    cdj_aout.windows++;

    /*
     * A 64-byte probe can match in several places, so require the whole
     * overlap to agree. The first verified candidate is the largest overlap;
     * if none verifies the stream has jumped and the window is all new.
     */
    if (cdj_aout.tail_len >= CDJ_AOUT_PROBE && bytes >= CDJ_AOUT_PROBE) {
        uint32_t from = 0;
        bool joined = false;

        while (from + CDJ_AOUT_PROBE <= cdj_aout.tail_len) {
            const uint8_t *hit = cdj_memmem(cdj_aout.tail + from,
                                            cdj_aout.tail_len - from,
                                            buf, CDJ_AOUT_PROBE);
            uint32_t q, overlap;

            if (!hit) {
                break;
            }
            q = (uint32_t)(hit - cdj_aout.tail);
            overlap = cdj_aout.tail_len - q;
            if (overlap <= bytes
                && !memcmp(cdj_aout.tail + q, buf, overlap)) {
                off = overlap;
                joined = true;
                break;
            }
            from = q + 1;
        }
        if (!joined) {
            cdj_aout.jumps++;
        }
    }
    /* The overlap is a cheap first cut; the frame filter handles reordering. */
    if (off < bytes) {
        cdj_frames_emit(buf + off, bytes - off, cdj_audio_emit_frame);
        cdj_aout.written += bytes - off;
    }
    cdj_aout.skipped += off;

    cdj_aout.tail_len = bytes < CDJ_AOUT_TAIL ? bytes : CDJ_AOUT_TAIL;
    memcpy(cdj_aout.tail, buf + (bytes - cdj_aout.tail_len), cdj_aout.tail_len);

    if (cdj_aout.fp && cdj_aout.windows % 32 == 0) {
        info_report("audio out: %" PRIu64 " windows, %" PRIu64 " jumps; frames "
                    "%" PRIu64 " emitted, %" PRIu64 " dropped as duplicates, "
                    "%" PRIu64 " bytes resynced (%.1f s of unique audio)",
                    cdj_aout.windows, cdj_aout.jumps, cdj_frames.emitted,
                    cdj_frames.dropped, cdj_frames.unparsed,
                    (double)cdj_frames.emitted * 1152.0 / 44100.0);
    }
}

