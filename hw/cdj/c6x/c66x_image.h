/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The DSP program as MAIN ships it: a bare little-endian TI C6000 boot table
 * (u32 entry; {u32 size, u32 addr, data padded to 4}*; u32 0) at offset
 * 0x34E0 of extract/main_unpacked.bin.
 */
#ifndef C66X_IMAGE_H
#define C66X_IMAGE_H

#include <stdint.h>
#include <stddef.h>

typedef struct c66x_section {
    uint32_t addr;
    uint32_t size;
    const uint8_t *data;   /* points into the image buffer */
} c66x_section;

typedef struct c66x_image {
    uint8_t *buf;          /* whole file, owned */
    size_t   len;
    uint32_t entry;
    unsigned nsections;
    c66x_section sections[64];
} c66x_image;

/* Returns 0 on success; the table is validated structurally (in-bounds sizes,
 * zero terminator) and rejected otherwise. */
int  c66x_image_load(c66x_image *img, const char *path, size_t offset);
void c66x_image_free(c66x_image *img);

#endif
