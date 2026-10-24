! SPDX-License-Identifier: GPL-2.0-or-later
!
! Key shift for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian): every track load starts with no shift.
!
! Hook: the deck's request for a track's audio, the call 0x08300254 at
! 0x0830BBFE, after it returns. Of the callers of 0x08300254 only this one
! asks for audio, once per load, in the load's dispatcher 0x082FF464.
!
! The shift is 0x100 | (n & 0xFF); main_keyshift.s sends it.
!
! r0-r7 are free; r8-r14 are kept.

	.text
keyshiftload:
	mov.l	p_shift,r1
	mov	#1,r0
	shll8	r0
	rts
	mov.l	r0,@r1

	.p2align 2
p_shift:	.long 0x081FFFEC
