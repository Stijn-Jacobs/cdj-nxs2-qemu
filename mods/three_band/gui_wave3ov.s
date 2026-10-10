! SPDX-License-Identifier: GPL-2.0-or-later
!
! 3-band overview strip for the CDJ-2000NXS2 display firmware (SH7269, SH-2A,
! big-endian). The strip renderer loops over 600 six-byte column records from
! the colour-preview pool and starts each column by decoding the record's
! fifth and sixth bytes into two heights. patch_gui.py replaces that decode
! with a call here.
!
! The main board's 3-band mod rewrites the records as
!
!     +0 total rows   +1 rows of mid and below   +2 rows of low
!     +3 0x3B         +4..+5 0xFFFF
!
! A stock record is never taken for one of these: 0xFFFF at +4 would read as a
! 63-row bar, past the 40 rows the renderer clamps to. The low four bits of
! that halfword are the phrase mod's, so they are ignored in the test.
!
! A 3-band record is painted here, bottom-aligned like the stock fill: the low
! rows blue (0055E1), the mid rows amber (FFA600) and the rest white, all as
! RGB565. The renderer is then told the column is empty (r2 = 0), so it skips
! its own fill and goes to the end of the column, where the phrase band and the
! played/unplayed shading are applied as for any other column.
!
! Any other record gets the decode this call replaced, unchanged.
!
! In:  r5 = the column's record, r6 = -4, r8 = the column, 0..599,
!      @r15 = the canvas row length in pixels, @(4,r15) = the canvas
!      (read off the renderer's frame: the call does not move r15).
! r0-r7 are clobbered; nothing else is touched.

        .text
        .global wave3ov
wave3ov:
        mov.w   @(4,r5),r0
        or      #0x0f,r0
        cmp/eq  #-1,r0
        bf      stock
        mov.b   @(3,r5),r0
        cmp/eq  #0x3b,r0
        bf      stock

        mov.l   @r15,r6
        shll    r6                      ! r6 = one canvas row in bytes
        mov     r6,r3
        mov     #39,r0
        mulr    r0,r3                   ! the bottom row is row 39 of 40
        mov.l   @(4,r15),r4
        add     r3,r4
        mov     r8,r0
        shll    r0
        add     r0,r4                   ! r4 = the column's bottom pixel
        neg     r6,r6                   ! r6 = one row up

        mov.b   @r5,r1
        mov.b   @(1,r5),r0
        mov     r0,r2
        mov.b   @(2,r5),r0
        mov     r0,r3
        sub     r2,r1                   ! r1 = white rows
        sub     r3,r2                   ! r2 = amber rows, r3 = blue rows

        movi20  #0x02bc,r7
        tst     r3,r3
        bt      2f
1:      mov.w   r7,@r4
        dt      r3
        bf/s    1b
        add     r6,r4
2:      movi20  #0xfd20,r7
        tst     r2,r2
        bt      4f
3:      mov.w   r7,@r4
        dt      r2
        bf/s    3b
        add     r6,r4
4:      movi20  #0xffff,r7
        tst     r1,r1
        bt      6f
5:      mov.w   r7,@r4
        dt      r1
        bf/s    5b
        add     r6,r4
6:      mov     #0,r2
        rts
        nop

stock:
        movu.b  @(4,r5),r7
        movu.w  @(4,r5),r0
        shlr2   r7
        shld    r6,r0
        extu.b  r7,r2
        rts
        and     #63,r0
