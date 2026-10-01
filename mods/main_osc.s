! SPDX-License-Identifier: GPL-2.0-or-later
!
! Deck state as OSC for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian), the state half of the osc mod (the per-beat half is
! main_oscbeat.s). Eight values of the deck's own status record are sent
! to UDP port 50010 on the subnet broadcast address as one-int OSC messages,
! <n> being the player number as one ASCII digit:
!
!     /cdj/<n>/playing      record +0x89 bit 0x40
!     /cdj/<n>/state        record +0x78, the play-state code
!     /cdj/<n>/loaded       0/1, the play-state code is not 0
!     /cdj/<n>/end          0/1, the play-state code is 0x11
!     /cdj/<n>/master       record +0x89 bit 0x20
!     /cdj/<n>/sync         record +0x89 bit 0x10
!     /cdj/<n>/masterdeck   byte 0x0A35F39C, the tempo master's player, 0 none
!     /cdj/<n>/track        record +0x2C, the rekordbox id, 0 none
!
! A value is sent when it differs from the last one sent, at most two per
! pass, the rest on the following passes. Every value starts as "never sent".
! When a pass sends nothing and 250 ms of deck clock (the RTOS tick word, one
! count per millisecond) have passed since the last refresh, the next value in
! a rotation of the eight is sent again, so a receiver that starts late has
! all of them within 2 s.
!
! Five more messages mark an edge of the status record against the previous
! pass. They are sent at once, outside the two-per-pass limit and the refresh,
! and a deck at rest sends none of them:
!
!     /cdj/<n>/load         the play-state code becomes 2; the rekordbox id
!     /cdj/<n>/play         the playing flag rises; the BPM x 100 (record
!                           +0x90, low 16 bits, 0 when its bit 31 is clear)
!     /cdj/<n>/stop         the playing flag falls; 0
!     /cdj/<n>/cue          the code becomes 6 or 7; the code
!     /cdj/<n>/loop         the code becomes 4 (1) or leaves 4 (0)
!
! The first pass after boot only records the previous values.
!
! Hook: the Sentinel task's call to the status serialiser, on the patch
! engine's trampoline, so it runs right after the serialiser returns with
! r11 = the deck's own status record and r12 = the fpscr mask the firmware
! applies around every call. The send wrapper (beat packet path, descriptor
! u32 length, u8 mode 1 = broadcast, u32 destination, u8 0x60, u32 port) is
! called at its fixed 1.87 address: no site of this mod loads it. Message and
! descriptor live in this blob, not on the stack. The routine touches r0-r7
! and r8, r9, r13, r14 (saved); r11 and r12 are only read.
!
! Mod state, written at run time inside this file: the last value sent for
! each row, the rotation cursor, the clock of the last refresh and the code
! and playing flag of the previous pass.

	.text
	.global osc
osc:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	add	#-32,r15		! the eight current values
	mov.l	p_device,r1
	mov.b	@r1,r0
	extu.b	r0,r0
	add	#-1,r0
	mov	#3,r1
	cmp/hi	r1,r0
	bf	1f
	bra	out
	nop
1:	add	#0x31,r0
	mov	r0,r14			! the player's digit
	mova	state,r0
	mov	r0,r8

	mov.w	o_flags,r0
	mov.b	@(r0,r11),r1
	extu.b	r1,r1			! flags
	mov	#0x78,r0
	mov.b	@(r0,r11),r2
	extu.b	r2,r2			! play-state code
	mov	r1,r3
	bsr	flag
	mov	#0x40,r0
	mov.l	r0,@r15			! playing
	mov.l	r2,@(4,r15)		! state
	mov	r2,r3
	bsr	flag
	mov	#-1,r0
	mov.l	r0,@(8,r15)		! loaded
	mov	r2,r0
	cmp/eq	#0x11,r0
	movt	r0
	mov.l	r0,@(12,r15)		! end
	mov	r1,r3
	bsr	flag
	mov	#0x20,r0
	mov.l	r0,@(16,r15)		! master
	mov	r1,r3
	bsr	flag
	mov	#0x10,r0
	mov.l	r0,@(20,r15)		! sync
	mov.l	p_master,r1
	mov.b	@r1,r0
	extu.b	r0,r0
	mov.l	r0,@(24,r15)		! masterdeck
	mov	#0x2C,r0
	mov.l	@(r0,r11),r0
	mov.l	r0,@(28,r15)		! track

	mov.l	@(40,r8),r9		! the previous pass's code
	cmp/pz	r9
	bf	ev_save
	mov.l	@(4,r15),r13
	cmp/eq	r9,r13
	bt	ev_play
	mov	r13,r0
	cmp/eq	#2,r0
	bf	1f
	mov.l	@(28,r15),r5
	bsr	send
	mov	#8,r4
1:	mov	r13,r0
	add	#-6,r0
	mov	#1,r1
	cmp/hi	r1,r0
	bt	1f
	mov	r13,r5
	bsr	send
	mov	#11,r4
1:	mov	r13,r0
	cmp/eq	#4,r0
	movt	r5
	bt	1f
	mov	r9,r0
	cmp/eq	#4,r0
	bf	ev_play
1:	bsr	send
	mov	#12,r4
ev_play:
	mov.l	@r15,r13
	mov.l	@(44,r8),r0
	cmp/eq	r13,r0
	bt	ev_save
	tst	r13,r13
	bt	1f
	mov.w	o_bpm,r0
	mov.l	@(r0,r11),r5
	cmp/pz	r5
	bf/s	2f
	extu.w	r5,r5
	mov	#0,r5
2:	bra	3f
	mov	#9,r4
1:	mov	#0,r5
	mov	#10,r4
3:	bsr	send
	nop
ev_save:
	mov.l	@(4,r15),r0
	mov.l	r0,@(40,r8)
	mov.l	@r15,r0
	mov.l	r0,@(44,r8)

	mov	#0,r13
	mov	#0,r9
1:	mov	r13,r0
	shll2	r0
	mov.l	@(r0,r15),r5
	mov.l	@(r0,r8),r1
	cmp/eq	r5,r1
	bt	2f
	mov	r9,r0
	cmp/eq	#2,r0
	bt	4f
	bsr	send
	mov	r13,r4
	mov	r13,r0
	shll2	r0
	mov.l	@(r0,r15),r1
	mov.l	r1,@(r0,r8)
	add	#1,r9
2:	add	#1,r13
	mov	r13,r0
	cmp/eq	#8,r0
	bf	1b

4:	tst	r9,r9
	bf	out
	mov.l	p_tick,r1
	mov.l	@r1,r1
	mov.l	@(36,r8),r2
	mov	r1,r3
	sub	r2,r3
	mov.w	o_period,r0
	cmp/hs	r0,r3
	bf	out
	mov.l	r1,@(36,r8)
	mov.l	@(32,r8),r4
	mov	r4,r0
	shll2	r0
	bsr	send
	mov.l	@(r0,r15),r5
	mov.l	@(32,r8),r0
	add	#1,r0
	cmp/eq	#8,r0
	bf	1f
	mov	#0,r0
1:	mov.l	r0,@(32,r8)

out:
	add	#32,r15
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

! r0 = 1 when r3 has any bit of the mask in r0 set, else 0.
flag:
	and	r3,r0
	tst	r0,r0
	movt	r0
	rts
	xor	#1,r0

! Sends row r4 with the value r5: the row's template is copied to the message
! buffer, the player's digit and the value (big-endian, the message's last
! four bytes) are filled in, then the wrapper is called.
send:
	sts.l	pr,@-r15
	mov	r4,r0
	shll2	r0
	shll2	r0
	shll	r0
	mov	r0,r2
	mova	rows,r0
	add	r0,r2
	mov.l	@r2+,r3
	mova	msg,r0
	mov	r0,r6
	mov	r6,r4
	mov	#7,r7
1:	mov.l	@r2+,r1
	mov.l	r1,@r4
	add	#4,r4
	dt	r7
	bf	1b
	mov	r14,r0
	mov.b	r0,@(5,r6)
	swap.b	r5,r5
	swap.w	r5,r5
	swap.b	r5,r5
	mov	r6,r1
	add	r3,r1
	add	#-4,r1
	mov.l	r5,@r1
	mova	desc,r0
	mov	r0,r5
	mov.l	r3,@r5
	mov	r6,r4
	mov.l	p_wrap,r1
	sts	fpscr,r2
	and	r12,r2
	jsr	@r1
	lds	r2,fpscr
	lds.l	@r15+,pr
	rts
	nop

o_flags:	.word 0x89
o_period:	.word 250
o_bpm:		.word 0x90
	.balign 4,0
p_device:	.long 0x0AB84CFA
p_master:	.long 0x0A35F39C
p_tick:		.long 0x0B0C5258
p_wrap:		.long 0x085113EC

state:
	.long	-1,-1,-1,-1,-1,-1,-1,-1	! last value sent per row
	.long	0			! rotation cursor
	.long	0			! deck clock at the last refresh
	.long	-1			! previous pass's code, -1 before the first
	.long	0			! previous pass's playing flag

! The wrapper's descriptor: length filled in per send.
desc:
	.long	0,1,0,0x60,50010

	.macro row name
	.long	2f - 1f
1:	.ascii	"/cdj/0/\name"
	.byte	0
	.balign	4,0
	.ascii	",i"
	.byte	0,0
	.long	0
2:	.space	28 - (2b - 1b)
	.endm

! Per row (the first eight are the state rows, the rest the events) a length, then the message with the digit and value still to fill in.
rows:
	row	playing
	row	state
	row	loaded
	row	end
	row	master
	row	sync
	row	masterdeck
	row	track
	row	load
	row	play
	row	stop
	row	cue
	row	loop

msg:
	.space	28
