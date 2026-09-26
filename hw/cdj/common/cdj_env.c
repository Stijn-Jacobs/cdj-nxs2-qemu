/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
/*
 * Cached getenv(). The board reads many knobs on per-access paths, and
 * msvcrt's getenv() walks the whole environment each time.
 *
 *   CDJ_ENVCACHE=1  look each name up once and keep the answer.
 *   CDJ_ENVCACHE=2  also count lookups and print the busiest names every
 *                   2^20 lookups.
 *
 * Safe because nothing changes the environment after startup.
 */
#define CDJ_ENV_SLOTS 512                      /* power of two */

typedef struct CdjEnvSlot {
    const char *name;                           /* NULL = empty; set last */
    const char *value;                          /* NULL = unset */
    uint64_t hits;
} CdjEnvSlot;

static CdjEnvSlot cdj_env_slot[CDJ_ENV_SLOTS];
static uint64_t cdj_env_lookups;

static int cdj_env_mode(void)
{
    static int mode = -1;

    if (unlikely(mode < 0)) {
        const char *e = getenv("CDJ_ENVCACHE");

        mode = e && *e ? atoi(e) : 0;
        if (mode) {
            info_report("cdj2000nxs2: CDJ_ENVCACHE=%d", mode);
        }
    }
    return mode;
}

static void cdj_env_print(void)
{
    const CdjEnvSlot *top[10] = { NULL };
    unsigned i, j;

    for (i = 0; i < CDJ_ENV_SLOTS; i++) {
        const CdjEnvSlot *s = &cdj_env_slot[i];

        for (j = 0; qatomic_read(&s->name) && j < ARRAY_SIZE(top); j++) {
            if (!top[j] || s->hits > top[j]->hits) {
                memmove(&top[j + 1], &top[j],
                        (ARRAY_SIZE(top) - j - 1) * sizeof(top[0]));
                top[j] = s;
                break;
            }
        }
    }
    fprintf(stderr, "[CDJ] envcache lookups=%" PRIu64 " top:",
            qatomic_read(&cdj_env_lookups));
    for (j = 0; j < ARRAY_SIZE(top) && top[j]; j++) {
        fprintf(stderr, " %s=%" PRIu64, top[j]->name, top[j]->hits);
    }
    fprintf(stderr, "\n");
}

const char *cdj_getenv(const char *name)
{
    static QemuMutex fill_lock;
    static gsize fill_lock_ready;
    int mode = cdj_env_mode();
    uint32_t h = 2166136261u;
    const char *p;
    unsigned i, probes;

    if (!mode) {
        return getenv(name);
    }
    for (p = name; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    for (probes = 0; probes < CDJ_ENV_SLOTS; probes++) {
        CdjEnvSlot *s;
        const char *n;

        i = (h + probes) & (CDJ_ENV_SLOTS - 1);
        s = &cdj_env_slot[i];
        n = qatomic_load_acquire(&s->name);
        if (!n) {
            /* Not cached yet: fill this slot, or whichever one another
             * thread filled with the same name while we waited. */
            if (g_once_init_enter(&fill_lock_ready)) {
                qemu_mutex_init(&fill_lock);
                g_once_init_leave(&fill_lock_ready, 1);
            }
            qemu_mutex_lock(&fill_lock);
            n = s->name;
            if (!n) {
                const char *v = getenv(name);

                s->value = v ? g_strdup(v) : NULL;
                qatomic_store_release(&s->name, g_strdup(name));
                n = s->name;
            }
            qemu_mutex_unlock(&fill_lock);
        }
        if (strcmp(n, name) == 0) {
            if (mode >= 2) {
                qatomic_inc(&s->hits);
                if ((qatomic_fetch_inc(&cdj_env_lookups) & 0xFFFFF) == 0xFFFFF) {
                    cdj_env_print();
                }
            }
            return s->value;
        }
    }
    return getenv(name);                        /* table full */
}
