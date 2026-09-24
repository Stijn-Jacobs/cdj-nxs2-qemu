/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "peer_script.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum step_kind { STEP_REQ, STEP_SHIP, STEP_WAIT, STEP_MARK };
/* Exchanges MAIN waits for a tagged answer before moving on. */
enum { REPLY_TIMEOUT = 64, MAX_WORDS = 32 };

typedef struct step {
    enum step_kind kind;
    uint16_t hw[PEER_FRAME_HW];
    unsigned nhw, type, tag, nargs;
    uint32_t arg0;
    int      expect_words;        /* -1 unknown */
    const uint8_t *buf;
    size_t   len;
    uint64_t ns;
    char     label[48];
    /* outcome */
    int      answered, timed_out, kind_seen, len2_seen;
    uint32_t words[MAX_WORDS];
    unsigned nwords;
    uint64_t sent_ns, answered_ns;
} step;

struct peer_script {
    peer_script_io io;
    peer_main *peer;
    step      *steps;
    unsigned   nsteps, cap, cur;
    int        started, waiting;
    unsigned   wait_exchanges;
    uint64_t   wait_until;
    unsigned   next_tag;
    unsigned   trace;
    /* what the DSP publishes */
    uint64_t   kind_count[64], armed_frames, replies_untagged;
    uint64_t   first_ns, last_ns;
    uint64_t   gap_min, gap_max, gap_sum, gaps;
    uint16_t   status1[32], status1_first[32];
    uint32_t   status1_changes[32];
    int        have_status1;
    uint64_t   ships, shipped_bytes;
};

static uint64_t now(const peer_script *s)
{
    return s->io.now_ns ? s->io.now_ns(s->io.opaque) : 0;
}

static step *push(peer_script *s)
{
    if (s->nsteps == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->steps = realloc(s->steps, s->cap * sizeof *s->steps);
    }
    step *t = &s->steps[s->nsteps++];
    memset(t, 0, sizeof *t);
    t->expect_words = -1;
    return t;
}

/* Words the DSP handler answers with; -1 where unknown. The writers answer one
 * status word. */
static int expected_words(unsigned type, const uint32_t *args, unsigned nargs)
{
    switch (type) {
    case 1: case 7: return 1;
    case 2: case 3: case 10: return 1;
    case 6: case 12: return -1;
    case 4: return nargs;
    case 5: return nargs >= 2 ? (int)args[1] : -1;
    default: return -1;
    }
}

static void describe_frame(step *t)
{
    unsigned base = (t->hw[1] & 0xFF) ? 2 + PEER_IDBLOCK_HW : 2;
    unsigned len = t->hw[1] >> 8;
    uint32_t args[PEER_MAX_ARGS];
    t->type = t->hw[base];
    t->tag = t->hw[base + 1];
    t->nargs = len >= 2 ? (len - 2) / 2 : 0;
    if (t->nargs > PEER_MAX_ARGS)
        t->nargs = PEER_MAX_ARGS;
    for (unsigned i = 0; i < t->nargs; i++)
        args[i] = t->hw[base + 2 + 2 * i] | (uint32_t)t->hw[base + 3 + 2 * i] << 16;
    t->arg0 = t->nargs ? args[0] : 0;
    /* A write list's argument pairs are (index, value): the index is arg0. */
    t->expect_words = expected_words(t->type, args, t->nargs);
    if (t->type == 4)
        t->expect_words = t->nargs;
}

void peer_script_request(peer_script *s, unsigned type, const uint32_t *args, unsigned nargs)
{
    step *t = push(s);
    t->kind = STEP_REQ;
    t->nhw = peer_build_request(t->hw, type, 0x0101 + s->next_tag++, args, nargs, NULL);
    describe_frame(t);
}

void peer_script_ship(peer_script *s, const uint8_t *buf, size_t len)
{
    step *t = push(s);
    t->kind = STEP_SHIP;
    t->buf = buf;
    t->len = len;
}

void peer_script_wait_ns(peer_script *s, uint64_t ns)
{
    step *t = push(s);
    t->kind = STEP_WAIT;
    t->ns = ns;
}

void peer_script_mark(peer_script *s, const char *label)
{
    step *t = push(s);
    t->kind = STEP_MARK;
    snprintf(t->label, sizeof t->label, "%s", label);
}

int peer_script_corpus(peer_script *s, const char *path, unsigned run, unsigned upto)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned seq, r, nhw;
        int used;
        if (line[0] != 'F' || sscanf(line, "F %u %u %u%n", &seq, &r, &nhw, &used) != 3)
            continue;
        if (r != run || seq > upto || nhw > PEER_FRAME_HW)
            continue;
        step *t = push(s);
        t->kind = STEP_REQ;
        char *p = line + used;
        for (unsigned i = 0; i < nhw; i++)
            t->hw[i] = (uint16_t)strtoul(p, &p, 16);
        t->nhw = nhw;
        describe_frame(t);
        /* keep later scripted tags clear of the DSP's duplicate-tag history */
        if (t->tag >= 0x0101 + s->next_tag)
            s->next_tag = t->tag - 0x0101 + 1;
        n++;
    }
    fclose(f);
    return n;
}

int peer_script_mainlog(peer_script *s, const char *path, unsigned upto)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[8192];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "MAIN frame ");
        unsigned seq, nhw;
        if (!p || sscanf(p, "MAIN frame %u (%u hw)", &seq, &nhw) != 2 || seq > upto)
            continue;
        p = strstr(p, "ms: ");
        if (!p)
            continue;
        p += 4;
        step *t = push(s);
        t->kind = STEP_REQ;
        /* the log carries the frame driver's trailing pad word; stop at 0xAACC */
        unsigned i = 0;
        for (; i < nhw && i < PEER_FRAME_HW; i++) {
            char *e;
            unsigned long v = strtoul(p, &e, 16);
            if (e == p)
                break;
            t->hw[i] = (uint16_t)v;
            p = e;
            if (v == 0xAACC && i > 1) {
                i++;
                break;
            }
        }
        t->nhw = i;
        describe_frame(t);
        if (t->tag >= 0x0101 + s->next_tag)
            s->next_tag = t->tag - 0x0101 + 1;
        n++;
    }
    fclose(f);
    return n;
}

void peer_script_start(peer_script *s)
{
    s->started = 1;
}

int peer_script_done(const peer_script *s)
{
    return s->started && s->cur >= s->nsteps;
}

static void advance(peer_script *s)
{
    s->cur++;
    s->waiting = 0;
    s->wait_exchanges = 0;
    s->wait_until = 0;
}

/* Run steps that need no DSP answer, and queue the next request. */
static void pump(peer_script *s, const peer_dsp_frame *f)
{
    while (s->started && s->cur < s->nsteps) {
        step *t = &s->steps[s->cur];
        if (t->kind == STEP_MARK) {
            printf("script: --- %s at %.3f s\n", t->label, now(s) / 1e9);
            advance(s);
        } else if (t->kind == STEP_WAIT) {
            if (!s->wait_until)
                s->wait_until = now(s) + t->ns;
            if (now(s) < s->wait_until)
                return;
            advance(s);
        } else if (t->kind == STEP_SHIP) {
            /* MAIN ships only after the DSP says a receive is armed. */
            if (!f || !(f->flags & 0x80))
                return;
            s->io.upp_ship(s->io.opaque, t->buf, t->len);
            s->ships++;
            s->shipped_bytes += t->len;
            advance(s);
        } else {
            if (s->waiting)
                return;
            if (!peer_main_queue(s->peer, t->hw, t->nhw))
                return;
            t->sent_ns = now(s);
            s->waiting = 1;
            return;
        }
    }
}

static void take_reply(peer_script *s, const peer_dsp_frame *f)
{
    if (!f->len2)
        return;
    if (!f->reply) {
        s->replies_untagged++;
        return;
    }
    uint16_t tag = f->reply[1];
    if (!(s->waiting && s->cur < s->nsteps && s->steps[s->cur].kind == STEP_REQ &&
          s->steps[s->cur].tag == tag)) {
        s->replies_untagged++;
        return;
    }
    step *t = &s->steps[s->cur];
    t->answered = 1;
    t->answered_ns = now(s);
    t->kind_seen = f->kind;
    t->len2_seen = f->len2;
    t->nwords = 0;
    for (unsigned i = 2; i + 1 < f->len2 && t->nwords < MAX_WORDS; i += 2)
        t->words[t->nwords++] = f->reply[i] | (uint32_t)f->reply[i + 1] << 16;
    advance(s);
}

static void on_frame(void *opaque, const peer_dsp_frame *f)
{
    peer_script *s = opaque;
    uint64_t t = now(s);
    if (s->trace && f->hw[1]) {       /* empty frames carry nothing to read */
        s->trace--;
        printf("dsp frame @%.4f s kind %u flags %02x len2 %u %s:", t / 1e9, f->kind, f->flags,
               f->len2, f->valid ? "ok" : "BAD");
        for (unsigned i = 0; i < PEER_FRAME_HW; i++)
            printf("%s%04x", i % 16 ? " " : "\n   ", f->hw[i]);
        printf("\n");
    }
    if (s->last_ns) {
        uint64_t g = t - s->last_ns;
        if (!s->gaps || g < s->gap_min) s->gap_min = g;
        if (g > s->gap_max) s->gap_max = g;
        s->gap_sum += g;
        s->gaps++;
    } else {
        s->first_ns = t;
    }
    s->last_ns = t;
    if (!f->valid)
        return;
    s->kind_count[f->kind]++;
    if (f->flags & 0x80)
        s->armed_frames++;
    if (f->kind == 1) {
        for (unsigned i = 0; i < 32; i++) {
            if (!s->have_status1)
                s->status1_first[i] = f->status[i];
            else if (s->status1[i] != f->status[i])
                s->status1_changes[i]++;
            s->status1[i] = f->status[i];
        }
        s->have_status1 = 1;
    }
    take_reply(s, f);
    if (s->waiting && ++s->wait_exchanges > REPLY_TIMEOUT) {
        s->steps[s->cur].timed_out = 1;
        advance(s);
    }
    pump(s, f);
}

peer_script *peer_script_new(const peer_script_io *io)
{
    peer_script *s = calloc(1, sizeof *s);
    s->io = *io;
    s->peer = peer_main_new(on_frame, s);
    return s;
}

void peer_script_free(peer_script *s)
{
    peer_main_free(s->peer);
    free(s->steps);
    free(s);
}

uint16_t peer_script_spi(peer_script *s, uint16_t dsp_word)
{
    return peer_main_xfer(s->peer, dsp_word);
}

void peer_script_trace_frames(peer_script *s, unsigned n)
{
    s->trace = n;
}

const uint32_t *peer_script_last_reply(const peer_script *s, unsigned type, uint32_t arg0,
                                       unsigned *nwords)
{
    for (unsigned i = s->nsteps; i-- > 0;) {
        const step *t = &s->steps[i];
        if (t->kind == STEP_REQ && t->answered && t->type == type && t->arg0 == arg0) {
            *nwords = t->nwords;
            return t->words;
        }
    }
    return NULL;
}

void peer_script_report(const peer_script *s, FILE *f)
{
    fprintf(f, "\nlink: %" PRIu64 " exchanges, %" PRIu64 " without header, %" PRIu64
               " with a trailer MAIN would reject",
            peer_main_exchanges(s->peer), peer_main_bad_frames(s->peer),
            peer_main_bad_trailers(s->peer));
    if (s->gaps)
        fprintf(f, ", cadence %.3f ms mean (min %.3f, max %.3f), first at %.3f s\n",
                s->gap_sum / (double)s->gaps / 1e6, s->gap_min / 1e6, s->gap_max / 1e6,
                s->first_ns / 1e9);
    else
        fprintf(f, "\n");
    fprintf(f, "frames by kind:");
    for (unsigned k = 0; k < 64; k++)
        if (s->kind_count[k])
            fprintf(f, " %u:x%" PRIu64, k, s->kind_count[k]);
    fprintf(f, "  (uPP-armed flag on %" PRIu64 ", untagged replies %" PRIu64 ")\n",
            s->armed_frames, s->replies_untagged);
    fprintf(f, "uPP ships %" PRIu64 " (%" PRIu64 " B)\n", s->ships, s->shipped_bytes);
    if (s->have_status1) {
        fprintf(f, "kind-1 status vector (first -> last, changes):\n");
        for (unsigned i = 0; i < 32; i++)
            fprintf(f, "  p[%2u] %04x -> %04x  x%u%s", i, s->status1_first[i], s->status1[i],
                    s->status1_changes[i], i % 4 == 3 ? "\n" : "");
    }
    unsigned ok = 0, bad_shape = 0, timeouts = 0, reqs = 0;
    for (unsigned i = 0; i < s->nsteps; i++) {
        const step *t = &s->steps[i];
        if (t->kind != STEP_REQ)
            continue;
        reqs++;
        if (t->timed_out) timeouts++;
        else if (t->answered) {
            if (t->expect_words < 0 || (int)t->nwords == t->expect_words) ok++;
            else bad_shape++;
        }
    }
    fprintf(f, "requests: %u queued, %u answered with the expected word count, "
               "%u with another count, %u timed out, %u not reached\n",
            reqs, ok, bad_shape, timeouts, reqs - ok - bad_shape - timeouts);
    unsigned shown = 0;
    for (unsigned i = 0; i < s->nsteps && shown < 400; i++) {
        const step *t = &s->steps[i];
        if (t->kind != STEP_REQ || !(t->answered || t->timed_out))
            continue;
        shown++;
        fprintf(f, "  #%-4u type %2u tag %04x arg0 %08x  ", i, t->type, t->tag, t->arg0);
        if (t->timed_out) {
            fprintf(f, "TIMEOUT\n");
            continue;
        }
        fprintf(f, "kind %u len2 %2d  +%.2f ms  %u/%d words%s", t->kind_seen, t->len2_seen,
                (t->answered_ns - t->sent_ns) / 1e6, t->nwords, t->expect_words,
                t->expect_words >= 0 && (int)t->nwords != t->expect_words ? " MISMATCH" : "");
        for (unsigned k = 0; k < t->nwords && k < 20; k++)
            fprintf(f, " %08x", t->words[k]);
        fprintf(f, "\n");
    }
}
