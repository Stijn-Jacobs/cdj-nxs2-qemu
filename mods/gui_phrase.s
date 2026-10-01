! SPDX-License-Identifier: GPL-2.0-or-later
!
! FIXED: the first version of
! this mod replaced instructions 0x1C007BB8-0x1C007C26, which includes the
! function's own literal pool (0x1C007BDE onward) -- a shared pool that
! OTHER instructions outside that span, including the record-base load at
! 0x1C007B88 that runs before this call on every one of the renderer's 600
! iterations, read via their own mov.l @(disp,pc). Overwriting the pool with
! this routine's call trampoline corrupted every one of those loads with no
! fault (a wild pointer, not an illegal instruction), which is why the
! symptom was a silent load stall, not a crash. Fix: replace only the code
! before the pool (0x1C007BB8-0x1C007BDC, the mode check and the two colour
! branches) and never touch a byte from 0x1C007BDE on. Since sigpatch ties
! the call site's own auto-appended "return here" address to the end of the
! replaced span, and that span now ends AT the pool (not at the real resume
! point after it, 0x1C007C26), this routine does not "rts" back into that
! dead tail -- it jumps to 0x1C007C26 directly instead, the same way the
! replaced code's own two branches did. A second, separate bug found while
! fixing the first: this routine's own colour-table lookup originally went
! through ".long phrase_base" pointer slots, which assemble to an
! unresolved relocation (patch_gui.py's --assemble never links), not this
! blob's placed address -- fixed below with mova. Re-verified after both
! fixes: 12/12 interleaved rig runs loaded cleanly (none/phrase/both, 4
! reps each), matching the no-mods control rate, and a screenshot with a
! synthetic phrase table shows the override colouring the strip correctly.
!
! Phrase-colour overlay for the CDJ-2000NXS2's overview waveform strip
! (SH7269, SH-2A, big-endian). patch_gui.py splices this in place of the
! colour-preview renderer's own colour lookup (the if/else that picks the
! two 16-bit colours for one strip column, in the palette-index or the
! direct-RGB565 record shape) and lets it fall through unchanged unless a
! phrase id has been written for this column.
!
! The renderer already reduces every track to 600 columns, one screen pixel
! each (x 100..700, the strip's own width). The type-0x35 link message fills
! only the first 3,616 of the ~16 KB pool this routine's column record lives
! in (0x0E5BB45C), so a 600-byte table of one phrase id per column fits in
! the same, already-allocated pool with room to spare, at a fixed offset
! past the column data. A MAIN-side patch that fetches and reduces PSSI
! writes that table before column 0 is ever drawn; until it does (or on a
! track with no phrase analysis), every byte reads 0 and every column
! renders exactly as it always has.
!
! In:  r5 = this column's 6-byte record (0x0E5BB45C + column*6),
!      r8 = column index, 0..599.
! Out: r1 = the record's base colour, r4 = its accent colour -- the same
!      two outputs the replaced code left in these registers.
! r2 and r11 (the column's clamped height and width, set before this call)
! are live across it and are not touched. Clobbers r0, r6, r7, r9, r10.
!
! Verified against v1.81 only, like the other mods in this file's registry:
! the addresses below are this build's own pool constants (mode flag,
! palette tables), read here as fixed values because the replaced code
! loads three different ones and the patcher's own literal-patch mechanism
! only carries a single value.

        .text
        .global phrase
phrase:
        mov.l   modeflag,r6
        mov.l   @r6,r6
        tst     r6,r6
        bt      direct
palette:
        mov.b   @(5,r5),r0
        mov     r0,r6
        extu.b  r0,r0
        shlr    r0
        and     #7,r0
        shll    r0
        mov.l   tab1,r9
        mov.w   @(r0,r9),r1
        extu.b  r6,r0
        shlr    r0
        and     #7,r0
        shll    r0
        mov.l   tab2,r10
        mov.w   @(r0,r10),r4
        bra     override
        nop
direct:
        mov.w   @(2,r5),r0
        mov.w   @r5,r1
        mov     r0,r4
override:
        mov     r8,r0
        mov.l   phrasetab,r9
        mov.b   @(r0,r9),r6
        extu.b  r6,r6
        tst     r6,r6
        bt      done
        add     #-1,r6
        ! mova, not a .long pointer slot: patch_gui.py's --assemble goes
        ! straight from .o to raw binary with no link step, so a .long
        ! naming a same-file label would carry an unresolved relocation
        ! (its raw section-relative offset, not this blob's placed address,
        ! silently -- caught by checking the assembled bytes by hand). mova
        ! computes the label's address purely from the current PC, so it is
        ! correct wherever sigpatch places this blob.
        mova    phrase_base,r0
        mov     r0,r9
        mov     r6,r0
        shll    r0
        mov.w   @(r0,r9),r1
        mova    phrase_accent,r0
        mov     r0,r10
        mov.w   @(r0,r10),r4
done:
        mov.l   cont,r0
        jmp     @r0
        nop

        .p2align 2
modeflag:   .long 0x0E592D84        ! direct-vs-palette record flag
tab1:       .long 0x1C041BA4        ! palette-mode base colour table
tab2:       .long 0x1C041BB4        ! palette-mode accent colour table
phrasetab:  .long 0x0E5BC45C        ! this mod's own table, one byte/column,
                                    ! 0x1000 past the column record base --
                                    ! well inside the 16 KB pool, past the
                                    ! 3,616 bytes the link message fills
cont:       .long 0x1C007C26        ! resume point, past the pool this call
                                    ! site's replaced span stops short of;
                                    ! same v1.81 pin as the constants above

! Seven placeholder phrase colours (id 1..7); id 0 is "no phrase data" and
! never reaches this table. Not Pioneer's own phrase-type palette -- nobody
! has that -- just seven visually distinct hues, base and a darker accent,
! until a real CDJ-3000 side by side gives something to match.
        .p2align 2
phrase_base:
        .word 0x0000
        .word 0xF800            ! 1 red
        .word 0xFC00            ! 2 orange
        .word 0xFFE0            ! 3 yellow
        .word 0x07E0            ! 4 green
        .word 0x001F            ! 5 blue
        .word 0x781F            ! 6 purple
        .word 0xF81F            ! 7 magenta
phrase_accent:
        .word 0x0000
        .word 0x7800
        .word 0x7A00
        .word 0x7BE0
        .word 0x03E0
        .word 0x0010
        .word 0x300F
        .word 0x7800
