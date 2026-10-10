! SPDX-License-Identifier: GPL-2.0-or-later
!
! Routines and packet layout shared by the TCNet mods (main_tcnet.s,
! main_tcnettime.s and main_tcnetdata.s), included at the end of each so that every blob stays one
! self-contained piece of position-independent code. Each blob has its own
! SEQ counter and its own send descriptor. TCNet is little-endian, as MAIN.
!
! Every packet starts with TCNet's 24-byte management header: node id u16,
! protocol 3.5, "TCN", the message type, the node name, SEQ, node type 2
! (Master), node options 0 and a timestamp.

	.macro	tcn_header type
	.short	0			! node id
	.byte	3,5			! protocol version 3.5
	.ascii	"TCN"
	.byte	\type
	.ascii	"NXS2-0"		! node name, the last digit the player
	.byte	0,0
	.byte	0			! SEQ
	.byte	2			! node type: Master
	.short	0			! node options
	.long	0			! timestamp
	.endm

! The 68-byte Opt-IN: node count 0, listener port 65023, the uptime filled
! in by optin_fill, vendor, device and version 1.87.0.
	.macro	tcn_optin
	tcn_header 2
	.short	0			! node count
	.short	65023			! listener port
	.short	0			! uptime
	.short	0
	.ascii	"cdj-nxs2-qemu"
	.space	3
	.ascii	"CDJ-2000NXS2"
	.space	4
	.byte	1,87,0,0		! version 1.87.0
	.endm

! Fills the variable part of the header at r4: node id = the low 16 bits of
! the deck's IPv4 address in r6, the player digit r7 into the name, the next
! SEQ, and the timestamp, microseconds 0..999999 into the current second of
! the deck clock. Returns the deck clock in whole seconds in r0. Clobbers
! r0-r3, r5-r7, mach and macl; r4 is kept.
header:
	sts.l	pr,@-r15
	mov	r6,r0
	mov.w	r0,@(0,r4)
	mov	r7,r0
	mov.b	r0,@(13,r4)
	mova	seq,r0
	mov.l	@r0,r1
	add	#1,r1
	mov.l	r1,@r0
	mov	r4,r2
	add	#16,r2
	mov.b	r1,@r2
	bsr	now
	nop
	mov.l	c_div1000,r2
	dmulu.l	r0,r2
	sts	mach,r2
	shlr2	r2
	shlr2	r2
	shlr2	r2			! seconds
	mov.w	c_1000,r3
	mul.l	r2,r3
	sts	macl,r3
	sub	r3,r0			! milliseconds into the second
	mov.w	c_1000,r3
	mul.l	r3,r0
	sts	macl,r0
	add	r1,r0
	mov.l	r0,@(20,r4)
	mov	r2,r0
	lds.l	@r15+,pr
	rts
	nop

! Fills the Opt-IN at r4: the header as above, then the uptime, the deck
! clock in seconds modulo 12 hours. Same registers as header.
optin_fill:
	sts.l	pr,@-r15
	bsr	header
	nop
	mov.l	c_div43200,r1
	dmulu.l	r0,r1
	sts	mach,r1
	shlr2	r1
	shlr2	r1
	shlr2	r1
	mov.l	c_43200,r2
	mul.l	r2,r1
	sts	macl,r1
	sub	r1,r0
	mov.w	r0,@(28,r4)
	lds.l	@r15+,pr
	rts
	nop

! The deck clock: r0 = the RTOS tick word (one count per underflow of TMU
! unit 1 channel 1, reloaded from 10416 at Pphi/4, i.e. 1 ms), r1 = the
! microseconds elapsed in the running tick, (10416 - TCNT4) * 1000 / 10417
! done as a multiply by 6291 / 65536. An underflow the tick ISR has not
! counted yet shows in TCR4 bit 8. The tick, TCR4 and TCNT4 are read until
! two passes agree. Clobbers r2, r3, r5-r7 and macl.
now:
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
	add	r3,r0
	mov.w	c_tcor4,r6
	sub	r5,r6
	mov.w	c_scale,r7
	mul.l	r7,r6
	sts	macl,r1
	rts
	shlr16	r1

! r0 = the deck's IPv4 address, 0 when the network stack has none.
address:
	sts.l	pr,@-r15
	add	#-16,r15
	mov.l	p_getter,r1
	mov	r15,r5
	jsr	@r1
	mov	#0,r4
	tst	r0,r0
	bt/s	1f
	mov	#0,r1
	bra	2f
	mov	r1,r0
1:	mov.l	@(4,r15),r0
2:	add	#16,r15
	lds.l	@r15+,pr
	rts
	nop

! r0 = the TCNet layer state for the play-state code in r0. The codes 3 to 11
! (playing, looping, paused, stopped at the cue, cue play, cue scratch,
! searching forward and back, hold) are TCNet's own numbers for the same
! states; every other code (no track, loading, spun down, ended) has no TCNet
! state and is sent as 0, idle. Clobbers r2, r3.
layer_state:
	mov	r0,r2
	add	#-3,r2
	mov	#8,r3
	cmp/hi	r3,r2
	bf	1f
	mov	#0,r0
1:	rts
	nop

! Broadcasts r5 bytes at r4 to UDP port r6 through the send wrapper of the
! Pro DJ Link beat packet (descriptor: u32 length, u8 mode 1 = subnet
! broadcast, u32 destination, u8 0x60, u32 port), with the fpscr mask the
! firmware applies before every call. Clobbers r0-r7.
send:
	sts.l	pr,@-r15
	mova	desc,r0
	mov.l	r5,@r0
	mov.l	r6,@(16,r0)
	mov	r0,r5
	mov.l	p_wrap,r1
	mov.l	c_fpmask,r2
	sts	fpscr,r3
	and	r2,r3
	jsr	@r1
	lds	r3,fpscr
	lds.l	@r15+,pr
	rts
	nop

c_1000:		.word 1000
c_tcor4:	.word 10416
c_scale:	.word 6291
	.p2align 2
p_tick:		.long 0x0B0C5258
p_tcnt4:	.long 0xFFD90018
p_getter:	.long 0x08221D9E
p_wrap:		.long 0x085113EC
p_device:	.long 0x0AB84CFA
c_fpmask:	.long 0xFFE7FFFF
c_div1000:	.long 0x10624DD3	! 2^38 / 1000, rounded up
c_43200:	.long 43200
c_div43200:	.long 6362916		! 2^38 / 43200, rounded up
seq:		.long 0
desc:		.long 0,1,0,0x60,0
