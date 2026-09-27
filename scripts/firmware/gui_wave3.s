! SPDX-License-Identifier: GPL-2.0-or-later
!
! 3-band centre waveform for the CDJ-2000NXS2 display firmware (SH7269, SH-2A,
! big-endian). patch_gui.py places this routine in unused on-chip RAM and
! replaces the RGB column fill of the centre-waveform renderer with a call to
! it. It draws one screen column as three overlaid bands, low (blue), mid
! (amber) and high (white), like the CDJ-3000's 3BAND waveform: the tallest
! band at the back and each shorter one in front of it.
!
! The NXS2 only ever receives the RGB detail waveform (PWV5): one 16-bit word
! per column, red 15-13, green 12-10, blue 9-7, height 6-2. Red follows the
! low band, green the mid and blue the high: fitted against rekordbox's own
! 3-band data (PWV7) of the same tracks, these weights give each band's
! height with a correlation of 0.93 / 0.85 / 0.85.
!
!   low  = 4 * (red + 4)   mid = 4 * (green + 3)   high = 3 * (blue + 1)
!
! The tallest band is drawn at the firmware's own height for the column, so
! the outline, and whatever the renderer draws above it (cue and loop marks),
! stay exactly where the RGB waveform had them.
!
! In:  r4 = screen column, r5 = sample index of the column's peak,
!      r6 = height in pixels (1..55), r7 = the PWV5 sample array.
! r0-r7 are clobbered; nothing else is touched (r8 is saved on the stack).
!
! The last word is the renderer's canvas address. It differs between
! firmware builds, so patch_gui.py fills it in from the instruction this
! call replaces.

        .text
        .global wave3
wave3:
        mov     r5,r0
        shll    r0
        mov.w   @(r0,r7),r1
        extu.w  r1,r1                   ! r1 = PWV5 word

        mov.l   canvas,r2
        movi20  #0x1a400,r0             ! centre row 70 of the 768-pixel canvas
        add     r0,r2
        shll    r4
        add     r2,r4                   ! r4 = the column's centre pixel

        mov     r1,r0
        shlr8   r0
        shlr2   r0
        shlr2   r0
        shlr    r0                      ! red
        add     #4,r0
        shll2   r0
        mov     r0,r5                   ! r5 = low weight

        mov     r1,r0
        shlr8   r0
        shlr2   r0
        and     #7,r0                   ! green
        add     #3,r0
        shll2   r0
        mov     r0,r7                   ! r7 = mid weight

        mov     r1,r0
        shlr2   r0
        shlr2   r0
        shlr2   r0
        shlr    r0
        and     #7,r0                   ! blue
        add     #1,r0
        mov     r0,r3
        add     r0,r3
        add     r0,r3                   ! r3 = high weight

        mov     r5,r2                   ! r2 = the largest weight
        cmp/hs  r7,r2
        bt      1f
        mov     r7,r2
1:      cmp/hs  r3,r2
        bt      2f
        mov     r3,r2
2:
        mov     r6,r0                   ! each band's height = height * weight / largest
        mulr    r0,r5
        mulr    r0,r7
        mulr    r0,r3
        mov     r2,r0
        divu    r0,r5
        divu    r0,r7
        divu    r0,r3

        movi20  #0x229c,r1              ! low: blue
        movi20  #0xf506,r2              ! mid: amber
        movi20  #0xf79e,r6              ! high: white

        ! Tallest band first, so each shorter one stays visible in front.
        cmp/hs  r7,r5
        bt      1f
        mov     r5,r0                   ! swap the first two bands
        mov     r7,r5
        mov     r0,r7
        mov     r1,r0
        mov     r2,r1
        mov     r0,r2
1:      cmp/hs  r3,r7
        bt      2f
        mov     r7,r0
        mov     r3,r7
        mov     r0,r3
        mov     r2,r0
        mov     r6,r2
        mov     r0,r6
        cmp/hs  r7,r5
        bt      2f
        mov     r5,r0                   ! swap the first two bands
        mov     r7,r5
        mov     r0,r7
        mov     r1,r0
        mov     r2,r1
        mov     r0,r2
2:
        mov.l   r8,@-r15
        mov     #-6,r8
        shll8   r8                      ! r8 = one row up

        mov     r4,r0
        tst     r5,r5
        bt      4f
3:      mov.w   r1,@r0
        dt      r5
        bf/s    3b
        add     r8,r0
4:
        mov     r4,r0
        tst     r7,r7
        bt      6f
5:      mov.w   r2,@r0
        dt      r7
        bf/s    5b
        add     r8,r0
6:
        mov     r4,r0
        tst     r3,r3
        bt      8f
7:      mov.w   r6,@r0
        dt      r3
        bf/s    7b
        add     r8,r0
8:
        rts
        mov.l   @r15+,r8

        .p2align 2
canvas: .long   0
