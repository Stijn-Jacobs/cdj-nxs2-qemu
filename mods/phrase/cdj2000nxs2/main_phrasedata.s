! SPDX-License-Identifier: GPL-2.0-or-later
!
! Phrase colours for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): writes the track's phrase colour into each of the 600
! overview records the deck sends to the display.
!
! Hook: the publisher that copies the 600 six-byte preview records into the
! link slot at 0x0B569528, right after that copy (the site main_wave3ovdata.s
! hooks too; this runs after it, so a 3-band record keeps its marker). The
! display reads record byte 5 as a palette index only in a palette mode this
! deck does not use, so its low four bits carry the phrase colour: in the
! halfword at +4 as MAIN stores it, they are the low byte's low nibble. 0 =
! no phrase. main_phrasefetch.s leaves the 600-byte table behind a pointer
! at 0x081FFFF0 while it is set; without one every nibble is cleared, so
! the stock records' own bits never read as a colour.

	.text
phrasedata:
	mov.l	p_shared,r1
	mov.l	@r1,r5			! the table, 0 or -1 when there is none
	mov.l	p_slot,r4
	mov.l	p_600,r6
	cmp/pl	r5
	bf	none
column:
	mov.b	@r5+,r2
	mov.b	@(4,r4),r0
	and	#0xF0,r0
	or	r2,r0
	mov.b	r0,@(4,r4)
	add	#6,r4
	dt	r6
	bf	column
	rts
	nop
none:
	mov.b	@(4,r4),r0
	and	#0xF0,r0
	mov.b	r0,@(4,r4)
	add	#6,r4
	dt	r6
	bf	none
	rts
	nop

	.p2align 2
p_shared:	.long 0x081FFFF0
p_slot:		.long 0x0B569528
p_600:		.long 600
