/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic host stand-ins: the test includes the actual board model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>

typedef uint64_t hwaddr;
typedef int *qemu_irq;
typedef struct MemoryRegion { int unused; } MemoryRegion;
typedef struct Notifier {void (*notify)(struct Notifier *,void *);} Notifier;
typedef struct QEMUTimer {void (*callback)(void *);void *opaque;int64_t due;bool armed;} QEMUTimer;
typedef struct VMStateField {int unused;} VMStateField;
typedef struct VMStateDescription {
 const char *name; int version_id,minimum_version_id;
 int (*pre_save)(void *);int (*pre_load)(void *);int (*post_load)(void *,int);
 const VMStateField *fields;
} VMStateDescription;
typedef struct MemoryRegionOps {
 uint64_t (*read)(void *,hwaddr,unsigned);
 void (*write)(void *,hwaddr,uint64_t,unsigned);
 int endianness;
 struct {unsigned min_access_size,max_access_size;} valid;
} MemoryRegionOps;
#define VMSTATE_UINT32(...) {0}
#define VMSTATE_UINT32_ARRAY(...) {0}
#define VMSTATE_BUFFER(...) {0}
#define VMSTATE_INT64(...) {0}
#define VMSTATE_TIMER_PTR(...) {0}
#define VMSTATE_END_OF_LIST() {0}
#define QEMU_CLOCK_VIRTUAL 0
#define DEVICE_LITTLE_ENDIAN 0
#define NANOSECONDS_PER_SECOND 1000000000LL
#define MIN(a,b) ((a)<(b)?(a):(b))
#define A7ADDR(a) (a)

static int64_t test_now;
static unsigned test_errors;
static char test_last_log[512];
static uint8_t test_memory[8192];
static QEMUTimer test_timers[4];
static unsigned test_timer_count;
static void test_log(bool error,const char *format,...)
{
 va_list ap;va_start(ap,format);vsnprintf(test_last_log,sizeof(test_last_log),format,ap);va_end(ap);
 test_errors+=error;
}
#define info_report(...) test_log(false,__VA_ARGS__)
#define error_report(...) test_log(true,__VA_ARGS__)
#define warn_report(...) test_log(false,__VA_ARGS__)
static inline int g_strcmp0(const char *a,const char *b) {return a?strcmp(a,b):-1;}
static inline uint32_t extract32(uint32_t v,unsigned shift,unsigned bits) {return (v>>shift)&(UINT32_MAX>>(32-bits));}
static inline uint32_t deposit32(uint32_t old,unsigned shift,unsigned bits,uint32_t v)
{uint32_t mask=(UINT32_MAX>>(32-bits))<<shift;return (old&~mask)|((v<<shift)&mask);}
static inline int64_t qemu_clock_get_ns(int clock) {return test_now;}
static inline QEMUTimer *timer_new_ns(int clock,void (*callback)(void *),void *opaque)
{assert(test_timer_count<4);QEMUTimer *t=&test_timers[test_timer_count++];*t=(QEMUTimer){.callback=callback,.opaque=opaque};return t;}
static inline void timer_mod(QEMUTimer *t,int64_t due) {t->due=due;t->armed=true;}
static void qemu_set_irq(qemu_irq irq,int level);
static inline void physical_memory_read(hwaddr at,void *to,size_t n) {assert(at+n<=sizeof(test_memory));memcpy(to,test_memory+at,n);}
static inline void physical_memory_write(hwaddr at,const void *from,size_t n) {assert(at+n<=sizeof(test_memory));memcpy(test_memory+at,from,n);}
static inline void qemu_hexdump(FILE *f,const char *prefix,const uint8_t *p,size_t n) {}
static inline void cdj_add_exit_report(Notifier *n) {}
static inline void vmstate_register_any(void *owner,const VMStateDescription *desc,void *state) {}
static inline void memory_region_init_io(MemoryRegion *r,void *o,const MemoryRegionOps *ops,void *p,const char *name,size_t n) {}
static inline void memory_region_add_subregion(MemoryRegion *r,hwaddr addr,MemoryRegion *child) {}
static void cdj2000_display_send(const uint8_t *packet,size_t len);
