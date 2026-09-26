/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What every CDJ board shares: the QEMU headers the common code needs, the
 * board descriptor, and the SH-4 boot and peripheral helpers in sh4_board.c
 * and sh4_periph.c. A board describes itself with a CdjBoardDesc; nothing in
 * common/ knows a firmware address.
 */
#ifndef CDJ_COMMON_H
#define CDJ_COMMON_H
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/notify.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/block/flash.h"
#include "hw/irq.h"
#include "hw/sh4/sh.h"
#include "hw/sh4/sh_intc.h"
#include "hw/timer/tmu012.h"
#include "chardev/char-fe.h"
/* QEMU 9.1 still uses the sysemu/ include prefix. */
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"

/* Strip the SH-4 segment bits to get the physical address, as sh7750.c does. */
#define A7ADDR(x) ((x) & 0x1fffffff)

/* RAM a board maps besides its DRAM. With an env knob it is opt-in: the
 * region stays unmapped unless the variable is set. */
typedef struct CdjRamRegion {
    const char *name;           /* memory region name, e.g. "cdj.area4" */
    const char *env;            /* opt-in knob, NULL = always mapped */
    hwaddr phys;
    uint64_t size;
    const char *what;           /* how the info line describes it */
} CdjRamRegion;

/* One run of equal flash sectors. A top-boot part is two runs. */
typedef struct CdjFlashBank {
    uint32_t blocks;
    uint32_t sector;
} CdjFlashBank;

typedef struct CdjBoardDesc {
    const char *name;           /* machine name, prefixes every log line */

    hwaddr dram_phys;
    uint64_t dram_size;
    const CdjRamRegion *extra_ram;
    unsigned n_extra_ram;

    /* NOR flash on CS0 as an AMD-command-set CFI device. */
    hwaddr flash_phys;
    uint64_t flash_size;
    CdjFlashBank flash[2];      /* the real sector layout */
    uint16_t flash_id[4];

    /* The decompressed MAIN image lands at dram_phys and is entered here. */
    uint32_t fw_entry;
    uint32_t init_sp;
    /* The SR the bootloader leaves when it enters the image; 0 keeps the
     * CPU's reset value (RB=1). */
    uint32_t init_sr;
    /* A global the RTOS restores SR from before it first writes it, seeded
     * after the image load; 0 when the firmware has none. */
    hwaddr sr_seed_slot;
    uint32_t sr_seed;

    uint32_t periph_hz;         /* TMU input clock, CDJ_PERIPH_HZ overrides */
    /* CDJ_DMAC_DEBUG logs the first INTEVT reads in this vector window. */
    uint16_t ccn_trace_lo, ccn_trace_hi;
    /* Board lines printed after the IRQ counts at exit; may be NULL. */
    void (*exit_report)(void);
} CdjBoardDesc;

/* The running board, set by cdj_board_init(). */
extern const CdjBoardDesc *cdj_board;
extern unsigned cdj_reset_count;

/* sh4_board.c: boot. Call init first, map the SoC, then load, then start. */
SuperHCPU *cdj_board_init(MachineState *machine, const CdjBoardDesc *desc);
void cdj_board_load(MachineState *machine);
void cdj_board_start(SuperHCPU *cpu);
void cdj_unimp(const char *name, hwaddr addr, hwaddr size);

/* sh4_periph.c: the SH-4 core blocks every board has. */
void cdj_ccn_init(MemoryRegion *sysmem, SuperHCPU *cpu);
void cdj_tmu_init(MemoryRegion *sysmem, hwaddr base,
                  qemu_irq ch0, qemu_irq ch1, qemu_irq ch2);
void cdj_scif(MemoryRegion *sysmem, const char *id,
              hwaddr addr, Chardev *chr);
qemu_irq cdj_count_irq(qemu_irq target, const char *name);
extern Notifier cdj_irqcount_exit;
void cdj_irqcount_dump(Notifier *n, void *opaque);
void cdj_irq_sink(void *opaque, int n, int level);
/* An unpopulated chip-select range: reads all ones, writes are logged. */
void cdj_open_bus(MemoryRegion *sysmem, const char *name, hwaddr base, hwaddr size);
/* A plain register block; init lists power-on values, ended by value 0. */
typedef struct CdjRegInit {
    hwaddr off;
    uint32_t value;
} CdjRegInit;
void cdj_regs(MemoryRegion *sysmem, const char *name, hwaddr base,
              hwaddr size, const CdjRegInit *init);

/* sh4_intc.c: set up, reset and report a board's sh_intc tables. */
void cdj_intc_setup(MemoryRegion *sysmem, SuperHCPU *cpu, struct intc_desc *desc,
                    int nr_sources,
                    struct intc_mask_reg *mask, int nmask,
                    struct intc_prio_reg *prio, int nprio,
                    struct intc_vect *vect, int nvect,
                    struct intc_group *groups, int ngroups);
void cdj_intc_report(struct intc_desc *desc, int nr_sources, uint16_t lo, uint16_t hi);

/* sh4a_dmac.c: the SH-4A DMAC. */
typedef struct CdjDmacDei {
    qemu_irq irq;               /* NULL = the channel raises no interrupt */
    unsigned max;               /* raise cap before it goes silent, 0 = none */
} CdjDmacDei;
void cdj_dmac_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                   hwaddr dreq_base, hwaddr dreq_size, const CdjDmacDei *dei);
void cdj_dmac_dreq(void);

const char *cdj_getenv(const char *name);
#endif
