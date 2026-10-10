/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c6xreplay -- re-run a core execution recorded with c66x_record_open.
 *
 * No SoC: every bus read returns the recorded value, every bus write is checked
 * against the recorded one, interrupts and host RAM stores are applied at the
 * cycle they happened, idle skips are repeated. So one in-machine run (boot,
 * load, MP3 decode, playback) becomes a deterministic offline workload:
 *   - a benchmark of the core on its real program (host time excludes loading);
 *   - an oracle: the recorded state hashes (every 2^22 cycles) and write values
 *     must match, and the final RAM/state hash printed here must be identical
 *     between two core builds.
 *
 * usage: c6xreplay <record> [max_cycles]
 *
 * REPLAY_PCM=<file>:<addr>:<len> appends, at every recorded state hash, the
 * cycle (u64) and <len> bytes of guest RAM from <addr>: a sampled view of an
 * output buffer, e.g. the McBSP0 ping-pong, whose words the audio model plays.
 *
 * REPLAY_WINDOW=<cycles> prints one line per window of that many core cycles
 * (executed + idle-skipped): the executed cycles and host seconds in it. The
 * whole-run rate mixes boot, load and playback; a window isolates one phase.
 *
 * REPLAY_RETURN_AT=<pc> makes the function at <pc> return at once, as the
 * CDJ-2000's hook on its control register 9 routine does in the machine; without
 * it the replay runs the routine's instructions and stops on one the core lacks.
 *
 * REPLAY_IDLE arms the core's busy-wait skip the way the machine does: 1 is
 * the NXS2's (head 0x80076F00, stack 0x008AC668..0x008BC670), or give the
 * head, stack range and bus-read flag as <head>:<stack_lo>:<stack_hi>[:<reads>].
 * The recording machine ran with it, so the core stops exactly at the
 * recorded skips, and the replay then pays the same idle-tracking cost the
 * machine pays, which it otherwise does not.
 */
#include "c66x.h"

#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct rec {
    const uint8_t *p;       /* the record's type byte */
    uint64_t cycle;
    uint8_t type;
    uint8_t inline_;        /* consumed from inside a step */
} rec;

static rec *R;
static size_t NR, I;
static c66x_core *C;
static uint8_t *host[8];
static uint32_t host_size[8];
static uint64_t n_read, n_write, bad_write, n_hash, bad_hash, n_irq, n_store, n_skip, skipped;
static uint64_t write_hash = 1469598103934665603ull;
static int stop_now;
static uint64_t n_steps, step_hist[64];     /* c66x_step calls by log2 budget */
static unsigned long idle_head = 0x80076F00, idle_lo = 0x008AC668, idle_hi = 0x008BC670;
static int replay_idle, idle_reads;
static uint32_t return_at;

/* The B3 register of the C6x calling convention is register 35. */
static int return_hook(c66x_core *c, void *opaque)
{
    c66x_set_pc(c, c66x_get_reg(c, 35));
    return 0;
}

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static size_t rec_len(const uint8_t *p)
{
    const uint8_t *b = p + 9;
    switch (p[0]) {
    case 'R': case 'W': return 9 + 9;
    case 'I': return 9 + 2;
    case 'D': return 9 + 8 + u32(b + 4);
    case 'S': return 9 + 8;
    case 'N': return 9;
    case 'M': return 9 + 12;
    case 'P': return 9 + 8 + 4096;
    case 'Z': return 9 + 4;
    case 'H': return 9 + 12;
    default:  return 0;
    }
}

static void apply_inline_followers(void);
static uint8_t *ram_at(uint32_t a, uint32_t len);

static FILE *pcm_out;
static uint32_t pcm_addr, pcm_len;

static void pcm_open(void)
{
    const char *spec = getenv("REPLAY_PCM");
    char path[512], *c2, *c1;
    /* Split from the right: a Windows path has a colon of its own. */
    if (!spec || snprintf(path, sizeof path, "%s", spec) >= (int)sizeof path ||
        !(c2 = strrchr(path, ':')) || (*c2 = 0, !(c1 = strrchr(path, ':'))))
        return;
    *c1 = 0;
    pcm_addr = strtoul(c1 + 1, NULL, 0);
    pcm_len = strtoul(c2 + 1, NULL, 0);
    pcm_out = fopen(path, "wb");
    if (!pcm_out)
        fprintf(stderr, "REPLAY_PCM: cannot open %s\n", path);
}

static void pcm_sample(uint64_t cycle)
{
    const uint8_t *d = pcm_out ? ram_at(pcm_addr, pcm_len) : NULL;
    if (!d)
        return;
    fwrite(&cycle, sizeof cycle, 1, pcm_out);
    fwrite(d, 1, pcm_len, pcm_out);
}

static void die_at(const char *what)
{
    const rec *r = I < NR ? &R[I] : NULL;
    fprintf(stderr, "DIVERGED: %s at core cycle %llu pc 0x%08x; next record '%c' cycle %llu\n",
            what, (unsigned long long)c66x_get_cycle(C), c66x_get_exec_pc(C),
            r ? r->type : '-', r ? (unsigned long long)r->cycle : 0ull);
    stop_now = 1;
}

static uint32_t bus_read(void *opaque, uint32_t a, unsigned n)
{
    if (stop_now)
        return 0;
    if (I >= NR || R[I].type != 'R' || u32(R[I].p + 9) != a || R[I].p[13] != n) {
        char m[96];
        snprintf(m, sizeof m, "unexpected read 0x%08x/%u", a, n);
        die_at(m);
        return 0;
    }
    uint32_t v = u32(R[I].p + 14);
    I++;
    n_read++;
    apply_inline_followers();
    return v;
}

static void bus_write(void *opaque, uint32_t a, uint32_t v, unsigned n)
{
    if (stop_now)
        return;
    if (I >= NR || R[I].type != 'W' || u32(R[I].p + 9) != a || R[I].p[13] != n) {
        char m[96];
        snprintf(m, sizeof m, "unexpected write 0x%08x/%u = 0x%08x", a, n, v);
        die_at(m);
        return;
    }
    if (u32(R[I].p + 14) != v) {
        if (bad_write++ < 10)
            fprintf(stderr, "write value differs: 0x%08x/%u = 0x%08x, recorded 0x%08x (cycle %llu)\n",
                    a, n, v, u32(R[I].p + 14), (unsigned long long)R[I].cycle);
    }
    write_hash = (write_hash ^ a) * 1099511628211ull;
    write_hash = (write_hash ^ v) * 1099511628211ull;
    I++;
    n_write++;
    apply_inline_followers();
}

static void apply_store(const rec *r)
{
    uint32_t a = u32(r->p + 9), len = u32(r->p + 13);
    uint8_t *d = ram_at(a, len);
    if (d)
        memcpy(d, r->p + 17, len);
    c66x_invalidate(C, a, len);
    n_store++;
}

static void apply_inline_followers(void)
{
    while (I < NR && R[I].inline_ && (R[I].type == 'I' || R[I].type == 'D')) {
        if (R[I].type == 'I') {
            c66x_set_irq(C, R[I].p[9], R[I].p[10]);
            n_irq++;
        } else {
            apply_store(&R[I]);
        }
        I++;
    }
}

/* Maps as recorded, for the replayer's own RAM lookups. */
static struct { uint32_t base, size, id; } map[8];
static unsigned nmap;

static uint8_t *ram_at(uint32_t a, uint32_t len)
{
    for (unsigned i = 0; i < nmap; i++)
        if (a - map[i].base < map[i].size && a - map[i].base + (uint64_t)len <= map[i].size)
            return host[map[i].id] + (a - map[i].base);
    return NULL;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: c6xreplay <record> [max_cycles]\n");
        return 2;
    }
    uint64_t max_cycles = argc > 2 ? strtoull(argv[2], NULL, 0) : ~0ull;
    /* GLib rather than mmap, so this also builds on Windows. */
    GError *maperr = NULL;
    GMappedFile *mf = g_mapped_file_new(argv[1], FALSE, &maperr);
    if (!mf) {
        fprintf(stderr, "%s: %s\n", argv[1], maperr->message);
        g_error_free(maperr);
        return 2;
    }
    const uint8_t *m = (const uint8_t *)g_mapped_file_get_contents(mf);
    size_t msize = g_mapped_file_get_length(mf);

    /* Index, and mark what a step consumes: reads, writes, and interrupts or
     * stores in the same cycle right after one (caused synchronously). */
    size_t cap = 1 << 20;
    R = malloc(cap * sizeof *R);
    for (const uint8_t *p = m; p < m + msize;) {
        /* A recorder killed mid-write leaves a partial record: its length
         * field may itself lie past the end of the file. */
        size_t left = (size_t)(m + msize - p);
        size_t l = left < 9 || (p[0] == 'D' && left < 17) ? 0 : rec_len(p);
        if (!l || l > left) {
            fprintf(stderr, "bad record at offset %zu\n", (size_t)(p - m));
            break;
        }
        if (NR == cap)
            R = realloc(R, (cap *= 2) * sizeof *R);
        rec *r = &R[NR];
        r->p = p;
        r->type = p[0];
        r->cycle = u64(p + 1);
        r->inline_ = r->type == 'R' || r->type == 'W';
        if ((r->type == 'I' || r->type == 'D') && NR && R[NR - 1].inline_ && R[NR - 1].cycle == r->cycle)
            r->inline_ = 1;
        NR++;
        p += l;
    }
    fprintf(stderr, "%zu records, %.1f MB\n", NR, msize / 1e6);

    c66x_bus bus = { NULL, bus_read, bus_write };
    pcm_open();
    double host_s = 0, t0 = 0;
    uint64_t cycles = 0, base_cycles = 0;
    /* The next record that is not consumed from inside a step, per index. */
    size_t *nb = malloc((NR + 1) * sizeof *nb);
    nb[NR] = NR;
    for (size_t k = NR; k-- > 0;)
        nb[k] = R[k].inline_ ? nb[k + 1] : k;

    const char *ra = getenv("REPLAY_RETURN_AT");
    return_at = ra ? strtoul(ra, NULL, 0) : 0;
    const char *ie = getenv("REPLAY_IDLE");
    replay_idle = ie && strcmp(ie, "0");
    if (replay_idle && strcmp(ie, "1"))
        sscanf(ie, "%lx:%lx:%lx:%d", &idle_head, &idle_lo, &idle_hi, &idle_reads);
    uint64_t idle_stuck = 0;
    const char *we = getenv("REPLAY_WINDOW");
    uint64_t win = we ? strtoull(we, NULL, 0) : 0, win_end = win, win_exec0 = 0;
    double win_host0 = 0;

    while (I < NR && !stop_now) {
        size_t b = nb[I];
        if (win && C && base_cycles + c66x_get_cycle(C) >= win_end) {
            uint64_t tot = base_cycles + c66x_get_cycle(C), ex = tot - skipped;
            double h = host_s - win_host0;
            fprintf(stderr, "window %llu: cycles %llu executed %llu host %.3f s %.1f M/s\n",
                    (unsigned long long)(win_end / win - 1), (unsigned long long)tot,
                    (unsigned long long)(ex - win_exec0), h,
                    h > 0 ? (ex - win_exec0) / h / 1e6 : 0);
            win_exec0 = ex;
            win_host0 = host_s;
            while (win_end <= tot)
                win_end += win;
        }
        if (C && (b == NR || (R[b].type != 'N' && R[b].type != 'M' && R[b].type != 'P'))) {
            uint64_t cur = c66x_get_cycle(C);
            /* Past the last boundary, run until the trailing reads are done. */
            uint64_t target = b < NR ? R[b].cycle : R[NR - 1].cycle + 1;
            if (cur < target) {
                if (base_cycles + cur >= max_cycles)
                    break;
                uint64_t n;
                t0 = now_s();
                c66x_stop st = c66x_step(C, target - cur, &n);
                /* With REPLAY_IDLE the core stops where the recorded run did,
                 * right at its skip; a stop anywhere else never makes progress. */
                if (st == C66X_STOP_IDLE && !n && ++idle_stuck > 1000) {
                    die_at("an idle stop the recorded run did not make");
                    break;
                }
                if (n)
                    idle_stuck = 0;
                n_steps++;
                step_hist[63 - __builtin_clzll((target - cur) | 1)]++;
                host_s += now_s() - t0;
                continue;
            }
            if (I < b) {
                die_at("a read/write the core did not perform");
                break;
            }
            if (b == NR)
                break;
            if (cur > target) {
                die_at("the core ran past a recorded event");
                break;
            }
        } else if (I < b) {
            die_at("a read/write before the core exists");
            break;
        }
        rec *r = &R[b];
        I = b;
        switch (r->type) {
        case 'N':
            if (C) {
                base_cycles += c66x_get_cycle(C);
                c66x_free(C);
            }
            C = c66x_new(&bus);
            nmap = 0;
            if (replay_idle)
                c66x_set_idle_loop(C, idle_head, idle_lo, idle_hi, idle_reads);
            if (return_at)
                c66x_hook_pc(C, return_at, return_hook, NULL);
            break;
        case 'M': {
            uint32_t base = u32(r->p + 9), size = u32(r->p + 13), id = u32(r->p + 17);
            if (!host[id]) {
                host[id] = calloc(1, size);
                host_size[id] = size;
            }
            map[nmap++] = (typeof(map[0])){ base, size, id };
            c66x_map_ram(C, base, size, host[id]);
            break;
        }
        case 'P':
            memcpy(host[u32(r->p + 9)] + u32(r->p + 13), r->p + 17, 4096);
            break;
        case 'Z':
            c66x_reset(C, u32(r->p + 9));
            break;
        case 'I':
            c66x_set_irq(C, r->p[9], r->p[10]);
            n_irq++;
            break;
        case 'D':
            apply_store(r);
            break;
        case 'S':
            c66x_skip_cycles(C, u64(r->p + 9));
            skipped += u64(r->p + 9);
            n_skip++;
            break;
        case 'H':
            n_hash++;
            pcm_sample(base_cycles + c66x_get_cycle(C));
            if (c66x_get_pc(C) != u32(r->p + 9) || c66x_state_hash(C) != u64(r->p + 13)) {
                if (bad_hash++ < 5)
                    fprintf(stderr, "state hash differs at cycle %llu: pc 0x%08x (recorded 0x%08x)\n",
                            (unsigned long long)r->cycle, c66x_get_pc(C), u32(r->p + 9));
                if (bad_hash == 1)
                    stop_now = getenv("REPLAY_KEEP_GOING") == NULL;
            }
            break;
        }
        I++;
    }
    if (C)
        cycles = base_cycles + c66x_get_cycle(C);

    /* REPLAY_DUMP=<dir>: the final RAM, one file per mapping named by its base,
     * for disassembling what the profile names. */
    const char *dump = getenv("REPLAY_DUMP");
    for (unsigned i = 0; dump && i < nmap; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/ram_%08x.bin", dump, map[i].base);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(host[map[i].id], 1, map[i].size, f);
            fclose(f);
        }
    }

    uint64_t ram_hash = 1469598103934665603ull;
    for (unsigned i = 0; i < 8; i++)
        for (uint32_t k = 0; host[i] && k < host_size[i]; k++)
            ram_hash = (ram_hash ^ host[i][k]) * 1099511628211ull;
    printf("replayed %zu of %zu records, %llu cycles (%llu executed, %llu idle-skipped) in %.2f s "
           "host: %.1f M executed cycles/s\n", I, NR, (unsigned long long)cycles,
           (unsigned long long)(cycles - skipped), (unsigned long long)skipped, host_s,
           host_s > 0 ? (cycles - skipped) / host_s / 1e6 : 0);
    printf("reads %llu writes %llu (value mismatches %llu) irqs %llu stores %llu skips %llu\n",
           (unsigned long long)n_read, (unsigned long long)n_write, (unsigned long long)bad_write,
           (unsigned long long)n_irq, (unsigned long long)n_store, (unsigned long long)n_skip);
    printf("state hashes %llu checked, %llu differ\n", (unsigned long long)n_hash,
           (unsigned long long)bad_hash);
    printf("final: state 0x%016llx writes 0x%016llx ram 0x%016llx\n",
           C ? (unsigned long long)c66x_state_hash(C) : 0ull, (unsigned long long)write_hash,
           (unsigned long long)ram_hash);
    if (getenv("REPLAY_STEPS")) {
        printf("steps %llu, by log2 budget:", (unsigned long long)n_steps);
        for (unsigned i = 0; i < 64; i++)
            if (step_hist[i])
                printf(" %u:%llu", i, (unsigned long long)step_hist[i]);
        printf("\n");
    }
    printf("verdict: %s\n", stop_now || bad_hash || bad_write ? "DIVERGED" : "EXACT");
    fflush(stdout);
    c66x_free(C);
    return stop_now || bad_hash || bad_write;
}
