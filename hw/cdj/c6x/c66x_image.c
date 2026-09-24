/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "c66x_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

int c66x_image_load(c66x_image *img, const char *path, size_t offset)
{
    memset(img, 0, sizeof(*img));
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    img->len = ftell(f);
    fseek(f, 0, SEEK_SET);
    img->buf = malloc(img->len);
    if (!img->buf || fread(img->buf, 1, img->len, f) != img->len) {
        fclose(f);
        c66x_image_free(img);
        return -1;
    }
    fclose(f);

    size_t p = offset;
    if (p + 4 > img->len)
        goto bad;
    img->entry = rd32(img->buf + p);
    p += 4;
    for (;;) {
        if (p + 4 > img->len)
            goto bad;
        uint32_t size = rd32(img->buf + p);
        p += 4;
        if (size == 0)
            break;
        if (p + 4 + size > img->len || img->nsections == 64)
            goto bad;
        c66x_section *s = &img->sections[img->nsections++];
        s->addr = rd32(img->buf + p);
        s->size = size;
        s->data = img->buf + p + 4;
        p += 4 + ((size + 3) & ~3u);
    }
    return 0;
bad:
    c66x_image_free(img);
    return -1;
}

void c66x_image_free(c66x_image *img)
{
    free(img->buf);
    memset(img, 0, sizeof(*img));
}
