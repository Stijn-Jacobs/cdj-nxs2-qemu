/*
 * Pioneer CDJ-2000: two or more players on one Pro DJ Link segment, in one
 * guest time.
 *
 * Under --cosim every player keeps a guest time of its own (MAIN under
 * -icount, see cdj2000_cosim.c), and each runs at whatever speed its host
 * threads manage, about a third of real time on this project's machines.  Two
 * such players on a plain hub drift apart: one's beat packets reach the other
 * early or late by however far their clocks have wandered, and a second of one
 * deck's time is not a second of the other's.  Presence, player numbers and
 * status survive that; beat phase, MASTER and SYNC do not.
 *
 * With CDJ_NETSIM=HOST:PORT or unix:PATH the player's Ethernet goes to a
 * synchronising hub (tools.cdj_main.link_hub --sync) instead of a QEMU
 * netdev, and the hub keeps
 * every player on one guest timeline, conservatively, exactly as the MAIN/GUI
 * link does:
 *
 *  - the segment has a latency, CDJ_NETSIM_QUANTUM_US (1000 us): a frame sent
 *    at guest time t reaches the other players at t + quantum, never earlier;
 *  - no player runs past the hub's promise + quantum, where the hub promises
 *    the least of the other players' own promises (and of its replay's next
 *    frame), so nothing can arrive in a player's past;
 *  - each player reports its time as it goes (every half quantum), with every
 *    frame, and whenever it has to wait; a sleeping player promises up to its
 *    next timer, so idle stretches pass in one go.
 *
 * Guest time is MAIN's virtual clock.  Every player starts paused and is
 * started by its own GUI, so all of them begin at guest time 0 however far
 * apart they were launched: the hub holds the first ones at one quantum until
 * the rest have joined.
 *
 * CDJ_NETSIM_CLOCK=wall opts into a local, bounded host-time clock instead
 * of -icount. CDJ_NETSIM_CLOCK_FILE names a fresh mmap file shared by the
 * decks on ONE host. Each clock is min(host_monotonic - shared_epoch,
 * next_barrier). The barrier remains no later than peer_promise + quantum,
 * or the next queued frame. Wall-mode promises are always the current time,
 * including when idle: thus clocks differ by at most one quantum, and no
 * frame can arrive in a player's past. Delayed timer dispatch cannot advance
 * the clock past its barrier. TCG runs normally inside each interval; this
 * preserves network causality, not instruction-count reproducibility.
 * This live socket segment/host epoch is not a migration or replay clock.
 *
 * The wire is cdj2000_cosim.c's: a 20-byte header -- "CDJC", the type, three
 * zero bytes, the guest time in ns (u64), the payload length (u32), little
 * endian -- and the payload.  Types: TIME (1; payload the promise, u64: the
 * sender sends nothing stamped before it), FRAME (5; an Ethernet frame sent at
 * the stamped time) and HELLO (4).
 *
 * Copyright (C) 2026 RaftTone
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"

#include "cdj_netsim.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/runstate.h"

#ifdef _WIN32
/* The wire is BSD sockets and poll(), like cdj2000_cosim.c's. */
bool cdj_netsim_active(void)
{
    return false;
}

void cdj_netsim_send(const uint8_t *frame, unsigned len)
{
}

bool cdj_netsim_init(CdjNetsimFrameFn sink, void *opaque)
{
    const char *spec = getenv("CDJ_NETSIM");

    if (spec && *spec) {
        error_report("cdj2000-netsim: CDJ_NETSIM is not supported on Windows");
        exit(1);
    }
    return false;
}
#else

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/mman.h>

enum { NET_TIME = 1, NET_HELLO = 4, NET_FRAME = 5 };
#define NET_HEADER          20
#define NET_PAYLOAD_MAX     2048
#define NET_WAIT_NOTE_S     10

typedef struct NetDue {
    int64_t due;                        /* guest ns it reaches this player */
    unsigned len;
    uint8_t data[];
} NetDue;

static struct {
    bool active;
    int fd;
    int64_t quantum;
    int64_t peer;                       /* the latest stamp from the hub */
    int64_t peer_promise;               /* it sends nothing stamped before */
    int64_t reported, promised;
    QEMUTimer *timer;
    GQueue due;
    CdjNetsimFrameFn sink;
    void *opaque;
    uint8_t in[NET_HEADER + NET_PAYLOAD_MAX + 65536];
    size_t in_len;
    uint64_t steps, waits, sent, received;
    int64_t wait_wall_ns, census_every, census_next;
    bool wall_clock;
    int64_t ceiling;
    int64_t *epoch;
    FILE *trace;
    int64_t max_late;
    uint64_t late, promise_violations;
    bool running;
    int64_t frozen;
} ns = { .fd = -1 };

/* The host epoch is common to this local segment, not the launch time of
 * either VM. The clock cannot cross the next conservative event boundary,
 * even if a timer or the main loop is dispatched late. TCG is unrestricted
 * inside the boundary. No socket work or clock recursion on this hot path. */
static int64_t netsim_wall_clock(void)
{
    int64_t epoch = qatomic_read(ns.epoch);
    int64_t ceiling = qatomic_read(&ns.ceiling);

    if (!qatomic_read(&ns.running)) {
        return qatomic_read(&ns.frozen);
    }
    return epoch ? MIN(MAX(get_clock() - epoch, 0), ceiling) : 0;
}

static void netsim_vm_state(void *opaque, bool running, RunState state)
{
    if (!running) {
        qatomic_set(&ns.frozen, netsim_wall_clock());
    }
    qatomic_set(&ns.running, running);
}

static void netsim_wall_init(const char *path)
{
    int fd;
    struct stat st;

    if (!path || !*path) {
        error_report("cdj2000-netsim: wall clock needs CDJ_NETSIM_CLOCK_FILE "
                     "(one fresh file shared by the local decks)");
        exit(1);
    }
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || flock(fd, LOCK_EX) < 0 || fstat(fd, &st) < 0 ||
        (st.st_size != 0 && st.st_size != sizeof(int64_t)) ||
        ftruncate(fd, sizeof(int64_t)) < 0) {
        error_report("cdj2000-netsim: cannot initialize clock file %s", path);
        exit(1);
    }
    ns.epoch = mmap(NULL, sizeof(int64_t), PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    if (ns.epoch == MAP_FAILED) {
        error_report("cdj2000-netsim: cannot map clock file %s", path);
        exit(1);
    }
    flock(fd, LOCK_UN);
    close(fd);
    qatomic_cmpxchg(ns.epoch, 0, get_clock());
    ns.wall_clock = true;
    ns.ceiling = ns.quantum / 2;
    /* The initial ceiling holds early decks until the hub admits the census. */
    board_virtual_clock = netsim_wall_clock;
    qemu_add_vm_change_state_handler(netsim_vm_state, NULL);
}

bool cdj_netsim_active(void)
{
    return ns.active;
}

static int64_t netsim_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void netsim_lost(const char *why)
{
    NetDue *due;

    if (!ns.active) {
        return;
    }
    error_report("cdj2000-netsim: %s at guest t=%.6f; the player is off the "
                 "segment", why, netsim_now() / 1e9);
    if (ns.wall_clock) {
        /* Continuing with an unbounded clock would invalidate Link timing. */
        exit(1);
    }
    ns.active = false;
    timer_del(ns.timer);
    while ((due = g_queue_pop_head(&ns.due))) {
        g_free(due);
    }
    if (ns.fd >= 0) {
        close(ns.fd);
        ns.fd = -1;
    }
}

static void netsim_write(const uint8_t *data, size_t len)
{
    while (len && ns.active) {
        ssize_t put = send(ns.fd, data, len, 0);

        if (put > 0) {
            data += put;
            len -= put;
        } else if (put < 0 && errno == EINTR) {
            continue;
        } else {
            netsim_lost(put < 0 ? strerror(errno) : "the hub closed the link");
        }
    }
}

static void netsim_send_msg(unsigned type, int64_t t, const uint8_t *payload,
                            unsigned len)
{
    uint8_t header[NET_HEADER] = { 'C', 'D', 'J', 'C', type };

    if (!ns.active) {
        return;
    }
    stq_le_p(header + 8, t);
    stl_le_p(header + 16, len);
    netsim_write(header, sizeof(header));
    if (len) {
        netsim_write(payload, len);
    }
    if (t > ns.reported) {
        ns.reported = t;
    }
}

static void netsim_parse(void)
{
    size_t at = 0;

    while (ns.active && ns.in_len - at >= NET_HEADER) {
        const uint8_t *head = ns.in + at;
        unsigned type = head[4], len = ldl_le_p(head + 16);
        int64_t t = ldq_le_p(head + 8);

        if (memcmp(head, "CDJC", 4) || len > NET_PAYLOAD_MAX) {
            netsim_lost("a malformed message from the hub");
            return;
        }
        if (ns.in_len - at < NET_HEADER + len) {
            break;
        }
        if (type == NET_FRAME && len) {
            NetDue *due = g_malloc(sizeof(*due) + len), *queued;
            GList *link;

            due->due = t + ns.quantum;
            due->len = len;
            memcpy(due->data, head + NET_HEADER, len);
            /* Frames from different senders may come out of stamp order;
               the queue stays in time order, ties in arrival order. */
            for (link = ns.due.tail; link; link = link->prev) {
                queued = link->data;
                if (queued->due <= due->due) {
                    break;
                }
            }
            if (link) {
                g_queue_insert_after(&ns.due, link, due);
            } else {
                g_queue_push_head(&ns.due, due);
            }
            ns.received++;
        }
        if (t > ns.peer) {
            ns.peer = t;
        }
        /* Only a TIME's promise lets this player run on: a frame from one
           player says nothing about what another may still send. */
        if (type == NET_TIME && len == 8) {
            int64_t promise = ldq_le_p(head + NET_HEADER);

            if (promise > ns.peer_promise) {
                ns.peer_promise = promise;
            }
        }
        at += NET_HEADER + len;
    }
    memmove(ns.in, ns.in + at, ns.in_len - at);
    ns.in_len -= at;
}

static bool netsim_drain(void)
{
    while (ns.active) {
        ssize_t got;

        if (ns.in_len == sizeof(ns.in)) {
            netsim_parse();
            if (ns.in_len == sizeof(ns.in)) {
                netsim_lost("a message larger than the input buffer");
                break;
            }
        }
        got = recv(ns.fd, ns.in + ns.in_len, sizeof(ns.in) - ns.in_len,
                   MSG_DONTWAIT);
        if (got > 0) {
            ns.in_len += got;
        } else if (got == 0) {
            netsim_lost("the hub closed the link");
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            netsim_lost(strerror(errno));
        }
    }
    netsim_parse();
    return ns.active;
}

static void netsim_deliver(int64_t now)
{
    NetDue *due;

    while (ns.active && (due = g_queue_peek_head(&ns.due)) && due->due <= now) {
        g_queue_pop_head(&ns.due);
        ns.max_late = MAX(ns.max_late, now - due->due);
        ns.late += now > due->due;
        if (ns.trace) {
            uint64_t hash = UINT64_C(14695981039346656037);
            for (unsigned i = 0; i < MAX(due->len, 60); i++) {
                hash = (hash ^ (i < due->len ? due->data[i] : 0)) *
                       UINT64_C(1099511628211);
            }
            fprintf(ns.trace, "{\"event\":\"rx\",\"due\":%" PRId64
                    ",\"now\":%" PRId64 ",\"len\":%u,\"hash\":\"%016"
                    PRIx64 "\"}\n", due->due, now, due->len, hash);
            fflush(ns.trace);
        }
        ns.sink(ns.opaque, due->data, due->len);
        g_free(due);
    }
}

/* As cosim_promise: now while MAIN runs, else its next event. */
static int64_t netsim_promise(int64_t now)
{
    CPUState *cpu;
    NetDue *due;
    int64_t promise, deadline;

    /* With a shared wall epoch, do not grant idle lookahead: a descheduled
     * idle VM must still keep its clock within one quantum of its peers. */
    if (ns.wall_clock) {
        return now;
    }

    CPU_FOREACH(cpu) {
        if (!cpu->halted || cpu_has_work(cpu)) {
            return now;
        }
    }
    timer_del(ns.timer);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          QEMU_TIMER_ATTR_ALL);
    promise = deadline < 0 ? INT64_MAX : now + deadline;
    due = g_queue_peek_head(&ns.due);
    if (due && due->due < promise) {
        promise = due->due;
    }
    if (ns.peer_promise + ns.quantum < promise) {
        promise = ns.peer_promise + ns.quantum;
    }
    return promise > now ? promise : now;
}

static void netsim_report(int64_t now, int64_t promise)
{
    uint8_t payload[8];

    stq_le_p(payload, promise);
    netsim_send_msg(NET_TIME, now, payload, sizeof(payload));
    ns.promised = promise;
}

static void netsim_census(int64_t now)
{
    if (!ns.census_every || now < ns.census_next) {
        return;
    }
    ns.census_next = now + ns.census_every;
    fprintf(stderr, "cdj2000-netsim: t=%.3f steps %" PRIu64 " waits %" PRIu64
            " (%.1f s of wall clock) frames out %" PRIu64 " in %" PRIu64
            " hub promise t=%.6f late=%" PRIu64 " max_late_ns=%" PRId64
            " promise_violations=%" PRIu64 " icount=%" PRId64 "\n", now / 1e9, ns.steps, ns.waits,
            ns.wait_wall_ns / 1e9, ns.sent, ns.received,
            ns.peer_promise / 1e9, ns.late, ns.max_late, ns.promise_violations,
            icount_enabled() ? icount_get_raw() : 0);
}

static void netsim_step(void *opaque)
{
    int64_t now, next, promise, wall = 0, noted = 0;
    NetDue *due;

    if (!ns.active) {
        return;
    }
    now = netsim_now();
    ns.steps++;
    if (!netsim_drain()) {
        return;
    }
    netsim_deliver(now);
    promise = netsim_promise(now);
    if (now - ns.reported >= ns.quantum / 2 || promise > ns.promised) {
        netsim_report(now, promise);
    }
    while (ns.active && now >= ns.peer_promise + ns.quantum) {
        struct pollfd wait = { .fd = ns.fd, .events = POLLIN };

        promise = netsim_promise(now);
        if (ns.reported != now || promise != ns.promised) {
            netsim_report(now, promise);
        }
        if (!wall) {
            wall = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            ns.waits++;
        }
        int ready;
        if (ns.wall_clock) {
            /* This timer runs on the main thread without icount. Holding BQL
             * over a peer wait starves MAIN, even though its clock is clamped:
             * catching up timers can then coalesce TMU IRQs and skip beats.
             * Keep the ceiling fixed while letting the vCPU service work.
             * All segment state/queued frames remain protected after relock. */
            bql_unlock();
            ready = poll(&wait, 1, 1000);
            bql_lock();
        } else {
            ready = poll(&wait, 1, 1000);
        }
        if (ready == 0) {
            int64_t waited = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - wall;

            if (waited / NANOSECONDS_PER_SECOND >= noted + NET_WAIT_NOTE_S) {
                noted = waited / NANOSECONDS_PER_SECOND;
                fprintf(stderr, "cdj2000-netsim: waited %" PRId64 " s for the "
                        "other players at t=%.6f (hub promise %.6f)\n", noted,
                        now / 1e9, ns.peer_promise / 1e9);
            }
            continue;
        }
        if (!netsim_drain()) {
            return;
        }
        netsim_deliver(now);
    }
    if (wall) {
        ns.wait_wall_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - wall;
    }
    if (!ns.active) {
        return;
    }
    netsim_census(now);
    next = ns.peer_promise + ns.quantum;
    promise = netsim_promise(now);
    if (promise > ns.promised) {
        netsim_report(now, promise);
    }
    if (promise == now) {
        if (ns.reported + ns.quantum / 2 < next) {
            next = ns.reported + ns.quantum / 2;
        }
    } else if (promise < next) {
        next = promise;
    }
    due = g_queue_peek_head(&ns.due);
    if (due && due->due < next) {
        next = due->due;
    }
    if (next <= now) {
        next = now + 1;
    }
    timer_mod(ns.timer, next);
    if (ns.wall_clock) {
        qatomic_set(&ns.ceiling, next);
    }
}

void cdj_netsim_send(const uint8_t *frame, unsigned len)
{
    int64_t now;
    if (!ns.active) {
        return;
    }
    ns.sent++;
    now = netsim_now();
    ns.promise_violations += now < ns.promised;
    if (ns.trace) {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned i = 0; i < MAX(len, 60); i++) {
            hash = (hash ^ (i < len ? frame[i] : 0)) * UINT64_C(1099511628211);
        }
        fprintf(ns.trace, "{\"event\":\"tx\",\"now\":%" PRId64
                ",\"promise\":%" PRId64 ",\"len\":%u,\"hash\":\"%016"
                PRIx64 "\"}\n", now, ns.promised, len, hash);
        fflush(ns.trace);
    }
    netsim_send_msg(NET_FRAME, now, frame, len);
}

bool cdj_netsim_init(CdjNetsimFrameFn sink, void *opaque)
{
    const char *spec = getenv("CDJ_NETSIM");
    const char *quantum = getenv("CDJ_NETSIM_QUANTUM_US");
    const char *census = getenv("CDJ_NETSIM_CENSUS");
    const char *clock = getenv("CDJ_NETSIM_CLOCK");
    const char *trace = getenv("CDJ_NETSIM_TRACE");
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *found = NULL;
    g_autofree char *host = NULL;
    const char *colon;
    int one = 1, fd;

    if (!spec || !*spec) {
        return false;
    }
    if (clock && strcmp(clock, "wall")) {
        error_report("cdj2000-netsim: unknown CDJ_NETSIM_CLOCK=%s", clock);
        exit(1);
    }
    if (clock && icount_enabled()) {
        error_report("cdj2000-netsim: wall clock must run without -icount");
        exit(1);
    }
    if (!clock && !icount_enabled()) {
        error_report("cdj2000-netsim: CDJ_NETSIM requires -icount (sleep=off)");
        exit(1);
    }
    if (!strncmp(spec, "unix:", 5)) {
        struct sockaddr_un where = { .sun_family = AF_UNIX };

        if (strlen(spec + 5) >= sizeof(where.sun_path)) {
            error_report("cdj2000-netsim: socket path too long: %s", spec + 5);
            exit(1);
        }
        strcpy(where.sun_path, spec + 5);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&where, sizeof(where)) < 0) {
            error_report("cdj2000-netsim: cannot reach the hub at %s: %s", spec,
                         strerror(errno));
            exit(1);
        }
        goto connected;
    }
    colon = strrchr(spec, ':');
    host = colon ? g_strndup(spec, colon - spec) : g_strdup("127.0.0.1");
    if (getaddrinfo(*host ? host : "127.0.0.1", colon ? colon + 1 : spec,
                    &hints, &found) != 0 || !found) {
        error_report("cdj2000-netsim: cannot resolve CDJ_NETSIM=%s", spec);
        exit(1);
    }
    fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd < 0 || connect(fd, found->ai_addr, found->ai_addrlen) < 0) {
        error_report("cdj2000-netsim: cannot reach the hub at %s: %s", spec,
                     strerror(errno));
        exit(1);
    }
    freeaddrinfo(found);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
connected:
    ns.fd = fd;
    ns.quantum = (quantum && *quantum ? strtoll(quantum, NULL, 0) : 1000)
                 * SCALE_US;
    if (ns.quantum < 2 * SCALE_US) {
        ns.quantum = 2 * SCALE_US;
    }
    ns.census_every = (int64_t)((census && *census ? strtod(census, NULL)
                                 : 10.0) * NANOSECONDS_PER_SECOND);
    ns.census_next = ns.census_every;
    ns.sink = sink;
    ns.opaque = opaque;
    g_queue_init(&ns.due);
    ns.active = true;
    if (trace && *trace) {
        ns.trace = fopen(trace, "w");
        if (!ns.trace) {
            error_report("cdj2000-netsim: cannot write trace %s", trace);
            exit(1);
        }
    }
    if (clock) {
        netsim_wall_init(getenv("CDJ_NETSIM_CLOCK_FILE"));
    }
    netsim_send_msg(NET_HELLO, 0, NULL, 0);
    ns.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, netsim_step, NULL);
    timer_mod(ns.timer, netsim_now() + ns.quantum / 2);
    fprintf(stderr, "cdj2000-netsim: on the segment at %s, latency %" PRId64
                    " us\n", spec, ns.quantum / 1000);
    return true;
}
#endif
