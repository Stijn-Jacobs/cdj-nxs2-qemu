"""Compile production netsim, stubbing only QEMU timers/CPU plumbing.

Exercise a delayed clock reader, two processes joining the same epoch,
out-of-order sender stamps, delivery deadlines and timer barrier updates.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_production_segment_clock_and_delivery(tmp_path):
    cc = shutil.which('cc')
    pkg = shutil.which('pkg-config')
    if not cc or not pkg or subprocess.run([pkg, '--exists', 'glib-2.0']).returncode:
        pytest.skip('C compiler and glib required')
    source = (ROOT / 'hw/cdj/common/cdj_netsim.c').read_text()
    source = re.sub(r'^#include "[^\n]+"\n', '', source, flags=re.M)
    prelude = r'''
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define SCALE_US 1000
#define NANOSECONDS_PER_SECOND 1000000000
#define QEMU_CLOCK_VIRTUAL 0
#define QEMU_CLOCK_REALTIME 1
#define QEMU_TIMER_ATTR_ALL 0
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define qatomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_SEQ_CST)
#define qatomic_cmpxchg(p,o,n) __sync_val_compare_and_swap(p,o,n)
#define error_report(...) fprintf(stderr, __VA_ARGS__)
typedef void (*CdjNetsimFrameFn)(void *, const uint8_t *, unsigned);
typedef struct { int64_t expires; } QEMUTimer;
typedef struct { bool halted; } CPUState;
static CPUState stub_cpu;
#define CPU_FOREACH(c) for (c = &stub_cpu; c; c = NULL)
static bool cpu_has_work(CPUState *c) { return false; }
static int64_t host = 1000000;
static int64_t get_clock(void) { return host; }
static int64_t (*board_virtual_clock)(void);
typedef int RunState;
static void qemu_add_vm_change_state_handler(void (*cb)(void *, bool, RunState), void *o) {}
static int64_t qemu_clock_get_ns(int type) {
    return type == QEMU_CLOCK_REALTIME ? host : board_virtual_clock();
}
static void timer_del(QEMUTimer *t) { t->expires = -1; }
static void timer_mod(QEMUTimer *t, int64_t n) { t->expires = n; }
static QEMUTimer *timer_new_ns(int type, void (*cb)(void *), void *o) {
    return g_new0(QEMUTimer, 1);
}
static int64_t qemu_clock_deadline_ns_all(int t, int a) { return 100000; }
static void bql_unlock(void);
static void bql_lock(void);
static int test_poll(struct pollfd *, nfds_t, int);
#define poll test_poll
static bool icount_enabled(void) { return false; }
static int64_t icount_get_raw(void) { return 0; }
static uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v,p,8); return GUINT64_FROM_LE(v); }
static uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v,p,4); return GUINT32_FROM_LE(v); }
static void stq_le_p(void *p, uint64_t v) { v=GUINT64_TO_LE(v); memcpy(p,&v,8); }
static void stl_le_p(void *p, uint32_t v) { v=GUINT32_TO_LE(v); memcpy(p,&v,4); }
'''
    checks = r'''
static unsigned count;
static bool locked = true, exercise_wait;
static int peer_fd;
static unsigned unlocks;
static void bql_unlock(void) { assert(locked); locked = false; unlocks++; }
static void bql_lock(void) { assert(!locked); locked = true; }
static int test_poll(struct pollfd *fds, nfds_t n, int timeout) {
    if (exercise_wait) {
        /* A vCPU can acquire BQL and transmit while the main thread waits.
         * The old timer ceiling is still hard, including during this window. */
        assert(locked != ns.wall_clock);
        int64_t before = netsim_now();
        if (ns.wall_clock) {
            locked = true;
            uint8_t data = 42;
            cdj_netsim_send(&data, 1);
            locked = false;
        }
        assert(netsim_now() == before);
        uint8_t grant[28] = {0};
        memcpy(grant, "CDJC", 4); grant[4] = NET_TIME;
        stq_le_p(grant + 8, 10000000); stl_le_p(grant + 16, 8);
        stq_le_p(grant + 20, 10000000);
        assert(write(peer_fd, grant, sizeof(grant)) == sizeof(grant));
        exercise_wait = false;
    }
    #undef poll
    return poll(fds, n, timeout);
    #define poll test_poll
}
static uint8_t delivered[8];
static void sink(void *o, const uint8_t *p, unsigned n) {
    assert(n == 1); delivered[count++] = p[0];
}
static void frame(int64_t stamp, uint8_t value) {
    uint8_t *p = ns.in + ns.in_len;
    memset(p, 0, 21); memcpy(p, "CDJC", 4); p[4] = NET_FRAME;
    stq_le_p(p+8, stamp); stl_le_p(p+16, 1); p[20] = value;
    ns.in_len += 21;
}
int main(int argc, char **argv) {
    ns.quantum = 1000000;
    netsim_wall_init(argv[1]);
    netsim_vm_state(NULL, true, 0);
    assert(netsim_wall_clock() == 0);
    host += 100000000; /* a descheduled main loop cannot overrun its grant */
    assert(netsim_wall_clock() == 500000);
    netsim_vm_state(NULL, false, 0);
    host += 100000000;
    assert(netsim_wall_clock() == 500000);
    netsim_vm_state(NULL, true, 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        int64_t epoch = *ns.epoch;
        host += 123456789;
        netsim_wall_init(argv[1]); /* late joining deck retains original epoch */
        assert(*ns.epoch == epoch);
        assert(netsim_wall_clock() == 500000);
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ns.active = true; ns.sink = sink; g_queue_init(&ns.due);
    frame(200, 2); frame(100, 1); frame(200, 3);
    netsim_parse();
    netsim_deliver(ns.quantum + 99); assert(count == 0);
    netsim_deliver(ns.quantum + 100); assert(count == 1 && delivered[0] == 1);
    netsim_deliver(ns.quantum + 200);
    assert(count == 3 && delivered[1] == 2 && delivered[2] == 3);
    assert(ns.late == 0 && ns.max_late == 0);
    int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    ns.fd = pair[0]; ns.peer_promise = 10000000;
    ns.timer = timer_new_ns(0, NULL, NULL);
    /* Running CPU only promises now, not an idle jump into the future. */
    assert(netsim_promise(netsim_now()) == netsim_now());
    stub_cpu.halted = true;
    assert(netsim_promise(netsim_now()) == netsim_now());
    stub_cpu.halted = false;
    netsim_step(NULL);
    assert(ns.ceiling == ns.timer->expires);
    assert(ns.ceiling <= ns.peer_promise + ns.quantum);
    assert(netsim_wall_clock() <= ns.ceiling);
    ns.peer_promise = netsim_now() - ns.quantum;
    peer_fd = pair[1]; exercise_wait = true;
    netsim_step(NULL);
    assert(!exercise_wait && unlocks == 1 && locked);
    assert(ns.sent == 1 && ns.promise_violations == 0);
    ns.wall_clock = false;
    ns.peer_promise = netsim_now() - ns.quantum;
    exercise_wait = true;
    netsim_step(NULL);
    assert(!exercise_wait && locked && unlocks == 1);
    ns.wall_clock = true;
    assert(ns.ceiling <= ns.peer_promise + ns.quantum);
    close(pair[0]); close(pair[1]);
    child = fork(); assert(child >= 0);
    if (!child) { netsim_lost("test disconnect"); _exit(99); }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    puts("segment clock, shared epoch, FIFO ties, deadlines and barrier: PASS");
}
'''
    program = tmp_path / 'netsim.c'
    program.write_text(prelude + source + checks)
    flags = shlex.split(subprocess.check_output([pkg, '--cflags', '--libs', 'glib-2.0'], text=True))
    exe = tmp_path / 'netsim'
    subprocess.run([cc, '-std=gnu11', '-O1', str(program), '-o', str(exe), *flags], check=True)
    subprocess.run([str(exe), str(tmp_path / 'epoch')], check=True, timeout=10)
    corrupt = tmp_path / 'corrupt'
    corrupt.write_bytes(b'bad')
    assert subprocess.run([str(exe), str(corrupt)], timeout=10).returncode == 1
    assert subprocess.run([str(exe), ''], timeout=10).returncode == 1
