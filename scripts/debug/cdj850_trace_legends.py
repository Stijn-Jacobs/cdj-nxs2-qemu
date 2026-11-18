#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reconstruct static FL legends from the user-supplied all-on photograph.

Requires Pillow only for this offline authoring step. Input is IMG_2295 after
HEIC->PNG conversion at its original 4032x1610 resolution. The output contains
monochrome letter coverage, not photographic backgrounds or firmware assets.
"""
import argparse
from collections import deque
from pathlib import Path
from PIL import Image

# Crop coordinates on a 2000px-wide preview; original pixels are scaled below.
# Bounds exclude illuminated frames. Widths are in the 1280px renderer canvas.
LEGENDS = [
    ("REMAIN", (258,105,481,150), 144, False),
    ("INFO", (106,545,202,598), 56, False),
    ("BROWSE", (81,325,226,373), 94, False),
    ("TAG", (111,420,202,460), 48, False),
    ("LIST", (106,461,203,501), 52, False),
    ("A.CUE", (280,180,470,226), 126, True),
    ("MT", (1710,305,1825,352), 74, True),
    ("WIDE", (1705,380,1840,440), 88, True),
    ("MEMORY", (88,641,214,674), 78, True),
    ("CUE", (111,697,193,729), 54, False),
    ("BPM", (1710,246,1845,280), 80, False),
    ("M", (674,209,714,242), 20, False),
    ("S", (914,209,945,242), 16, False),
    ("F", (1142,209,1173,242), 16, False),
]


def trace(image, bounds, width, red, name):
    k = image.width / 2000
    crop = image.crop(tuple(round(v*k) for v in bounds))
    channel = crop.getchannel("R" if red else "G")
    binary = channel.point(lambda v: 255 if v >= 165 else 0)
    # Remove isolated reflection/noise specks while retaining letter strokes.
    pixels = binary.load()
    seen = set()
    for y in range(binary.height):
        for x in range(binary.width):
            if not pixels[x,y] or (x,y) in seen:
                continue
            component = []; pending = deque([(x,y)]); seen.add((x,y))
            while pending:
                px,py = pending.popleft(); component.append((px,py))
                for nx,ny in ((px-1,py),(px+1,py),(px,py-1),(px,py+1)):
                    if (0 <= nx < binary.width and 0 <= ny < binary.height and
                            pixels[nx,ny] and (nx,ny) not in seen):
                        seen.add((nx,ny)); pending.append((nx,ny))
            frame_corner = (name == "MEMORY" and
                            min(px for px,py in component) > binary.width * .8 and
                            max(py for px,py in component) - min(py for px,py in component)
                            < binary.height * .5)
            if len(component) < 12 or frame_corner:
                for px,py in component: pixels[px,py] = 0
    box = binary.getbbox()
    if not box:
        raise ValueError("empty legend")
    binary = binary.crop(box)
    height = round(binary.height * width / binary.width)
    if width > 160 or height > 40:
        raise ValueError("legend exceeds renderer mask bounds")
    mask = binary.resize((width,height), Image.Resampling.LANCZOS)
    # Eight coverage levels keep edges smooth with compact, stable runs.
    mask = mask.point(lambda v: round(v/255*7)*255//7)
    runs = []
    for y in range(height):
        x = 0
        while x < width:
            value = mask.getpixel((x,y)); end = x+1
            while end < width and mask.getpixel((end,y)) == value: end += 1
            if value: runs.extend((y,x,end-x,value))
            x = end
    return mask, runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("photo", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    image = Image.open(args.photo).convert("RGB")
    header = ['/* SPDX-License-Identifier: GPL-2.0-or-later */',
              '/* Static glass legends traced from user photo IMG_2295.',
              ' * Regenerate with scripts/debug/cdj850_trace_legends.py.',
              ' * Values are y, x, run length, coverage; no RGB photo pixels. */',
              '#ifndef CDJ850_VFD_LEGENDS_H', '#define CDJ850_VFD_LEGENDS_H',
              '#include <stdint.h>',
              'typedef struct FlLegendMask {',
              '    const char *text;',
              '    uint8_t width, height;',
              '    uint16_t count;',
              '    const uint8_t *runs;',
              '} FlLegendMask;']
    entries = []
    for i,(name,bounds,width,red) in enumerate(LEGENDS):
        mask,runs = trace(image,bounds,width,red,name)
        if args.preview_dir:
            args.preview_dir.mkdir(parents=True, exist_ok=True)
            mask.save(args.preview_dir / (name.replace('.', '_')+'.png'))
        header.append(f'static const uint8_t fl_legend_runs_{i}[] = {{')
        for pos in range(0,len(runs),24):
            header.append('    '+','.join(map(str,runs[pos:pos+24]))+',')
        header.append('};')
        entries.append(f'    {{"{name}",{mask.width},{mask.height},{len(runs)//4},fl_legend_runs_{i}}},')
    header.extend(['static const FlLegendMask fl_legend_masks[] = {', *entries,
                   '};', '#endif', ''])
    args.output.write_text('\n'.join(header))
    print(args.output)


if __name__ == '__main__':
    main()
