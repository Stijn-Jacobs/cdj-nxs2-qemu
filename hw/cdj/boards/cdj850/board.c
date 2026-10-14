/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "dsp56371.h"
#include "xc/adc.h"
#include "xc/cmt.h"
#include "xc/panel_link.h"
#include "xc/port.h"
#include "xc/scif.h"
#include "xc/ser.h"
#include "xc/ssi.h"
#include "xc/vfd.h"
/*
 * Pioneer CDJ-850 MAIN board (Renesas SH7263, SH-2A) -- bring-up.
 *
 * The SoC is the SH7263 (SH-2A core, big-endian: this machine lives in the
 * sh4eb target, like the SH7269 GUI board, and runs with -cpu sh2a). The
 * service manual (CDJ-850 service manual, block diagram and parts list)
 * names the parts: IC103 is the MAIN CPU (SH7263), IC101 the MAIN
 * firmware flash (DYW1796, "FLASH ROM(32M)"), IC102 the work SDRAM
 * (K4S561632J-UC75, "SDRAM(256M)"), the bus clock is CLK_66M at 66 MHz for
 * flash and SDRAM, and the crystals are X101 33 MHz (CPU/bus PLL source on
 * EXTAL) and X102 48 MHz (USB clock).
 *
 * Memory map (SH7263 bus state controller areas; SDRAM belongs on CS3 when
 * only one SDRAM space is used, per the SH7262/SH7264 SDRAM app note):
 *   CS0  0x00000000  4 MB NOR flash (DYW1796, 32 Mbit = 4 MB).
 *   CS3  0x0C000000  32 MB SDRAM (K4S561632J, 256 Mbit = 32 MB).
 *   on-chip RAM 0xFFF80000 (SH7263: 64 KB high-speed + 16 KB retention;
 *     the reset SP vector is 0xFFF90000, the top of the 64 KB bank).
 *   on-chip peripherals 0xFFFC0000 up (BSC at 0xFFFC0000, CPG at 0xFFFE0000,
 *     ports at 0xFFFE3800+, all seen as literals in the boot block).
 *
 * Boot: through Pioneer's own bootloader in the NOR flash, the way the real
 * unit starts. make_flash_cdj850.py lays boot.bin at flash 0 and main_flat.bin
 * at 0x60000 (the updater's own offsets, per notes/cdj850-layout.md) in an
 * otherwise-erased 4 MB image. SH-2A power-on reset loads PC from the vector
 * at 0 and SP from the vector at 4 (SH-2A Software Manual §3; SH7263 hardware
 * manual §5.2.3); the image holds PC = 0x00000100, SP = 0xFFF90000, with the
 * manual-reset copies at 8/12. There is no -kernel path: the MAIN region
 * starts with a 32-byte label, not code, and runs in place from flash.
 */

/* DYW1796 32 Mbit = 4 MB: 63 x 64 KiB + 8 x 8 KiB = 4032 + 64 = 4096 KiB.
 * Top-boot shape mirrored from the cdj900's 32 Mbit part; the exact DYW1796
 * sector map is unverified (only erase/program care, and nothing programs
 * yet). */
/* K4S561632J 256 Mbit = 32 MB of 16-bit SDRAM on CS3. */

static const CdjBoardDesc cdj850_board = {
    .name = "cdj850",
    .dram_phys = 0x0C000000,
    .dram_size = 32 * MiB,
    .flash_phys = 0x00000000,
    .flash_size = 4 * MiB,
    .flash = { { 63, 64 * KiB }, { 8, 8 * KiB } },
    /* Unverified for the DYW1796 (Spansion 32 Mbit values, as on cdj900). */
    .flash_id = { 0x0001, 0x227e, 0x2220, 0x2200 },
    .flash_be = true,
    /* Power-on reset PC vector (flash offset 0); do not copy cdj900's. */
    .fw_entry = 0x00000100,
    /* Power-on reset SP vector (flash offset 4). */
    .init_sp = 0xFFF90000,
    /* 0 keeps the CPU's reset value (IMASK=15); cdj850_reset then clears
     * the SH-4-only RB/BL bits, which do not exist on the SH-2A (VBR=0
     * comes from the reset itself). The bootloader sets SR after that. */
    .init_sr = 0,
    .sr_seed_slot = 0,
    .sr_seed = 0,
    /* CLK_66M bus clock. Pphi (peripheral clock, 33 MHz max) feeds the timers,
     * not the bus; no timer is modelled yet, so this is currently inert. */
    .periph_hz = 66000000,
    /* No INTEVT trace window (the SH-4 CCN/INTC model does not apply). */
    .ccn_trace_lo = 1,
    .ccn_trace_hi = 0,
    .exit_report = NULL,
};

/* SH-2A vector reset, after the SH7269 GUI board's sh7269_reset: reset the CPU
 * (which leaves SH-4-style pc=0xA0000000) and apply the board's vectors. */
static void cdj850_reset(void *opaque)
{
    SuperHCPU *cpu = opaque;
    CPUSH4State *env = &cpu->env;

    cpu_reset(CPU(cpu));
    env->pc = cdj_board->fw_entry;
    env->gregs[15] = cdj_board->init_sp;
    /* SH-2A has no MMU and runs privileged from reset. */
    env->mmucr = 0;
    /*
     * The SH-2A status register has no MD/RB/BL bits (SH-2A Software
     * Manual §2.2.2: only T/S/IMASK/M/Q/CS/BO exist; reset sets IMASK
     * and clears BO/CS, reserved bits read 0), but the sh4 target's
     * reset applies SH-4 semantics (MD|RB|BL|IMASK). A stale RB=1
     * selects the alternate R0-R7 bank for every translated TB, so the
     * pool-2 registration in the init-call chain (0x0C0C64B0 loads base
     * 0xFFF86638 into r4, then an ldc of a from-zero SR clears RB)
     * strands its r4/r5 in bank 1 and registers base 0 instead: every
     * pool-2 alloc NULLs and RTOS init halts with E_SYS at 0x0C0B8DBE
     * (see notes/cdj850-bringup.md, Task 6c). Clear the SH-4-only bits
     * at reset. MD stays set (the translator forces privileged mode
     * for SH-2A and keys memidx off it); IMASK stays set per the
     * manual.
     */
    env->sr &= ~((1u << SR_RB) | (1u << SR_BL));
}

/* On-chip RAM: 256 KB at 0xFFF80000, up to where the peripherals begin at
 * 0xFFFC0000 (same convention as the SH7269 board). Covers the 64 KB
 * high-speed bank the reset stack lives in plus the retention bank. */
#define CDJ850_OCRAM_BASE   0xFFF80000
#define CDJ850_OCRAM_SIZE   (256 * KiB)

/* CDJ850_TRACE=1 logs every access to the modelled devices with the guest
 * PC and virtual time, for bring-up gap analysis. */
static bool cdj850_trace(void)
{
    static int trace = -1;

    if (trace < 0) {
        const char *e = getenv("CDJ850_TRACE");

        trace = e && *e != '0';
    }
    return trace;
}

static uint64_t cdj850_vms(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
}

/*
 * INTC at 0xFFFE0800, as plain storage so the firmware's read-modify-writes
 * of the ICR/IRQSR/IPR registers read back. Interrupt delivery does not
 * consult these (see xc/cmt.c).
 */
#define CDJ850_INTC_BASE    0xFFFE0800
#define CDJ850_INTC_SIZE    0x800

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_INTC_SIZE];
} Cdj850Intc;

static uint64_t cdj850_intc_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Intc *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_INTC_SIZE - 1)];
    }
    if (cdj850_trace()) {
        info_report("cdj850: intc read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_intc_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Intc *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (CDJ850_INTC_SIZE - 1)] = val >> (8 * i);
    }
    if (cdj850_trace()) {
        info_report("cdj850: intc write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_intc_ops = {
    .read = cdj850_intc_read,
    .write = cdj850_intc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_intc = {
    .name = "cdj850-intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Intc, CDJ850_INTC_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Intc *s = g_new0(Cdj850Intc, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_intc, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_intc_ops, s,
                          "cdj850.intc", CDJ850_INTC_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_INTC_BASE, &s->iomem,
                                        1);
}

/*
 * Cache control word at 0xFFFC1000: the first register the boot block
 * touches (writes 0, 0x808, 0x101, two readbacks). Storage only; no cache
 * is modelled, so any value reads back.
 */
typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint32_t ccr;
} Cdj850Ccr;

static uint64_t cdj850_ccr_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Ccr *s = opaque;

    (void)off;
    (void)size;
    if (cdj850_trace()) {
        info_report("cdj850: ccr read = 0x%x at pc=0x%08x %" PRIu64 " ms",
                    s->ccr, s->cpu->env.pc, cdj850_vms());
    }
    return s->ccr;
}

static void cdj850_ccr_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Cdj850Ccr *s = opaque;

    (void)off;
    (void)size;
    if (cdj850_trace()) {
        info_report("cdj850: ccr write = 0x%x at pc=0x%08x %" PRIu64 " ms",
                    (unsigned)val, s->cpu->env.pc, cdj850_vms());
    }
    s->ccr = val;
}

static const MemoryRegionOps cdj850_ccr_ops = {
    .read = cdj850_ccr_read,
    .write = cdj850_ccr_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_ccr = {
    .name = "cdj850-ccr",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccr, Cdj850Ccr),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_ccr_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Ccr *s = g_new0(Cdj850Ccr, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_ccr, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ccr_ops, s,
                          "cdj850.ccr", 4);
    /* Priority 1: sh7263.internal covers this address. */
    memory_region_add_subregion_overlap(sysmem, 0xFFFC1000, &s->iomem, 1);
}

/* Port/PFC (0xFFFE3800), serial (0xFFFE7000) and mailbox (0xFFFFC000)
 * live in xc/ now that the board carries more than two peripheral
 * families (Task 6e file-split rule). */

/*
 * CCN audit shadow at 0xFF000000. The SH7263 has no SH-4-style CCN block,
 * but cdj_board_init maps the SH-4 CCN model there, whose accesses are
 * silent. This stub shadows it with identical semantics (EXPEVT/INTEVT
 * mirror the CPU state, every other register powers on zero) and reports
 * the first accesses with the guest PC, so a future MAIN gap at 0xFF00xxxx
 * shows up in the bring-up log instead of vanishing into the wrong model.
 */
#define CDJ850_CCNAUDIT_BASE  0xFF000000
#define CDJ850_CCNAUDIT_SIZE  0x30
#define CDJ850_CCNAUDIT_LOG   16

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_CCNAUDIT_SIZE];
    unsigned accesses;
} Cdj850CcnAudit;

static uint64_t cdj850_ccnaudit_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850CcnAudit *s = opaque;
    uint64_t v = 0;
    unsigned i;

    if (off == 0x24) {
        return s->cpu->env.expevt;
    }
    if (off == 0x28) {
        return s->cpu->env.intevt;
    }
    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_CCNAUDIT_SIZE - 1)];
    }
    if (s->accesses < CDJ850_CCNAUDIT_LOG ||
        (s->accesses & 4095) == 0) {
        info_report("cdj850: ccn-audit read +0x%x size %u at pc=0x%08x #%u",
                    (unsigned)off, size, s->cpu->env.pc, s->accesses + 1);
    }
    s->accesses++;
    return v;
}

static void cdj850_ccnaudit_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size)
{
    Cdj850CcnAudit *s = opaque;
    unsigned i;

    if (off == 0x24) {
        s->cpu->env.expevt = val;
    } else if (off == 0x28) {
        s->cpu->env.intevt = val;
    } else {
        for (i = 0; i < size; i++) {
            s->reg[(off + size - 1 - i) & (CDJ850_CCNAUDIT_SIZE - 1)] =
                val >> (8 * i);
        }
    }
    if (s->accesses < CDJ850_CCNAUDIT_LOG ||
        (s->accesses & 4095) == 0) {
        info_report("cdj850: ccn-audit write +0x%x size %u = 0x%x "
                    "at pc=0x%08x #%u", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, s->accesses + 1);
    }
    s->accesses++;
}

static const MemoryRegionOps cdj850_ccnaudit_ops = {
    .read = cdj850_ccnaudit_read,
    .write = cdj850_ccnaudit_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_ccnaudit = {
    .name = "cdj850-ccn-audit",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850CcnAudit, CDJ850_CCNAUDIT_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_ccnaudit_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850CcnAudit *s = g_new0(Cdj850CcnAudit, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_ccnaudit, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ccnaudit_ops, s,
                          "cdj850.ccn-audit", CDJ850_CCNAUDIT_SIZE);
    /* Priority 1: the SH-4 CCN model from cdj_board_init covers this. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_CCNAUDIT_BASE,
                                        &s->iomem, 1);
}

static void cdj850_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ocram = g_new(MemoryRegion, 1);
    SuperHCPU *cpu = cdj_board_init(machine, &cdj850_board);
    uint8_t reset_word[2];

    if (machine->kernel_filename) {
        error_report("%s: no -kernel path; boot through the bootloader in "
                     "the NOR flash (MAIN_BOOT=flash, image from "
                     "scripts/firmware/make_flash_cdj850.py)", cdj850_board.name);
        exit(1);
    }
    /* The reset vectors are the first words of the flash; erased there means
     * an image built without the bootloader, and the CPU would run 0xFFFF. */
    cpu_physical_memory_read(cdj850_board.flash_phys, reset_word,
                             sizeof(reset_word));
    if (reset_word[0] == 0xFF && reset_word[1] == 0xFF) {
        error_report("%s: the NOR flash holds no bootloader; build it with "
                     "scripts/firmware/make_flash_cdj850.py from the prepared "
                     "firmware", cdj850_board.name);
        exit(1);
    }

    memory_region_init_ram(ocram, NULL, "cdj850.ocram", CDJ850_OCRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, CDJ850_OCRAM_BASE, ocram);

    /*
     * On-chip RAM above the peripherals (0xFFFF0000-0xFFFFFFFF): the boot
     * block zero-fills 0xFFFFFCA4-0xFFFFFEA3 descending, so this must be
     * RAM, not a log-and-discard stub. Priority 1 over sh7263.internal.
     * If a later gap lands a real peripheral in this window, carve it out
     * with a higher-priority overlap.
     */
    {
        MemoryRegion *hi = g_new(MemoryRegion, 1);

        memory_region_init_ram(hi, NULL, "cdj850.onchip-hi", 64 * KiB,
                               &error_fatal);
        memory_region_add_subregion_overlap(sysmem, 0xFFFF0000, hi, 1);
    }

    /*
     * Cache address/data arrays as plain RAM: no cache is modelled, and the
     * boot block writes a tag pattern across 0xF0800228-0xF0800418 that a
     * later readback must see. Same convention as the SH7269 board.
     */
    {
        MemoryRegion *cache = g_new(MemoryRegion, 1);
        MemoryRegion *cache_p0 = g_new(MemoryRegion, 1);

        memory_region_init_ram(cache, NULL, "cdj850.cache-sink", 0x1000000,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0xF0000000, cache);
        memory_region_init_ram(cache_p0, NULL, "cdj850.cache-p0-sink", 0x1000,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0xE0000000, cache_p0);
    }

    cdj850_ccr_init(sysmem, cpu);
    cdj850_intc_init(sysmem, cpu);
    cdj850_adc_init(sysmem, cpu);
    cdj850_dsp_init();
    cdj850_port_init(sysmem, cpu);
    cdj850_ser_init(sysmem, cpu);
    cdj850_scif_init(sysmem, cpu);
    cdj850_ssi_init(sysmem, cpu);
    cdj850_cmt_init(sysmem, cpu);
    /* Audit the silent SH-4 CCN mapping (see the device comment). */
    cdj850_ccnaudit_init(sysmem, cpu);
    /* Panel link + VFD text window (MAIN-side model; panel CPU not
     * emulated). The window stays empty under -nographic. */
    panel_link_init();
    vfd_init();

    /* On-chip peripheral block (BSC/CPG, less CCR/INTC/CMT/ports modelled
     * above): every remaining access is logged rather than silently
     * reading zero. No A7 alias: SH-2A has no segments, and an alias could
     * shadow a real area. */
    create_unimplemented_device("sh7263.internal", 0xFFFC0000, 0x40000);
    /* Cache windows are RAM-backed sinks above; the P0 note stays for the
     * record of what the boot block touches (literals 0xF0000008,
     * 0xF0800008, 0xE0000000). */

    /* Unpopulated chip-select ranges: reads all ones, writes logged. */
    cdj_open_bus(sysmem, "sh7263.cs0-rest", 0x00400000, 0x03C00000);
    cdj_open_bus(sysmem, "sh7263.cs1", 0x04000000, 0x04000000);
    cdj_open_bus(sysmem, "sh7263.cs2", 0x08000000, 0x04000000);
    cdj_open_bus(sysmem, "sh7263.cs3-rest", 0x0E000000, 0x02000000);

    info_report("%s: boot through the bootloader in the NOR flash, "
                "entry 0x%08x, sp 0x%08x",
                cdj850_board.name, cdj850_board.fw_entry,
                cdj850_board.init_sp);
    qemu_register_reset(cdj850_reset, cpu);
}

static void cdj850_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-850 (Renesas SH7263 / SH-2A), bring-up";
    mc->init = cdj850_init;
    mc->default_cpu_type = TYPE_SH2A_CPU;
    mc->default_ram_size = 32 * MiB;
}

DEFINE_MACHINE("cdj850", cdj850_machine_init)
