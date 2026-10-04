! SPDX-License-Identifier: GPL-2.0-or-later
!
! Real 3-band overview for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): the track's own PWV6 bands replace the colour preview in
! the overview payload the deck sends to the display.
!
! Hook: the publisher that copies the 600 six-byte preview records (3600
! bytes) into the link slot at 0x0B569528, run right after that copy, with
! the slot's lock still held. main_wave3data.s leaves the packed PWV6
! (600 x ABC, AB, A extents) behind a pointer at 0x081FFFFC while it is set
! and complete; with no pointer the stock records go out untouched.
!
! The records are three native halfwords each, which the display reads with
! the same values as its big-endian halfwords, so its byte k is byte k^1
! here. Each record becomes, as the display reads it: +0 ABC, +1 AB, +2 A
! (1..40 rows), +3 0x3B, +4 and +5 0xFF. In a stock record the +4 halfword
! starts with a height field, and 0xFFFF there would be 63 rows, past the
! strip renderer's own 40-row clamp, so the display takes it as the mark.

	.text
wave3ov:
	mov.l	p_shared,r1
	mov.l	@r1,r5			! packed PWV6, 0 or -1 when there is none
	cmp/pl	r5
	bf	out
	mov.l	p_slot,r4
	mov.l	p_600,r6
column:
	mov.b	@r5+,r0
	mov.b	r0,@(1,r4)		! ABC
	mov.b	@r5+,r0
	mov.b	r0,@r4			! AB
	mov.b	@r5+,r0
	mov.b	r0,@(3,r4)		! A
	mov	#0x3B,r0
	mov.b	r0,@(2,r4)
	mov	#-1,r0
	mov.b	r0,@(4,r4)
	mov.b	r0,@(5,r4)
	add	#6,r4
	dt	r6
	bf	column
out:
	rts
	nop

	.p2align 2
p_shared:	.long 0x081FFFFC
p_slot:		.long 0x0B569528
p_600:		.long 600
