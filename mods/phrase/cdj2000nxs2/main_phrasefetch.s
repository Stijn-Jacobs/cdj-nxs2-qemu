! SPDX-License-Identifier: GPL-2.0-or-later
!
! Phrase analysis for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): reads the track's PSSI tag from its .EXT and its beat grid
! (PQT2, also in the .EXT), and reduces them to one phrase colour per column of the
! 600-column overview strip, for main_phrasedata.s to write into the overview
! payload.
!
! Hook: the colour-preview reader, right before it reads the PWV4 tag, the same
! site as main_wave3ovfetch.s: r13 is the open ANLZ handle and the saved r5
! (@(20,r4)) the track's hash.
!
! A tag fetch hands back a block whose first word is the tag's length and
! whose tag starts at +4 (4CC, header length, tag length, all big-endian).
!
! PQT2 (the extended beat grid, in the .EXT; the firmware reads it for the
! deck's own grid): from tag+28 the time of the first beat, from tag+36 the
! time of the last and from tag+40 the number of beats, big-endian, ms. The
! beats are taken to be evenly spaced between those two, which is exact for a
! fixed tempo and what a tempo change is approximated by. The strip covers the
! whole track and the grid runs to its end, so the track length is the last
! beat's time plus one beat period (148.834 s against a 148.82 s waveform on
! the track this was written against). Times are kept in 16 ms units.
!
! PSSI: tag+16 the phrase count n, from tag+18 the body, which rekordbox 6 and
! later mask with a 19-byte key (each byte of the public key plus n). The mask
! is there when the body's first halfword, the mood, is above 20. Body: mood
! (1 high, 2 mid, 3 low) @0, the beat the last phrase ends at @8, then n
! 24-byte phrases from @14, each with its first beat @+2 and its kind @+4. A
! phrase runs from its first beat to the next one's, the last to the end beat.
! The kind maps to one of the eight palette colours the display's phrase mod
! knows (mood_high, mood_mid, mood_low below: colour index + 1, 0 = none).
! Columns are filled where the column's centre is inside the phrase.
!
! The table (600 bytes) is handed over through one word at 0x081FFFF0, next to
! the 3-band mod's words below it: -1 before the first load, 0 when the
! current track has none, set only once the table is complete. The previous
! track's table is dropped first. A track without PSSI or PQT2, a tag
! that is not what it should be, or a failed allocation leaves no table.

	.text
phrasefetch:
	mov.l	r14,@-r15
	mov.l	r13,@-r15
	mov.l	r12,@-r15
	mov.l	r11,@-r15
	mov.l	r10,@-r15
	mov.l	r9,@-r15
	mov.l	r8,@-r15
	sts.l	pr,@-r15
	add	#-40,r15		! 0 hash, 4 PSSI block, 8 PQT2 block, 12 mood table,
					! 16 phrase, 20 column, 24 end beat, 28 count, 32 colour
	mova	pool,r0
	mov	r0,r14
	mov.l	@(20,r4),r0
	mov.l	r0,@r15
	bra	start
	nop

	.p2align 2
pool:
	.long	0x081FFFF0		! 0   the table's word
	.long	0x08344B42		! 4   free
	.long	0x08344A9C		! 8   malloc
	.long	0x08533D50		! 12  memset
	.long	0x082545F8		! 16  generic tag fetch
	.long	0xFFE7FFFF		! 20  fpscr mask the firmware applies after calls
	.long	1200			! 24
	.long	600			! 28
	.ascii	"PSSI"			! 32
	.ascii	"PQT2"			! 36
	.asciz	"EXT"			! 40
	.byte	0xCB,0xE1,0xEE,0xFA,0xE5,0xEE,0xAD,0xEE,0xE9,0xD2	! 44 the mask key
	.byte	0xE9,0xEB,0xE1,0xE9,0xF3,0xE8,0xE9,0xF4,0xE1
	.byte	0
mood_high:				! 64 by kind: intro, up, down, -, chorus, outro
	.byte	0,2,8,3,0,5,6,0,0,0,0,0,0,0,0,0
mood_mid:				! 80 intro, verse 1-6, bridge, chorus, outro
	.byte	0,2,7,7,7,7,7,7,4,5,6,0,0,0,0,0
mood_low:				! 96 intro, verse 1-2 (three each), bridge, chorus, outro
	.byte	0,1,8,8,8,8,8,8,4,5,6,0,0,0,0,0

start:
	mov.l	@r14,r2			! drop the previous track's table; cleared first,
	mov.l	@r2,r1			! its reader runs in another task
	mov	#0,r3
	mov.l	r3,@(4,r15)
	mov.l	r3,@(8,r15)
	mov.l	r3,@r2
	cmp/pl	r1
	bf	1f
	mov.l	@(4,r14),r3
	jsr	@r3
	mov	r1,r4
1:
	mov	#0,r10			! the table, until it is published
	mov	r13,r4
	mov.l	@r15,r5
	mov	r14,r6
	add	#32,r6
	mov	r14,r7
	add	#40,r7
	bsr	fetch_tag
	nop
	mov.l	r0,@(4,r15)
	tst	r0,r0
	bt	done

	mov	r13,r4
	mov.l	@r15,r5
	mov	r14,r6
	add	#36,r6
	mov	r14,r7
	add	#40,r7
	bsr	fetch_tag
	nop
	mov.l	r0,@(8,r15)
	tst	r0,r0
	bt	done

	! -- the beat grid: r8 = its tag, r11 = beats, r12 = track length / 16 ms
	mov.l	@(8,r15),r8
	add	#4,r8
	mov.l	@(40,r8),r0
	bsr	be32
	nop
	mov	r0,r11
	mov	#2,r1
	cmp/hs	r1,r11
	bf	done
	mov.l	@(8,r8),r0
	bsr	be32
	nop
	mov	#56,r1
	cmp/hs	r1,r0
	bf	done
	mov.l	@(36,r8),r0
	bsr	be32
	nop
	mov	r0,r2
	mov.l	@(28,r8),r0
	bsr	be32
	nop
	sub	r0,r2			! the span from the first beat to the last
	cmp/pl	r2
	bf	done
	shlr2	r2
	shlr2	r2
	mov	r2,r0
	mov	r11,r1
	add	#-1,r1
	bsr	udiv
	nop
	mov	r0,r12			! one beat period
	mov.l	@(36,r8),r0
	bsr	be32
	nop
	shlr2	r0
	shlr2	r0
	add	r0,r12
	tst	r12,r12
	bt	done

	! -- the phrases: r9 = the tag, r13 = the mask's n, -1 for none
	mov.l	@(4,r15),r9
	add	#4,r9
	mov.l	@(12,r9),r0
	bsr	be32
	nop
	mov	#24,r1
	cmp/eq	r1,r0
	bf	done
	mov	r9,r1
	add	#16,r1
	mov.b	@r1+,r0
	extu.b	r0,r2
	shll8	r2
	mov.b	@r1+,r0
	extu.b	r0,r0
	or	r2,r0			! n
	mov.l	r0,@(28,r15)
	tst	r0,r0
	bt	done
	mov	#64,r1
	cmp/hi	r1,r0
	bt	done
	mov	r0,r1
	shll	r1
	add	r0,r1
	shll2	r1
	shll	r1			! 24 * n
	add	#32,r1
	mov.l	@(8,r9),r0
	bsr	be32
	nop
	cmp/hs	r1,r0			! the tag has to hold the phrases
	bf	done
	mov	r9,r1
	add	#18,r1
	mov.b	@r1+,r0
	extu.b	r0,r2
	shll8	r2
	mov.b	@r1,r0
	extu.b	r0,r0
	or	r2,r0			! the body's first halfword as stored
	mov	#20,r1
	cmp/hi	r1,r0
	bf	3f
	mov.l	@(28,r15),r13
	bra	4f
	nop
3:	mov	#-1,r13
4:
	mov	#0,r4
	bsr	body16
	nop
	mov	#1,r1
	cmp/eq	r1,r0			! 1 high, 3 low, anything else as mid
	bf	5f
	mov	#0,r2
	bra	7f
	nop
5:	mov	#3,r1
	cmp/eq	r1,r0
	bf	6f
	mov	#32,r2
	bra	7f
	nop
6:	mov	#16,r2
7:	mov	r14,r1
	add	#64,r1
	add	r2,r1
	mov.l	r1,@(12,r15)
	mov	#8,r4
	bsr	body16
	nop
	mov.l	r0,@(24,r15)

	mov.l	@(8,r14),r2		! the table
	mov.l	@(28,r14),r4
	jsr	@r2
	nop
	bsr	fpfix
	nop
	tst	r0,r0
	bt	done
	mov	r0,r10
	mov	r10,r4
	mov	#0,r5
	mov.l	@(28,r14),r6
	mov.l	@(12,r14),r2
	jsr	@r2
	nop
	bsr	fpfix
	nop

	mov	#0,r1
	mov.l	r1,@(16,r15)
	bra	first
	nop

phrase_next:
	mov.l	@(16,r15),r1
phrase_loop:
	mov	r1,r2			! entry offset 14 + 24 * phrase
	shll	r2
	add	r1,r2
	shll2	r2
	shll	r2
	add	#14,r2
	mov.l	r2,@(36,r15)
	mov	r2,r4
	add	#4,r4
	bsr	body16
	nop
	mov	#15,r1
	cmp/hi	r1,r0			! a kind past the table has no colour
	bt	8f
	mov.l	@(12,r15),r1
	mov.b	@(r0,r1),r0
	extu.b	r0,r0
	bra	9f
	nop
8:	mov	#0,r0
9:	mov.l	r0,@(32,r15)
	mov.l	@(16,r15),r1		! the next phrase's first beat, the end beat after the last
	add	#1,r1
	mov.l	@(28,r15),r2
	cmp/hs	r2,r1
	bt	10f
	mov.l	@(36,r15),r4
	add	#26,r4
	bsr	body16
	nop
	bra	11f
	nop
10:	mov.l	@(24,r15),r0
11:	mov	r0,r4
	bsr	beat_time
	nop
	mov	r0,r4
	bsr	column_of
	nop
	mov.l	@(20,r15),r2		! fill the columns from the last boundary to this one
	mov.l	@(32,r15),r3
	mov	r10,r4
	add	r2,r4
	cmp/hs	r0,r2
	bt	13f
12:	mov.b	r3,@r4
	add	#1,r4
	add	#1,r2
	cmp/hs	r0,r2
	bf	12b
13:	mov.l	r0,@(20,r15)
	mov.l	@(16,r15),r1
	add	#1,r1
	mov.l	r1,@(16,r15)
	mov.l	@(28,r15),r2
	cmp/hs	r2,r1
	bf	phrase_loop
	mov.l	@r14,r2			! publish
	mov.l	r10,@r2
	mov	#0,r10
	bra	done
	nop

first:	! the first phrase's start column
	mov	#16,r4			! the first phrase's first beat
	bsr	body16
	nop
	mov	r0,r4
	bsr	beat_time
	nop
	mov	r0,r4
	bsr	column_of
	nop
	mov.l	r0,@(20,r15)
	bra	phrase_next
	nop

done:
	tst	r10,r10
	bt	14f
	mov.l	@(4,r14),r2
	jsr	@r2
	mov	r10,r4
14:	mov.l	@(4,r15),r4
	tst	r4,r4
	bt	15f
	mov.l	@(4,r14),r2
	jsr	@r2
	nop
15:	mov.l	@(8,r15),r4
	tst	r4,r4
	bt	16f
	mov.l	@(4,r14),r2
	jsr	@r2
	nop
16:	add	#40,r15
	lds.l	@r15+,pr
	mov.l	@r15+,r8
	mov.l	@r15+,r9
	mov.l	@r15+,r10
	mov.l	@r15+,r11
	mov.l	@r15+,r12
	mov.l	@r15+,r13
	rts
	mov.l	@r15+,r14

! be32: r0 = the big-endian word stored in r0 as read (little-endian load).
be32:
	swap.b	r0,r0
	swap.w	r0,r0
	rts
	swap.b	r0,r0

! fpfix: the firmware clears the fpscr size and precision bits after a call.
fpfix:
	sts	fpscr,r1
	mov.l	@(20,r14),r2
	and	r2,r1
	rts
	lds	r1,fpscr

! udiv: r0 / r1 -> r0, unsigned. Clobbers r5-r7.
udiv:
	mov	r0,r7
	mov	#0,r0
	mov	#0,r5
	mov	#32,r6
1:	shll	r7
	rotcl	r5
	shll	r0
	cmp/hs	r1,r5
	bf	2f
	sub	r1,r5
	add	#1,r0
2:	dt	r6
	bf	1b
	rts
	nop

! beat_time: r4 = beat number -> r0 = its time in 16 ms units; 0 before the
! grid, the track length past it. r8 = the beat grid's tag, r11 = its beats.
beat_time:
	cmp/pl	r4
	bf	1f
	cmp/hi	r11,r4
	bt	2f
	sts.l	pr,@-r15
	add	#-1,r4
	mov.l	r4,@-r15
	mov.l	@(36,r8),r0
	bsr	be32
	nop
	mov	r0,r2
	mov.l	@(28,r8),r0
	bsr	be32
	nop
	mov	r0,r3			! the first beat's time
	sub	r3,r2
	shlr2	r2
	shlr2	r2			! the span
	mov.l	@r15+,r4
	mul.l	r4,r2
	sts	macl,r0
	mov	r11,r1
	add	#-1,r1
	bsr	udiv
	nop
	shlr2	r3
	shlr2	r3
	add	r3,r0
	lds.l	@r15+,pr
	rts
	nop
1:	rts
	mov	#0,r0
2:	rts
	mov	r12,r0

! column_of: r4 = a time in 16 ms units -> r0 = the column whose centre it
! passes, 0..600, as (t * 1200 + length) / (2 * length).
column_of:
	sts.l	pr,@-r15
	mov.l	@(24,r14),r1
	mul.l	r1,r4
	sts	macl,r0
	add	r12,r0
	mov	r12,r1
	shll	r1
	bsr	udiv
	nop
	mov.l	@(28,r14),r1
	cmp/hs	r1,r0
	bf	1f
	mov	r1,r0
1:	lds.l	@r15+,pr
	rts
	nop

! body16: r4 = an offset in the PSSI body -> r0 = the halfword there, unmasked.
body16:
	sts.l	pr,@-r15
	mov.l	r4,@-r15
	bsr	body8
	nop
	mov	r0,r5
	mov.l	@r15,r4
	add	#1,r4
	bsr	body8
	nop
	shll8	r5
	or	r5,r0
	add	#4,r15
	lds.l	@r15+,pr
	rts
	nop

! body8: r4 = an offset in the PSSI body -> r0 = the byte there, unmasked.
body8:
	mov	r9,r1
	add	#18,r1
	add	r4,r1
	mov.b	@r1,r2
	extu.b	r2,r2
	cmp/pz	r13
	bf	2f
	mov	r4,r1			! r1 = offset mod 19
	mov	#19,r3
1:	cmp/hs	r3,r1
	bf	3f
	bra	1b
	sub	r3,r1
3:	mov	r14,r0
	add	#44,r0
	add	r1,r0
	mov.b	@r0,r0
	add	r13,r0
	extu.b	r0,r0
	xor	r0,r2
2:	rts
	mov	r2,r0

! fetch_tag: r4 = the open ANLZ handle, r5 = the track's hash, r6 = the tag,
! r7 = the extension (both 4 bytes at most, 0-terminated if shorter) ->
! r0 = the block the generic fetch handed back, 0 when there is none.
fetch_tag:
	sts.l	pr,@-r15
	add	#-12,r15		! status, block, size
	mov	#0,r1
	mov.l	r1,@r15
	mov.l	r1,@(4,r15)
	mov.l	r1,@(8,r15)
	mov	r15,r1
	mov.l	r1,@-r15
	mov	r15,r1
	add	#8,r1
	mov.l	r1,@-r15
	mov	r15,r1
	add	#16,r1
	mov.l	r1,@-r15
	mov.l	@(16,r14),r1
	jsr	@r1
	nop
	add	#12,r15
	mov.l	r0,@r15
	bsr	fpfix
	nop
	mov.l	@(4,r15),r4
	mov.l	@r15,r0
	cmp/pz	r0
	bt	1f
	tst	r4,r4			! it failed: drop what it did hand back
	bt	2f
	mov.l	@(4,r14),r1
	jsr	@r1
	nop
2:	mov	#0,r4
1:	add	#12,r15
	lds.l	@r15+,pr
	rts
	mov	r4,r0
