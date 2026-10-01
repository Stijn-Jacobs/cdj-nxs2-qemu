! SPDX-License-Identifier: GPL-2.0-or-later
!
! USB-MIDI clock for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian), the timing half of the usb_midi mod: counts the Timing
! Clocks (F8, 24 per beat) and the Start / Stop / Continue events on the
! firmware's 1 ms timer, and wakes the MIDI task, whose hook (main_usbmidi.s)
! sends them. Clocks therefore leave on a 1 ms grid, one per USB transfer.
!
! Hook: the body of the timer interrupt that bumps the tick word 0x0B0C5258
! (TMU unit 1 channel 1, TCOR 10416 at Pphi/4: one underflow per ms), on a
! down-counter early in the body. The body runs on every tick once the deck's
! tasks are up. Interrupt context: no lock is taken and the buffers are not
! touched; the routine only reads MAIN state, updates its own and posts to an
! event flag with set_flg, which the same interrupt body already does for
! another task. It saves mach and macl, which the interrupted code may hold.
!
! Arming: the deck only starts the MIDI task's class when it enters PC
! control mode, so on a playing deck the task never gets its "configured"
! event. Once the host has configured the port (USB1 INTSTS0, device state
! bits 6-4 = 3) and the MIDI task sits in its attached state (0x0BD50E8C = 1),
! the routine posts that event to the MIDI class's flag itself, once per
! connection, and zeroes both buffer lengths, which the task's init leaves at
! the buffer size. Only the MIDI class is armed: the stock class start would
! arm the audio and HID classes too, which stops playback. A reset or unplug
! (state other than 3) re-arms it.
!
! Inputs, all read from MAIN's own state:
!
!     player    the deck's own device number (byte at 0x0AB84CFA), 1..4
!     record    the deck's Pro DJ Link status record, 0x0A371D3C + player *
!               0x124, the source the status packet is serialised from:
!                 +0x89 flags, bit 0x40 = playing
!                 +0x8C pitch, 0x100000 = 0 %
!                 +0x90 BPM word, bit 31 = valid, low 16 bits = BPM x 100
!
! Clock: with playing and a valid BPM, the tempo is BPM x 100 * pitch /
! 0x100000 (the 64-bit product from dmulu.l shifted right 20, truncated to
! 0.01 BPM). Every tick, phase += tempo, and one clock is due for every
! 250,000 of phase (24 pulses per beat / 60 s / 1000 ticks per s x 100). Up
! to 655 BPM at +100 % that is at most one clock per tick.
!
! Transport: on the first rise of the play flag since boot, Start; on later
! rises, Continue; on a fall, Stop. On a rise the phase starts one short of a
! clock, so the first clock goes out with the Start or Continue and marks the
! downbeat. The flag is the only measured signal, so Start after a return to
! the cue point is not told from a resume.
!
! Hand-over: while the host has the port configured (the gate word the stock
! generator tests), each tick that produced something adds its clocks to
! "clocks due", stores a transport status and bumps "events due", and sets
! bit 0x2 of the MIDI task's flag. The task's wait takes any bit and its loop
! drops the ones its state does not accept, so 0x2 only brings it round to
! the hook. main_usbmidi keeps the matching "sent" counts; each counter has
! one writer.
!
! State: the nine words after the first 8 bytes, which main_usbmidi.s reaches
! from its own end (patch_main places this file right after it).
! The routine touches r0-r7 and r8-r10 (saved).

	.text
	.global usbmiditick
usbmiditick:
	mova	state,r0
	bra	tick
	mov	r0,r1
	nop

	.p2align 2
state:
	.long 0			! +0  phase
	.long 0			! +4  last play flag
	.long 0			! +8  started
	.long 0			! +12 armed
	.long 0			! +16 clocks due
	.long 0			! +20 transport events due
	.long 0			! +24 status of the latest event
	.long 0			! +28 clocks sent (main_usbmidi)
	.long 0			! +32 transport events sent (main_usbmidi)

tick:
	sts.l	pr,@-r15
	sts.l	mach,@-r15
	sts.l	macl,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov	r1,r8

	mov.l	p_intsts0,r1
	mov.w	@r1,r0
	and	#0x70,r0
	cmp/eq	#0x30,r0
	bt	1f
	mov	#0,r0
	mov.l	r0,@(12,r8)		! not configured: arm again next time
	bra	player
	nop
1:	mov.l	@(12,r8),r0
	tst	r0,r0
	bf	player
	mov.l	p_gate,r1
	mov.l	@r1,r1
	tst	r1,r1
	bf	player
	mov.l	p_taskstate,r1
	mov.l	@r1,r1
	mov	#1,r0
	cmp/eq	r0,r1
	bf	player
	mov.l	p_classflags,r1
	mov.l	@(4,r1),r4		! the MIDI class's event flag
	mov.l	p_configured,r5
	mov.l	p_setflg,r1
	jsr	@r1
	nop
	mov.l	p_lens,r1
	mov	#0,r0
	mov.l	r0,@r1			! the task starts both lengths at 0xA0
	mov.l	r0,@(4,r1)
	mov	#1,r0
	mov.l	r0,@(12,r8)

player:
	mov.l	p_device,r1
	mov.b	@r1,r1
	extu.b	r1,r1
	add	#-1,r1
	mov	#4,r2
	cmp/hs	r2,r1
	bf	1f
	bra	out
	nop
1:	mov.w	p_stride,r2
	mul.l	r2,r1
	sts	macl,r1
	mov.l	p_record1,r2
	add	r2,r1			! r1 = the deck's status record

	mov	#0x22,r0
	shll2	r0
	add	#1,r0			! +0x89
	mov.b	@(r0,r1),r0
	tst	#0x40,r0
	movt	r9
	mov	#1,r2
	xor	r2,r9			! r9 = 1 while playing

	mov	#0x24,r0
	shll2	r0			! +0x90
	mov.l	@(r0,r1),r6
	cmp/pz	r6
	bf/s	1f
	extu.w	r6,r6
	mov	#0,r6			! bit 31 clear: no BPM
1:
	mov	#0x23,r0
	shll2	r0			! +0x8C
	mov.l	@(r0,r1),r2		! pitch, 0x100000 = 0 %
	dmulu.l	r2,r6
	sts	mach,r3
	sts	macl,r6
	shlr16	r6
	shlr2	r6
	shlr2	r6
	shll8	r3
	shll2	r3
	shll2	r3
	or	r3,r6			! r6 = BPM x 100 x pitch / 0x100000

	mov	#0,r10			! r10 = transport status, 0 = none
	mov.l	@(4,r8),r3
	cmp/eq	r3,r9
	bt	clocks
	mov.l	r9,@(4,r8)
	tst	r9,r9
	bt	fell
	mov	#0xFA,r10
	mov.l	@(8,r8),r3
	tst	r3,r3
	bt	1f
	mov	#0xFB,r10
1:	extu.b	r10,r10
	mov	#1,r3
	mov.l	r3,@(8,r8)		! started
	mov.l	p_period,r3
	add	#-1,r3
	mov.l	r3,@r8			! the first clock goes with the transport event
	bra	clocks
	nop
fell:	mov	#0xFC,r10
	extu.b	r10,r10

clocks:
	mov	#0,r7			! r7 = clocks due this tick
	tst	r9,r9
	bt	post
	mov.l	@r8,r3
	add	r6,r3
	mov.l	p_period,r2
1:	cmp/hs	r2,r3
	bf	2f
	sub	r2,r3
	bra	1b
	add	#1,r7
2:	mov.l	r3,@r8

post:
	mov.l	p_gate,r1
	mov.l	@r1,r1
	tst	r1,r1
	bt	out
	mov	r7,r0
	or	r10,r0
	tst	r0,r0
	bt	out
	mov.l	@(16,r8),r0
	add	r7,r0
	mov.l	r0,@(16,r8)
	tst	r10,r10
	bt	1f
	mov.l	r10,@(24,r8)		! the status before its count
	mov.l	@(20,r8),r0
	add	#1,r0
	mov.l	r0,@(20,r8)
1:	mov.l	p_classflags,r1
	mov.l	@(4,r1),r4
	mov.l	p_setflg,r1
	jsr	@r1
	mov	#2,r5

out:
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,macl
	lds.l	@r15+,mach
	lds.l	@r15+,pr
	rts
	nop

	.p2align 2
p_device:	.long 0x0AB84CFA
p_record1:	.long 0x0A371D3C + 0x124	! the record of player 1
p_period:	.long 250000
p_gate:		.long 0x10DE1FA4
p_lens:		.long 0x0BD4B0F4	! committed length per buffer
p_intsts0:	.long 0xA4D90040	! USB1 interrupt status 0
p_taskstate:	.long 0x0BD50E8C	! MIDI task state, 1 = attached
p_classflags:	.long 0x0BD4B0C8	! event flag id of each USB class
p_configured:	.long 0x20000000
p_setflg:	.long 0x08516448
p_stride:	.word 0x124
