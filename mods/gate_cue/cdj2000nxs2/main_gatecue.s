! SPDX-License-Identifier: GPL-2.0-or-later
!
! Gate Cue for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian), the release half: when the pad of the slot
! main_gatecuepress.s armed is let go, the deck goes back to that hot cue and
! pauses, as a CDJ-3000 does. Stock 1.87 has no gate: a hot cue recalled while
! paused plays on after the pad is released.
!
! Hook: the entry of the pad pass's mode switch 0x08442D96, after its pushes
! of r8-r14 and pr. The pad pass 0x08443050 calls it on every Player pass, once the
! edges of the pass are queued, whether or not a pad is down; the stock pad
! handling never looks at a release.
!
! While a slot n in 1..8 is armed and the pad of slot n on the deck's own
! panel (Player key bank word 0x0B056EDC + 4n) reads 0, the gate is disarmed
! and the CUE key's own return to the cue (0x084337EE once its checks have
! passed) is run with slot n in place of slot 0, the main cue:
!
!     0x0844794C            clears a flag (0x0B058C34)
!     0x084330E4            clears the Player's paused flag, Player state 7
!     0x0842ADD2(n, 1, *0x0B058C04)
!                           posts the transport command 3 for the slot's cue
!                           number (5..7, 10..14) with the stop flag set
!     0x0843278A            the Player's mode bookkeeping
!     0x0843D712            loop tidy-up
!
! The firmware clears the size and precision bits of fpscr after every call;
! so does this routine.
!
! r0-r7 are free; r8-r14 are kept.

	.text
gatecue:
	mov.l	p_armed,r1
	mov.l	@r1,r2
	mov	r2,r0
	add	#-1,r0
	mov	#8,r3
	cmp/hs	r3,r0
	bt	idle
	mov	r2,r0
	shll2	r0
	mov.l	p_pads,r3
	add	r3,r0
	mov.l	@r0,r0
	tst	r0,r0
	bf	idle
	mov	#0,r0
	mov.l	r0,@r1

	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov	r2,r8
	mov.l	p_mask,r9

	mov.l	p_clear,r1
	jsr	@r1
	nop
	bsr	fpfix
	nop
	mov.l	p_stop,r1
	jsr	@r1
	nop
	bsr	fpfix
	nop
	mov.l	p_cuearg,r1
	mov.l	@r1,r6
	mov	#1,r5
	mov.l	p_cue,r1
	jsr	@r1
	mov	r8,r4
	bsr	fpfix
	nop
	mov.l	p_modes,r1
	jsr	@r1
	nop
	bsr	fpfix
	nop
	mov.l	p_loop,r1
	jsr	@r1
	nop
	bsr	fpfix
	nop

	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
idle:
	rts
	nop

fpfix:
	sts	fpscr,r0
	and	r9,r0
	rts
	lds	r0,fpscr

	.p2align 2
p_armed:	.long 0x081FFFF8
p_pads:		.long 0x0B056EDC
p_mask:		.long 0xFFE7FFFF
p_clear:	.long 0x0844794C
p_stop:		.long 0x084330E4
p_cuearg:	.long 0x0B058C04
p_cue:		.long 0x0842ADD2
p_modes:	.long 0x0843278A
p_loop:		.long 0x0843D712
