! SPDX-License-Identifier: GPL-2.0-or-later
!
! Per-beat OSC output for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian), the beat half of the osc mod (the state half is
! main_osc.s). Every time MAIN sends its Pro DJ Link beat packet, send up
! to three OSC messages to UDP port 50010 on the subnet broadcast address,
! <n> being the player number from the packet as one ASCII digit:
!
!     /cdj/<n>/beat    ,i       beat in bar, 1..4
!     /cdj/<n>/bar     ,i       1, only on beat 1 of the bar
!     /cdj/<n>/timing  ,iiiiii  beat in bar, bpm x100, pitch, ms to the next
!                               beat, ms to the next bar, the deck clock in ms
!
! The arguments are the beat packet's own fields (offsets into the 0x60-byte
! packet): beat in bar +0x5C, bpm x100 +0x5A (16-bit), pitch +0x54 (0x100000 =
! 0 percent), next beat +0x24 and next bar +0x2C (ms at normal speed). The
! deck clock is the RTOS tick word, one count per millisecond. Nothing is
! sent when the packet's player is not 1 to 4.
!
! Hook: the beat loop's call to the send wrapper, on the patch engine's
! trampoline, exactly as main_oscbeat.s: r5 = the beat function's frame, the
! packet is *(frame+0x218), the stock descriptor at frame+0x150, r12 = the
! send wrapper and r10 = the fpscr mask. Message and descriptor are built on
! the stack (the wrapper copies the payload), the descriptor being a copy of
! the stock one with its length and port replaced. r8, r9, r13 and r14 are
! saved; r10 and r12 are only read.

	.text
	.global oscbeat
oscbeat:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	add	#-68,r15		! 48 bytes of message, 20 of descriptor
	mov.w	o_packet,r0
	mov.l	@(r0,r5),r8		! the beat packet
	mov.w	o_desc,r0
	mov	r5,r6
	add	r0,r6			! the stock descriptor
	mov	r15,r13
	add	#48,r13			! its copy
	mov	r13,r7
	mov	#5,r3
1:	mov.l	@r6+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r3
	bf	1b

	mov	#0x5F,r0
	mov.b	@(r0,r8),r0
	extu.b	r0,r0
	add	#-1,r0
	mov	#3,r1
	cmp/hi	r1,r0
	bf	1f
	bra	out
	nop
1:	add	#0x31,r0
	mov	r0,r14			! the player's digit
	mov	#0x5C,r0
	mov.b	@(r0,r8),r0
	extu.b	r0,r9			! beat in bar

	mov	r15,r4
	mova	t_beat,r0
	bsr	emit
	mov	r0,r5
	mov	#19,r0
	mov.b	r9,@(r0,r15)
	mov	r15,r4
	mov	r13,r5
	bsr	send
	mov	#20,r6

	mov	r9,r0
	cmp/eq	#1,r0
	bf	1f
	mov	r15,r4
	mova	t_bar,r0
	bsr	emit
	mov	r0,r5
	mov	r15,r4
	mov	r13,r5
	bsr	send
	mov	#20,r6

1:	mov	r15,r4
	mova	t_timing,r0
	bsr	emit
	mov	r0,r5
	mov	#27,r0
	mov.b	r9,@(r0,r15)
	mova	fields,r0
	mov	r0,r7
2:	mov.b	@r7+,r0
	extu.b	r0,r0
	tst	r0,r0
	bt	4f
	mov	r8,r5
	add	r0,r5			! source in the packet
	mov.b	@r7+,r0
	mov	r15,r4
	add	r0,r4			! destination in the message
	mov.b	@r7+,r6
3:	mov.b	@r5+,r0
	mov.b	r0,@r4
	add	#1,r4
	dt	r6
	bf	3b
	bra	2b
	nop
4:	mov.l	p_tick,r1
	mov.l	@r1,r5
	swap.b	r5,r5
	swap.w	r5,r5
	swap.b	r5,r5
	mov	#44,r0
	mov.l	r5,@(r0,r15)
	mov	r15,r4
	mov	r13,r5
	bsr	send
	mov	#48,r6

out:
	add	#68,r15
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

! Copies the template at r5 (a length word, then the message) to r4 and sets
! the player's digit in the address.
emit:
	mov.l	@r5+,r2
	shlr2	r2
	mov	r4,r7
1:	mov.l	@r5+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r2
	bf	1b
	mov	r14,r0
	rts
	mov.b	r0,@(5,r4)

! Sends the message at r4, r6 bytes long, with the descriptor at r5.
send:
	sts.l	pr,@-r15
	mov.l	r6,@r5
	mov.l	p_port,r1
	mov.l	r1,@(16,r5)
	sts	fpscr,r2
	and	r10,r2
	jsr	@r12
	lds	r2,fpscr
	lds.l	@r15+,pr
	rts
	nop

o_desc:		.word 0x150
o_packet:	.word 0x218
	.balign 4,0
p_port:		.long 50010
p_tick:		.long 0x0B0C5258

! Packet field, offset into the timing message, length; 0 ends the list.
fields:
	.byte	0x5A,30,2
	.byte	0x54,32,4
	.byte	0x24,36,4
	.byte	0x2C,40,4
	.byte	0
	.balign 4,0

! Length, then the message with the digit and the arguments still to fill in.
t_beat:
	.long	20
	.ascii	"/cdj/0/beat"
	.byte	0
	.ascii	",i"
	.byte	0,0
	.long	0
t_bar:
	.long	20
	.ascii	"/cdj/0/bar"
	.byte	0,0
	.ascii	",i"
	.byte	0,0
	.byte	0,0,0,1
t_timing:
	.long	48
	.ascii	"/cdj/0/timing"
	.byte	0,0,0
	.ascii	",iiiiii"
	.byte	0
	.space	24
