! SPDX-License-Identifier: GPL-2.0-or-later
!
! 3-band centre waveform for the CDJ-2000NXS2 display firmware (SH7269, SH-2A,
! big-endian). patch_gui.py places this routine in unused on-chip RAM and
! replaces the RGB column fill of the centre-waveform renderer with a call to
! it. The renderer keeps one 16-bit word per waveform entry; the stock word is
! RGB (red 15-13, green 12-10, blue 9-7, height 6-2, bits 1-0 always clear).
! The main board's 3-band mod rewrites the entries in place as
!
!     high5 << 11 | mid5 << 6 | low5 << 1 | 1
!
! so bit 0 tells the two apart. This draws one screen column:
!
!   bit 0 clear   the stock fill, unchanged: the word's own RGB colour, as
!                 many rows up from row 70 as the renderer's height.
!   bit 0 set     the 3-band column. Each band's height is the largest of
!                 its field over the column's entry group, 3 * field / 2 rows
!                 and at least one (the stock height rule, so a full band is
!                 as tall as a full RGB column). Each row takes its colour
!                 from which bands still reach it: low blue, mid amber, high
!                 white, and a blend where two or three overlap.
!
! The renderer's own peak search is meaningless on 3-band words, so r12 (the
! height it hands on to the cue and loop marks drawn above the column) is set
! to the tallest band.
!
! In:  r4 = screen column, r5 = index of the column's peak entry,
!      r6 = height in rows (1..55), r7 = the entry array,
!      @(40,r15) = first entry of the column's group, @(12,r15) = its length
!      (read off the renderer's frame: the call does not move r15).
! r0-r7 and r12 are clobbered; nothing else is touched.
!
! The last word is the renderer's canvas address. It differs between
! firmware builds, so patch_gui.py fills it in from the instruction this
! call replaces.

        .text
        .global wave3
wave3:
        mov.l   canvas,r2
        movi20  #0x1a400,r0             ! centre row 70 of the 768-pixel canvas
        add     r0,r2
        shll    r4
        add     r2,r4                   ! r4 = the column's centre pixel

        mov     r5,r0
        shll    r0
        mov.w   @(r0,r7),r3
        extu.w  r3,r3                   ! r3 = the peak entry
        mov     r3,r0
        tst     #1,r0
        bt      stock

        mov.l   @(40,r15),r0
        shll    r0
        add     r0,r7
        mov     r7,r5                   ! r5 = first entry of the group
        mov.l   @(12,r15),r2            ! r2 = entries left
        mov     #0,r7                   ! r7 = low, r12 = mid, r6 = high
        mov     #0,r12
        mov     #0,r6
1:      mov.w   @r5+,r3
        extu.w  r3,r3
        mov     r3,r1
        shlr    r1
        mov     r1,r0
        and     #31,r0
        cmp/hi  r7,r0
        bf      2f
        mov     r0,r7
2:      mov     r3,r1
        shlr2   r1
        shlr2   r1
        shlr2   r1
        mov     r1,r0
        and     #31,r0
        cmp/hi  r12,r0
        bf      3f
        mov     r0,r12
3:      mov     r3,r1
        shlr8   r1
        shlr2   r1
        shlr    r1
        cmp/hi  r6,r1
        bf      4f
        mov     r1,r6
4:      dt      r2
        bf      1b

        .macro  bandrows field
        mov     \field,r0
        add     \field,r0
        add     \field,r0
        shlr    r0
        tst     r0,r0                   ! a silent band still shows one row
        movt    r1
        or      r1,r0
        mov     r0,\field
        .endm
        bandrows r7
        bandrows r12
        bandrows r6

        mov     r7,r1                   ! r1 = the tallest band
        cmp/hs  r12,r1
        bt      5f
        mov     r12,r1
5:      cmp/hs  r6,r1
        bt      6f
        mov     r6,r1
6:      mov     #-6,r3
        shll8   r3                      ! r3 = one row up
        mov     #0,r5                   ! r5 = row, counted up from the centre
7:      mov     #0,r2                   ! r2 = which bands reach this row
        cmp/gt  r5,r7
        rotcl   r2
        cmp/gt  r5,r12
        rotcl   r2
        cmp/gt  r5,r6
        rotcl   r2
        shll    r2
        mova    bands,r0
        mov.w   @(r0,r2),r2
        mov.w   r2,@r4
        add     #1,r5
        cmp/eq  r5,r1
        bf/s    7b
        add     r3,r4
        rts
        mov     r1,r12

stock:
        mov     r3,r1
        shlr2   r1
        movi20  #0x700,r0
        and     r0,r1
        mov     r3,r2
        shlr2   r2
        shlr2   r2
        shlr    r2
        mov     r2,r0
        and     #28,r0
        add     r0,r1
        movi20  #0xe000,r0
        and     r3,r0
        add     r0,r1                   ! r1 = the word's own colour
        mov     #-6,r3
        shll8   r3
8:      mov.w   r1,@r4
        dt      r6
        bf/s    8b
        add     r3,r4
        rts
        nop

! RGB565 colour by which bands reach the row (low, mid, high as bits 2..0).
! Low blue 0055E1, mid amber FFA600, high white FFFFFF; the overlaps are the
! blends of the 3-band reference (unused entry 0: no row is reached by none).
        .p2align 2
bands:  .word   0x0000, 0xffff, 0xfd20, 0xff9a, 0x02bc, 0xd6ff, 0xb341, 0xf75a

        .p2align 2
canvas: .long   0
