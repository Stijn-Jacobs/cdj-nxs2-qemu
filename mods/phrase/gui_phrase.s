! SPDX-License-Identifier: GPL-2.0-or-later
!
! Redesigned per instruction: phrases are a separate coloured band at the
! BOTTOM of the overview strip, not a recolour of the waveform's own columns.
! The earlier version of this file replaced the strip renderer's colour
! lookup (see git history) and is superseded -- that hook is not used here at
! all, so the waveform's own fill colours are never touched by this mod.
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
! with no phrase id (table byte 0) paints nothing -- the waveform underneath
! shows through unchanged, which is also what a track with no phrase
! analysis renders as MAIN never writes anything but zero to the table then.
!
! The canvas destination pointer ([r15+4]) and the row pitch, already halved
! ([r15+0]), are computed once before the per-column loop and held in these
! same stack slots for the whole function, at every fill style and at both
! the old and the new hook -- confirmed by reading the renderer's own pixel
! address formula, used unchanged below: addr = (0x27 - row_from_bottom) *
! pitch * 2 + dest + column * 2. Row 0x27 is the canvas's own bottom row (the
! widget is 0x28 = 40 rows tall); this mod fills the bottom 6.
!
! The phrase table is one byte per column, held in this routine's OWN
! reserved space (label phrasetab below) rather than in the type-0x35
! column-record pool at 0x0E5BB45C. An earlier version of this file put it
! at pool-base+0x1000, on the assumption that the pool was a ~16 KB buffer
! with only its first 3,616 bytes in use -- wrong: a data-address census
! (Ghidra reference model, cdjgui) shows a live buffer at 0x0E5BC26C, read
! by gui_draw_layer_upload and by several functions in the same address
! range as the centre-waveform renderer (0x1C009xxx-0x1C00Axxx), only 0x1F0
! bytes past pool-base+0x1000 -- the real free gap after the column records
! is at most ~3,600 bytes, already smaller than what the column records
! alone use, i.e. there was no spare room there at all. Writing a phrase
! table at pool-base+0x1000 landed inside that buffer, which is why the
! centre waveform rendered as a flat blue/white fill with the mod on.
! Fix: `place_segment()` already
! proves the routine's own segment is free (an erased-flash scan, checked
! against every load-table entry); reserving the table there needs no new
! address to trust. It also comes pre-zeroed by the normal load-table copy,
! so a track with no phrase analysis (or the very first boot) reads all
! zero without MAIN having to write it explicitly. Byte 0 = no phrase data
! for this column (paint nothing); 1-8 select one of the eight colours below.
!
! In:  r8 = the just-finished column index, 0..599.
! r15 stack layout ([r15+0] pitch, [r15+4] dest, [r15+8] remaining count) and
! r2/r8 are the only things read after this point by the surviving code;
! everything else (r0,r1,r6,r7,r9,r10) is free to clobber.
!
! Verified against v1.81 only, like the other mods in this file's registry:
! the two resume addresses below are this build's own, read here as fixed
! values for the same reason the other mods in this file pin a firmware
! version. phrasetab is this file's own label, not a firmware address, so it
! needs no such pin -- sigpatch places it wherever the registry's other mods
! leave room, in this build or any other.

        .text
        .global phrase
phrase:
        mova    phrasetab,r0
        mov     r0,r9
        mov     r8,r0
        mov.b   @(r0,r9),r6
        extu.b  r6,r6
        tst     r6,r6
        bt      noband
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
loopback:   .long 0x1C007B82        ! more columns left: back to the loop top
markers:    .long 0x1C007DE6        ! loop done: into the renderer's own tick/marker pass

! Eight colours, Rekordbox's own phrase/hot-cue palette (github.com/mganss/
! CueGen, MIT licensed, ColorTable.cs's 8-entry Colors[] and Analysis/
! PhraseEntry.cs's mood-to-colour-index tables -- no official Pioneer/
! AlphaTheta source for the CDJ-3000's own phrase band colours could be
! found; this is Rekordbox's colour for the phrase, which is what the PSSI
! data actually carries). Converted from that table's 8-bit RGB to RGB565.
! MAIN is expected to write one of these eight indices (1-8, 0 = none) per
! column, already resolved from the phrase's kind and the track's mood.
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

! This mod's own phrase table: one byte per column, 600 columns, reserved
! here instead of in the firmware's own RAM (see the header comment for why).
! Zero-filled in the assembled blob, so a fresh load-table copy always
! starts all columns at "no phrase data" with no MAIN write required. MAIN's
! future fetch+reduce patch writes
! here at whatever address this build placed it -- print it with
! `patch_gui.py --list` once that patch exists, the same way this file's
! own hook address is never assumed, only found.
        .p2align 2
phrasetab:  .skip 600, 0
