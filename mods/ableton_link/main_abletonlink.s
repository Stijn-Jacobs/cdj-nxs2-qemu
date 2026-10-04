! SPDX-License-Identifier: GPL-2.0-or-later
!
! Ableton Link peer for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): every time MAIN sends its Pro DJ Link beat packet, announce
! the deck to Link apps on the network with a Link ALIVE datagram carrying the
! deck's tempo and beat position, so Live or any Link app follows it. The
! measurement half of a Link peer is main_abletonlinkpong.s.
!
! The ALIVE goes to UDP port 20808, the port Link peers listen on, on the
! subnet broadcast address: the network stack shows no sign of multicast, and
! a Link socket bound to 0.0.0.0 receives broadcasts. It is sent through the
! same send wrapper as the beat packet, with a copy of the beat's descriptor
! whose length and port are replaced, as main_oscbeat.s does.
!
! Hook: the beat loop's call to the send wrapper, on the patch engine's
! trampoline (see sigpatch.trampoline()), so it runs right after the stock
! send with r5 = the beat function's frame, r12 = the send wrapper and r10 =
! the fpscr mask the firmware applies around every call. The beat packet is
! *(frame+0x218), the stock descriptor at frame+0x150.
!
! Message, 107 bytes, integers big-endian (Link's discovery v1 wire format):
!
!     "_asdp_v" 01  01 (ALIVE)  ttl 10 s  group 0  <node id>
!     "tmln" 24  <microseconds per beat> <beat origin> <time origin>
!     "sess" 8   <session id>
!     "stst" 17  playing 0, beats 0, timestamp 0
!     "mep4" 6   <own IPv4 address> 50000
!
! The node id and the session id are the same eight bytes
! (main_link_shared.inc): the deck founds its own session. The timeline is
! re-anchored on every beat; each field is an i64:
!
!     microseconds per beat  6e9 x 0x100000 / (bpm_x100 x pitch) from the
!                            packet's +0x5A and +0x54, in single-precision
!                            floating point and rounded: the tempo the deck
!                            plays at, pitch included
!     beat origin            N x 1,000,000, N the smallest beat count above
!                            the previous one with N mod 4 = beat_in_bar - 1
!                            (packet +0x5C), so the origin only grows and bar
!                            phase follows the deck's downbeat
!     time origin            the ghost time now (main_link_shared.inc)
!
! A beat with no BPM or no pitch sends nothing. The start/stop state is
! always "stopped, timestamp 0", which Link apps ignore.
!
! Mod state (one word inside this file, before its literal pool): N, zero at
! boot. The routine touches r0-r7 and r8, r9, r11, r13 (saved); r10 and r12
! are only read.

	.text
	.global abletonlink
abletonlink:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r11,@-r15
	mov.l	r13,@-r15
	add	#-128,r15		! 108 bytes of message, 20 of descriptor

	mov	r5,r11
	sts	fpscr,r0
	and	r10,r0
	lds	r0,fpscr
	mova	state,r0
	mov	r0,r9
	mov.w	o_packet,r0
	mov.l	@(r0,r11),r8		! the beat packet

	mova	message,r0
	mov	r0,r6
	mov	r15,r7
	mov	#27,r3
1:	mov.l	@r6+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r3
	bf	1b

	mov	r15,r4
	add	#12,r4
	bsr	node_id
	nop
	tst	r0,r0
	bf	1f
	bra	out
	nop
1:	mov	r0,r13			! the address, for mep4
	mov	r15,r4
	add	#60,r4
	mov.l	@(12,r15),r1
	mov.l	r1,@r4
	mov.l	@(16,r15),r1
	mov.l	r1,@(4,r4)
	mov	r15,r4
	add	#101,r4
	bsr	put32
	mov	r13,r5

	mov	#0x5A,r0
	mov.w	@(r0,r8),r1
	swap.b	r1,r1
	extu.w	r1,r1			! bpm x 100
	mov	#0x54,r0
	mov.l	@(r0,r8),r2
	swap.b	r2,r2
	swap.w	r2,r2
	swap.b	r2,r2			! pitch, 0x100000 = 0 percent
	mov	#0x5C,r0
	mov.b	@(r0,r8),r6		! beat in bar
	tst	r1,r1
	bf	1f
	bra	out
	nop
1:	tst	r2,r2
	bf	1f
	bra	out
	nop
1:	lds	r1,fpul
	float	fpul,fr1
	lds	r2,fpul
	float	fpul,fr2
	fmul	fr2,fr1
	mov.l	c_ratio,r3
	lds	r3,fpul
	fsts	fpul,fr0
	fdiv	fr1,fr0
	mov.l	c_half,r3
	lds	r3,fpul
	fsts	fpul,fr2
	fadd	fr2,fr0
	ftrc	fr0,fpul
	sts	fpul,r5
	mov	r15,r4
	add	#32,r4
	bsr	put32
	nop

	mov	r6,r0
	add	#-1,r0
	and	#3,r0
	mov	r0,r6
	mov.l	@r9,r3
1:	add	#1,r3
	mov	r3,r0
	and	#3,r0
	cmp/eq	r6,r0
	bf	1b
	mov.l	r3,@r9
	mov.l	c_micro,r7
	dmulu.l	r3,r7
	mov	r15,r4
	add	#36,r4
	sts	mach,r5
	bsr	put32
	nop
	add	#4,r4
	sts	macl,r5
	bsr	put32
	nop

	mov	r15,r4
	add	#44,r4
	bsr	ghost_time
	nop

	mov.w	o_desc,r0
	mov	r11,r6
	add	r0,r6			! the stock descriptor
	mov	r15,r5
	add	#108,r5			! its copy
	mov	r5,r7
	mov	#5,r3
1:	mov.l	@r6+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r3
	bf	1b
	mov	#107,r1
	mov.l	r1,@r5			! length
	mov.l	p_port,r1
	mov.l	r1,@(16,r5)		! port
	mov	r15,r4
	sts	fpscr,r2
	and	r10,r2
	jsr	@r12
	lds	r2,fpscr

out:
	add	#64,r15
	add	#64,r15
	mov.l	@r15+,r13
	mov.l	@r15+,r11
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

o_desc:		.word 0x150
o_packet:	.word 0x218
	.p2align 2
state:
	.long 0			! N
p_port:		.long 20808
c_micro:	.long 1000000
c_ratio:	.long 0x59B2D05E	! 6e9 x 0x100000 as a float
c_half:		.long 0x3F000000	! 0.5

! The ALIVE with every variable field zeroed for the code above to fill in.
message:
	.ascii	"_asdp_v"
	.byte	1
	.byte	1,10,0,0		! ALIVE, ttl 10 s, group 0
	.space	8			! node id
	.ascii	"tmln"
	.byte	0,0,0,24
	.space	24
	.ascii	"sess"
	.byte	0,0,0,8
	.space	8
	.ascii	"stst"
	.byte	0,0,0,17
	.space	17
	.ascii	"mep4"
	.byte	0,0,0,6
	.space	4
	.byte	0xC3,0x50		! port 50000
	.byte	0

	.include "main_link_shared.inc"
