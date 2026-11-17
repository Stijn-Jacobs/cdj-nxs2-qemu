! SPDX-License-Identifier: GPL-2.0-or-later
!
! TCNet Time packets for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian), the layer-sync half of the tcnet mod (Opt-IN and Status are
! main_tcnet.s). The deck is a TCNet Master with one layer, its own player
! number p (1..4); the 162-byte Time packet (message type 254) goes to UDP port
! 60001 on the subnet broadcast address, every 20 ms of deck clock and at once
! when the layer state changes. Layer p carries, every other layer is zero:
!
!     layer time ms     @24+4(p-1)  the play position word 0x09947488, which
!                                   this task recomputes from the DSP's
!                                   position frame on every pass, sent as is
!     layer total ms    @56+4(p-1)  the track length in seconds, 0x0B5125E8,
!                                   x 1000
!     beat marker       @88+(p-1)   status record +0xA6, the beat in the bar
!                                   1..4, 0 for anything else
!     layer state       @96+(p-1)   status record +0x78, the play-state code,
!                                   through layer_state
!
! SMPTE mode, timecode and on-air stay 0: the deck has no timecode source and
! cannot see the mixer's fader.
!
! Hook: the DJcont output task's loop, right after its position update and
! its DSP-output step, on the patch engine's trampoline. The status record is
! 0x0A371D3C + p * 0x124. The routine touches r0-r7 and r8, r9, r13 (saved).
!
! Mod state, written at run time inside this file: the deck clock and the
! layer state of the last Time packet, the state -1 before the first.

	.text
	.global tcnettime
tcnettime:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r13,@-r15
	mov.l	p_device,r1
	mov.b	@r1,r0
	extu.b	r0,r13			! the player
	mov	r13,r0
	add	#-1,r0
	mov	#3,r1
	cmp/hi	r1,r0
	bt	out
	mov.w	c_record,r1
	mul.l	r13,r1
	sts	macl,r9
	mov.l	p_records,r1
	add	r1,r9			! the deck's own status record
	mov	#0x78,r0
	mov.b	@(r0,r9),r0
	bsr	layer_state
	extu.b	r0,r0
	mov	r0,r8

	mov.l	p_tick,r1
	mov.l	@r1,r1
	mova	last,r0
	mov.l	@(4,r0),r2
	cmp/eq	r8,r2
	bf	1f
	mov.l	@r0,r2
	mov	r1,r3
	sub	r2,r3
	mov	#20,r2
	cmp/hs	r2,r3
	bf	out
1:	mov.l	r1,@r0
	mov.l	r8,@(4,r0)

	bsr	address
	nop
	tst	r0,r0
	bt	out
	mov	r0,r6
	mova	time,r0
	mov	r0,r4
	mov	r13,r7
	bsr	header
	add	#0x30,r7

	mova	time,r0
	mov	r0,r4
	mov	r4,r1
	add	#24,r1
	mov	#69,r2
	mov	#0,r3
1:	mov.w	r3,@r1			! every layer's fields, 24..161
	add	#2,r1
	dt	r2
	bf	1b

	mov	r13,r5
	add	#-1,r5			! the layer's index
	mov	r5,r1
	shll2	r1
	add	r4,r1
	mov.l	p_position,r2
	mov.l	@r2,r2
	mov.l	r2,@(24,r1)
	mov.l	p_length,r2
	mov.l	@r2,r2
	mov.w	c_1000,r3
	mul.l	r3,r2
	sts	macl,r2
	mov.l	r2,@(56,r1)
	mov	r4,r1
	add	r5,r1
	add	#88,r1
	mov.w	o_inbar,r0
	mov.b	@(r0,r9),r0
	extu.b	r0,r0
	mov	#4,r2
	cmp/hi	r2,r0
	bf	1f
	mov	#0,r0
1:	mov.b	r0,@r1
	add	#8,r1
	mov.b	r8,@r1

	mov.w	c_length,r5
	mov.l	p_port,r6
	bsr	send
	nop

out:
	mov.l	@r15+,r13
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

c_record:	.word 0x124
o_inbar:	.word 0xA6
c_length:	.word 162
	.p2align 2
p_records:	.long 0x0A371D3C
p_position:	.long 0x09947488
p_length:	.long 0x0B5125E8
p_port:		.long 60001
last:
	.long	0			! deck clock at the last Time packet
	.long	-1			! its layer state

	.include "main_tcnet_shared.inc"

	.p2align 2
time:
	tcn_header 254
	.space	162 - 24
	.p2align 2
