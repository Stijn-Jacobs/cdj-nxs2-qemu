! SPDX-License-Identifier: GPL-2.0-or-later
!
! Routines shared by the two Ableton Link mods (main_abletonlink.s and
! main_abletonlinkpong.s), included at the end of each so that every blob
! stays one self-contained piece of position-independent code. The routines
! are entered with bsr and clobber r0-r7, mach, macl and pr as noted.

! Stores r5 big-endian at r4, byte by byte: Link's fields sit at odd offsets.
! Clobbers r0 only.
put32:
	mov	r5,r0
	shlr16	r0
	shlr8	r0
	mov.b	r0,@r4
	mov	r5,r0
	shlr16	r0
	mov.b	r0,@(1,r4)
	mov	r5,r0
	shlr8	r0
	mov.b	r0,@(2,r4)
	mov	r5,r0
	mov.b	r0,@(3,r4)
	rts
	nop

! Writes the 64-bit ghost time, big-endian, at r4.
!
! The host clock is the RTOS tick word (one count per underflow of TMU unit 1
! channel 1, reloaded from 10416 at Pphi/4, i.e. 1 ms) plus the microseconds
! elapsed in the running tick, (10416 - TCNT4) * 1000 / 10417 done as a
! multiply by 6291 / 65536. An underflow the tick ISR has not counted yet
! shows in TCR4 bit 8. The tick, TCR4 and TCNT4 are read until two passes
! agree. Ghost time is host time + 2^40 microseconds, which puts the deck's
! session ahead of any session a Link app founded on its own.
ghost_time:
	sts.l	pr,@-r15
	mov.l	p_tick,r1
	mov.l	p_tcnt4,r2
1:	mov.l	@r1,r3
	mov.w	@(4,r2),r0
	mov	r0,r6
	mov.l	@r2,r5
	mov.w	@(4,r2),r0
	cmp/eq	r0,r6
	bf	1b
	mov.l	@r1,r7
	cmp/eq	r3,r7
	bf	1b
	mov	r6,r0
	shlr8	r0
	and	#1,r0
	add	r0,r3
	mov.w	p_tcor4,r6
	sub	r5,r6
	mov.w	p_scale,r7
	mul.l	r7,r6
	sts	macl,r6
	shlr16	r6			! microseconds into the tick
	mov.w	p_1000,r7
	dmulu.l	r3,r7
	sts	macl,r5
	sts	mach,r3
	mov	#0,r7
	clrt
	addc	r6,r5
	addc	r7,r3
	mov	#1,r7
	shll8	r7
	add	r7,r3			! + 2^40
	mov	r5,r7
	mov	r3,r5
	bsr	put32
	nop
	add	#4,r4
	bsr	put32
	mov	r7,r5
	lds.l	@r15+,pr
	rts
	nop

! Writes this deck's Link node id at r4: its IPv4 address, "NXS" and its
! player number. Returns the address in r0, 0 when the network stack has none.
node_id:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	add	#-16,r15
	mov	r4,r8
	mov.l	p_getter,r1
	mov	r15,r5
	jsr	@r1
	mov	#0,r4
	tst	r0,r0
	bt	1f
	bra	2f
	mov	#0,r0
1:	mov.l	@(4,r15),r1
	mov	r8,r4
	bsr	put32
	mov	r1,r5
	mov	#0x4E,r0
	mov.b	r0,@(4,r8)
	mov	#0x58,r0
	mov.b	r0,@(5,r8)
	mov	#0x53,r0
	mov.b	r0,@(6,r8)
	mov.l	p_device,r2
	mov.b	@r2,r0
	mov.b	r0,@(7,r8)
	mov	r1,r0
2:	add	#16,r15
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

	.p2align 2
p_tick:		.long 0x0B0C5258
p_tcnt4:	.long 0xFFD90018
p_getter:	.long 0x08221D9E
p_device:	.long 0x0AB84CFA
p_tcor4:	.word 10416
p_scale:	.word 6291
p_1000:		.word 1000
