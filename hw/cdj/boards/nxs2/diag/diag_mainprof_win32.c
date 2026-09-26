/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "cdj_getenv.h"
/*
 * CDJ_MAINPROF=<path>[:<start virtual s>] (Windows only): a sampling profiler
 * for MAIN's vCPU thread, since perf is not available on Windows. A thread
 * samples the vCPU's instruction pointer at ~1 kHz (SuspendThread +
 * GetThreadContext; no lock is taken while it is suspended). Samples inside
 * translated code are charged to the block's guest pc; others are written as
 * host addresses with their module. Output goes to <path>.<pid> at exit.
 */
#ifdef _WIN32
static struct {
    HANDLE vcpu;
    GHashTable *guest, *host;       /* pc/rip -> count */
    uint64_t samples, in_tcg;
    int64_t start_virt_ns;
    bool quit;
    char *path;
    QemuThread thread;
} cdj_mprof;

static void *cdj_mprof_thread(void *opaque)
{
    CONTEXT ctx;

    while (!qatomic_read(&cdj_mprof.quit)) {
        TranslationBlock *tb;
        uintptr_t rip;

        Sleep(1);
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < cdj_mprof.start_virt_ns) {
            continue;
        }
        if (SuspendThread(cdj_mprof.vcpu) == (DWORD)-1) {
            continue;
        }
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        rip = GetThreadContext(cdj_mprof.vcpu, &ctx) ? (uintptr_t)ctx.Rip : 0;
        ResumeThread(cdj_mprof.vcpu);
        if (!rip) {
            continue;
        }
        cdj_mprof.samples++;
        tb = tcg_tb_lookup(rip);
        if (tb) {
            gpointer k = (gpointer)(uintptr_t)tb->pc;

            cdj_mprof.in_tcg++;
            g_hash_table_insert(cdj_mprof.guest, k, (gpointer)(uintptr_t)(
                GPOINTER_TO_SIZE(g_hash_table_lookup(cdj_mprof.guest, k)) + 1));
        } else {
            gpointer k = (gpointer)rip;

            g_hash_table_insert(cdj_mprof.host, k, (gpointer)(uintptr_t)(
                GPOINTER_TO_SIZE(g_hash_table_lookup(cdj_mprof.host, k)) + 1));
        }
    }
    return NULL;
}

static void cdj_mprof_dump(Notifier *n, void *unused)
{
    FILE *f;
    GHashTableIter it;
    gpointer k, v;
    char mod[MAX_PATH];

    qatomic_set(&cdj_mprof.quit, true);
    qemu_thread_join(&cdj_mprof.thread);
    f = fopen(cdj_mprof.path, "w");
    if (!f) {
        warn_report("CDJ_MAINPROF: cannot write %s", cdj_mprof.path);
        return;
    }
    fprintf(f, "# samples %" PRIu64 " in_tcg %" PRIu64 " exe_base 0x%llx\n",
            cdj_mprof.samples, cdj_mprof.in_tcg,
            (unsigned long long)(uintptr_t)GetModuleHandle(NULL));
    g_hash_table_iter_init(&it, cdj_mprof.guest);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        fprintf(f, "G 0x%08lx %zu\n", (unsigned long)(uintptr_t)k, GPOINTER_TO_SIZE(v));
    }
    g_hash_table_iter_init(&it, cdj_mprof.host);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        HMODULE m = NULL;
        uintptr_t base = 0;

        mod[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)k, &m) && m) {
            GetModuleFileNameA(m, mod, sizeof mod);
            base = (uintptr_t)m;
        }
        fprintf(f, "H 0x%llx %zu 0x%llx %s\n", (unsigned long long)(uintptr_t)k,
                GPOINTER_TO_SIZE(v), (unsigned long long)base, mod[0] ? mod : "?");
    }
    fclose(f);
    info_report("CDJ_MAINPROF: %" PRIu64 " samples (%" PRIu64 " in translated code) "
                "written to %s", cdj_mprof.samples, cdj_mprof.in_tcg, cdj_mprof.path);
}

/* Called on the vCPU thread (the lockstep tick), once. */
void cdj_mprof_arm(void)
{
    static Notifier exit_n = { .notify = cdj_mprof_dump };
    const char *e = getenv("CDJ_MAINPROF");
    char **parts;

    if (!e || !*e || cdj_mprof.path) {
        return;
    }
    parts = g_strsplit(e, ":", 0);
    /* A Windows path has a drive colon: C:/x/y:40 splits as C, /x/y, 40. */
    if (parts[0] && strlen(parts[0]) == 1 && parts[1]) {
        cdj_mprof.path = g_strconcat(parts[0], ":", parts[1], NULL);
        cdj_mprof.start_virt_ns = parts[2] ? strtoll(parts[2], NULL, 0) * NANOSECONDS_PER_SECOND : 0;
    } else {
        cdj_mprof.path = g_strdup(parts[0]);
        cdj_mprof.start_virt_ns = parts[1] ? strtoll(parts[1], NULL, 0) * NANOSECONDS_PER_SECOND : 0;
    }
    g_strfreev(parts);
    {
        /* Every deck of a rig inherits the same env: one file per process. */
        char *pp = g_strdup_printf("%s.%d", cdj_mprof.path, (int)getpid());

        g_free(cdj_mprof.path);
        cdj_mprof.path = pp;
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &cdj_mprof.vcpu, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                         THREAD_QUERY_INFORMATION, FALSE, 0)) {
        warn_report("CDJ_MAINPROF: cannot open the vCPU thread");
        return;
    }
    cdj_mprof.guest = g_hash_table_new(NULL, NULL);
    cdj_mprof.host = g_hash_table_new(NULL, NULL);
    qemu_thread_create(&cdj_mprof.thread, "cdj-mainprof", cdj_mprof_thread, NULL,
                       QEMU_THREAD_JOINABLE);
    qemu_add_exit_notifier(&exit_n);
    info_report("CDJ_MAINPROF: sampling vCPU thread %d from virtual %.0f s into %s",
                qemu_get_thread_id(), cdj_mprof.start_virt_ns / 1e9, cdj_mprof.path);
}
#else
void cdj_mprof_arm(void)
{
}
#endif

