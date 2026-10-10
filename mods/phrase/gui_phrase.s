! SPDX-License-Identifier: GPL-2.0-or-later
!
! Overview-strip phrase band for the CDJ-2000NXS2's colour-preview renderer
! (SH7269, SH-2A, big-endian), FUN_1C007B22. The renderer loops 600 times,
! once per screen column, and every fill style it can take for that column
! rejoins at one instruction pair before the column counter is bumped:
!
!     mov.l @(8,r15),r2      ! remaining-column counter
!     add   #1,r8            ! r8: the column just finished, 0..599
!
! patch_gui.py replaces that pair (plus enough surrounding words for a unique
! match) with a call here. This runs once per column, after that column's own
! waveform pixels are already painted, so a band drawn here sits on top of
! the waveform rather than being overwritten by a tall sample, and a column
! with no phrase id paints nothing and the waveform underneath shows through
! unchanged.
!
! The canvas destination pointer ([r15+4]) and the row pitch, already halved
! ([r15+0]), are computed once before the per-column loop and held in these
! same stack slots for the whole function, at every fill style -- confirmed
! by reading the renderer's own pixel address formula, used unchanged below:
! addr = (0x27 - row_from_bottom) *
! pitch * 2 + dest + column * 2. Row 0x27 is the canvas's own bottom row (the
! widget is 0x28 = 40 rows tall); this mod fills the bottom 6.
!
! Each column's phrase id rides in its own 0x35 overview record, which the
! main board's phrase mod writes: the low four bits of record byte 5 (the
! renderer reads that byte only as a palette index, and only when its mode
! flag selects the palette; this deck's records are direct RGB565, so the bits
! are unused). 0 = no phrase for this column, 1-8 = a colour of the palette
! below. A track without phrase analysis reads 0 in every column, so nothing
! is painted and the waveform shows through unchanged.
!
! In:  r8 = the just-finished column index, 0..599.
! r15 stack layout ([r15+0] pitch, [r15+4] dest, [r15+8] remaining count) and
! r2/r8 are the only things read after this point by the surviving code;
! everything else (r0,r1,r6,r7,r9,r10) is free to clobber.
!
! Verified against v1.81 only, like the other mods in this file's registry:
! the two resume addresses and the record pool below are this build's own.

        .text
        .global phrase
phrase:
        mov.l   records,r9
        mov     r8,r6
        shll    r6
        mov     r6,r7
        shll    r7
        add     r7,r6                   ! r6 = column * 6
        add     r6,r9                   ! r9 = this column's record
        mov.b   @(5,r9),r0
        and     #0x0f,r0
        tst     r0,r0
        bt      noband
        mov     r0,r6
        add     #-1,r6                  ! table value 1-8 -> palette index 0-7

        mova    palette,r0
        mov     r0,r9
        mov     r6,r0
        shll    r0
        mov.w   @(r0,r9),r1             ! r1 = this phrase's band colour

        mov.l   @(4,r15),r9             ! r9 = canvas dest (stable all loop)
        mov     r8,r0
        shll    r0                      ! r0 = column * 2 (byte offset)
        add     r9,r0
        mov     r0,r10                  ! r10 = dest + this column's byte offset

        mov     #6,r7                   ! band height: bottom 6 of the 40 rows
band:
        mov     r7,r6
        add     #-1,r6                  ! r6 = row_from_bottom, 5..0
        mov     #0x27,r9
        sub     r6,r9                   ! r9 = row index, 0x22..0x27
        mov.l   @r15,r0                 ! r0 = row pitch, already halved
        mulr    r0,r9                   ! r9 = pitch * row index
        shll    r9                      ! r9 *= 2
        add     r10,r9
        mov.w   r1,@r9
        dt      r7
        bf      band
noband:
        ! the two instructions this call site replaces, unchanged
        mov.l   @(8,r15),r2
        add     #1,r8
        ! ...and the branch they lead into, reproduced here because sigpatch's
        ! own auto-appended "resume" cannot express two different targets:
        ! this jumps to whichever the original bt/bra pair would have picked
        add     #-1,r2
        tst     r2,r2
        bt      finished
        mov.l   r2,@(8,r15)
        mov.l   loopback,r0
        jmp     @r0
        nop
finished:
        mov.l   r2,@(8,r15)
        mov.l   markers,r0
        jmp     @r0
        nop

        .p2align 2
records:    .long 0x0E5BB45C        ! the 600 six-byte overview records the 0x35 message fills
loopback:   .long 0x1C007B82        ! more columns left: back to the loop top
markers:    .long 0x1C007DE6        ! loop done: into the renderer's own tick/marker pass

! Eight colours, Rekordbox's own phrase/hot-cue palette (github.com/mganss/
! CueGen, MIT licensed, ColorTable.cs's 8-entry Colors[] and Analysis/
! PhraseEntry.cs's mood-to-colour-index tables -- no official Pioneer/
! AlphaTheta source for the CDJ-3000's own phrase band colours could be
! found; this is Rekordbox's colour for the phrase, which is what the PSSI
! data actually carries). Converted from that table's 8-bit RGB to RGB565.
! The main board writes the index (1-8, 0 = none) already resolved from the
! phrase's kind and the track's mood.
        .p2align 2
palette:
        .word 0xFB9F            ! 1 magenta  (248,112,248)
        .word 0xF800            ! 2 red      (248,0,0)
        .word 0xFD06            ! 3 orange   (248,163,48)
        .word 0xFF06            ! 4 yellow   (248,227,48)
        .word 0x0700            ! 5 green    (0,224,0)
        .word 0x061F            ! 6 cyan     (0,192,248)
        .word 0x029F            ! 7 blue     (0,80,248)
        .word 0x985F            ! 8 purple   (152,8,248)
