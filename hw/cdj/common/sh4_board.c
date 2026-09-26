/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_getenv.h"
/*
 * The boot half of a CDJ MAIN board: CPU, DRAM, NOR flash, the SH-4 core
 * blocks, and the firmware image. The decompressed MAIN image is loaded at
 * the DRAM base and entered where the bootloader enters it, with the SP the
 * bootloader leaves; the numbers come from the board's CdjBoardDesc.
 */

const CdjBoardDesc *cdj_board;
unsigned cdj_reset_count;

typedef struct CdjResetData {
    SuperHCPU *cpu;
    uint32_t pc;
    uint32_t sp;
} CdjResetData;

static void cdj_cpu_reset(void *opaque)
{
    CdjResetData *s = opaque;
    CPUSH4State *env = &s->cpu->env;

    /* A reset looks like a firmware loop from the PC alone; log it. */
    if (++cdj_reset_count > 1) {
        qemu_log("MACHINE RESET #%u (pc was 0x%08x)\n",
                 cdj_reset_count, env->pc);
    }
    cpu_reset(CPU(s->cpu));
    /* Every bank register is zero after the reset, so changing RB here needs
     * no bank swap. */
    if (cdj_board->init_sr) {
        cpu_write_sr(env, cdj_board->init_sr);
    }
    env->pc = s->pc;
    env->gregs[15] = s->sp;
}

/* Map a device region at both its architectural address and its A7 alias, so it
 * is reachable whichever segment the firmware uses. */
void cdj_unimp(const char *name, hwaddr addr, hwaddr size)
{
    create_unimplemented_device(name, addr, size);
    if (A7ADDR(addr) != addr) {
        g_autofree char *alias = g_strdup_printf("%s-a7", name);
        create_unimplemented_device(alias, A7ADDR(addr), size);
    }
}

static void cdj_board_ram(MemoryRegion *sysmem, const CdjRamRegion *r)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    memory_region_init_ram(mr, NULL, r->name, r->size, &error_fatal);
    memory_region_add_subregion(sysmem, r->phys, mr);
    if (r->env) {
        info_report("%s: %s backed with %u MB RAM at 0x%08x", cdj_board->name,
                    r->what, (unsigned)(r->size / MiB), (unsigned)r->phys);
    }
}

/* NOR flash as a CFI device, not RAM: the firmware programs its settings
 * sectors at runtime with the AMD command set (0xAA/0x55 unlock, 0xA0
 * program, then DQ7/DQ5 polling). The 0x55 goes to byte 0x554, i.e. word
 * 0x2AA on a 16-bit bus. The sector layout matters: with a uniform layout,
 * erasing one small top-boot sector wipes its neighbours. No backing drive
 * means erased (0xFF). CDJ_FLASH_UNIFORM=1 selects 64 KiB sectors throughout.
 */
static void cdj_board_flash(void)
{
    const CdjBoardDesc *d = cdj_board;
    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);
    DeviceState *fl = qdev_new(TYPE_PFLASH_CFI02);
    bool uniform = getenv("CDJ_FLASH_UNIFORM") != NULL;

    if (dinfo) {
        qdev_prop_set_drive(fl, "drive", blk_by_legacy_dinfo(dinfo));
    }
    if (uniform) {
        qdev_prop_set_uint32(fl, "num-blocks", d->flash_size / (64 * KiB));
        qdev_prop_set_uint32(fl, "sector-length", 64 * KiB);
    } else {
        qdev_prop_set_uint32(fl, "num-blocks0", d->flash[0].blocks);
        qdev_prop_set_uint32(fl, "sector-length0", d->flash[0].sector);
        qdev_prop_set_uint32(fl, "num-blocks1", d->flash[1].blocks);
        qdev_prop_set_uint32(fl, "sector-length1", d->flash[1].sector);
    }
    qdev_prop_set_uint8(fl, "width", 2);
    qdev_prop_set_uint8(fl, "mappings", 1);
    qdev_prop_set_uint8(fl, "big-endian", 0);
    qdev_prop_set_uint16(fl, "id0", d->flash_id[0]);
    qdev_prop_set_uint16(fl, "id1", d->flash_id[1]);
    qdev_prop_set_uint16(fl, "id2", d->flash_id[2]);
    qdev_prop_set_uint16(fl, "id3", d->flash_id[3]);
    qdev_prop_set_uint16(fl, "unlock-addr0", 0x555);
    qdev_prop_set_uint16(fl, "unlock-addr1", 0x2aa);
    qdev_prop_set_string(fl, "name", "cdj.flash");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(fl), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(fl), 0, d->flash_phys);
    if (uniform) {
        info_report("%s: NOR flash uniform %u x 64 KiB (CDJ_FLASH_UNIFORM)",
                    d->name, (unsigned)(d->flash_size / (64 * KiB)));
    } else {
        info_report("%s: NOR flash top-boot %u x %u KiB + %u x %u KiB",
                    d->name, d->flash[0].blocks, d->flash[0].sector / KiB,
                    d->flash[1].blocks, d->flash[1].sector / KiB);
    }
}

SuperHCPU *cdj_board_init(MachineState *machine, const CdjBoardDesc *desc)
{
    MemoryRegion *sysmem = get_system_memory();
    SuperHCPU *cpu;
    unsigned i;

    cdj_board = desc;
    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    cdj_board_ram(sysmem, &(CdjRamRegion) {
        .name = "cdj.dram", .phys = desc->dram_phys, .size = desc->dram_size,
    });
    for (i = 0; i < desc->n_extra_ram; i++) {
        const CdjRamRegion *r = &desc->extra_ram[i];

        if (!r->env || getenv(r->env)) {
            cdj_board_ram(sysmem, r);
        }
    }
    cdj_board_flash();

    /* CCN, the SH-4 core control block (0xFF000000..0xFF000028). QEMU only
     * provides it through the SH7750 SoC model, which these boards do not
     * use; without it the firmware spins re-reading EXPEVT. */
    cdj_ccn_init(sysmem, cpu);
    cdj_unimp("sh4.ubc", 0xFF200000, 0x1000);
    return cpu;
}

void cdj_board_load(MachineState *machine)
{
    const CdjBoardDesc *d = cdj_board;
    ssize_t fwsize;

    if (!machine->kernel_filename) {
        error_report("%s: pass the decompressed MAIN image with "
                     "-kernel main_unpacked.bin", d->name);
        exit(1);
    }

    /* Flat binary, not an ELF. */
    fwsize = load_image_targphys(machine->kernel_filename,
                                 d->dram_phys, d->dram_size);
    if (fwsize < 0) {
        error_report("%s: cannot load '%s'", d->name, machine->kernel_filename);
        exit(1);
    }
    info_report("%s: %zd bytes at 0x%08x, entry 0x%08x, sp 0x%08x", d->name,
                fwsize, (unsigned)d->dram_phys, d->fw_entry, d->init_sp);

    /* Done after the image load so the image cannot overwrite it. */
    if (d->sr_seed_slot) {
        uint32_t sr_seed = cpu_to_le32(d->sr_seed);

        cpu_physical_memory_write(d->sr_seed_slot, &sr_seed, sizeof(sr_seed));
    }
}

/* PC/SP are applied on reset; setting env->pc directly would be overwritten
 * by the CPU's own reset. */
void cdj_board_start(SuperHCPU *cpu)
{
    CdjResetData *reset_info = g_new0(CdjResetData, 1);

    reset_info->cpu = cpu;
    reset_info->pc = cdj_board->fw_entry;
    reset_info->sp = cdj_board->init_sp;
    qemu_register_reset(cdj_cpu_reset, reset_info);
}
