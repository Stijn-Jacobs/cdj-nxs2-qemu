! SPDX-License-Identifier: GPL-2.0-or-later
!
! USB-MIDI transport and clock for the CDJ-2000NXS2 MAIN firmware (SH7724,
! SH-4, little-endian): send MIDI Start (FA), Stop (FC), Continue (FB) when
! the deck's play state changes, and Timing Clock (F8) at 24 per beat while
! it plays, through the rear USB-B port's own MIDI endpoint. Each message is
! one USB-MIDI event packet, 0F <status> 00 00 (cable 0, real-time byte).
!
! This is the sending half. The timing half (main_usbmiditick.s) runs on the
! firmware's 1 ms timer interrupt, counts the clocks and transport events
! that fall due there and wakes the MIDI task; this routine, in the task,
! puts them in the MIDI buffer.
!
! Hook: the top of the MIDI task's event loop, just before its wait (a hook
! on the patch engine's trampoline, see sigpatch.trampoline()). The wait
! takes any bit of the task's flag; the timer half sets one the loop drops
! for every state, so the task comes straight back here. Runs in the task
! itself, so the stock fill and commit helpers, with their lock, are used
! as the stock generator uses them. Nothing is taken from the site's
! registers.
!
! On each pass, while the host has the MIDI interface configured (the gate
! word the generator tests):
!
!   1. The buffer the task sent last becomes the fill buffer at its next swap.
!      The buffer helpers keep one committed length per buffer (words at
!      0x0BD4B0F4, send index at 0x0BD4B0F0) and never clear it, so its length
!      is zeroed here; the fill buffer's length is then whatever the generator
!      committed since the last send.
!   2. The counts the timer half has produced since the last pass, against the
!      ones sent here, give the packets: the latest transport event first,
!      then one F8 per clock.
!   3. They are appended to the fill buffer after the generator's bytes and
!      committed with the summed length. The commit sets the task's send bit,
!      so its wait returns at once and the stock code swaps and sends. At most
!      0xA0 bytes fit (the limit 0x084F5148 enforces): clocks that do not fit
!      are dropped. A pass with nothing new commits nothing.
!
! State: the timer half's nine words, 8 bytes into main_usbmiditick.bin,
! which patch_main places right after this file (mods go in name order, each
! on a 16-byte boundary, and this file is padded to one): +16 clocks due,
! +20 transport events due, +24 status of the latest event, +28 clocks sent,
! +32 transport events sent; this routine writes only the last two.
! The routine touches r0-r7 and r8-r11 (saved).

	.text
	.global usbmidi
usbmidi:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mova	tick,r0
	add	#8,r0
	mov	r0,r8			! r8 = the timer half's state

	mov.l	p_gate,r1
	mov.l	@r1,r1
	tst	r1,r1
	bt	out
	mov.l	p_bufidx,r1
	mov.w	@r1,r0
	shll2	r0
	mov.l	p_lens,r2
	mov	#0,r3
	mov.l	r3,@(r0,r2)		! the buffer sent last: its length is spent

	mov	#0,r10			! r10 = transport status, 0 = none
	mov.l	@(20,r8),r0
	mov.l	@(32,r8),r1
	cmp/eq	r0,r1
	bt	1f
	mov.l	r0,@(32,r8)
	mov.l	@(24,r8),r10
1:	mov.l	@(16,r8),r0
	mov.l	@(28,r8),r9
	mov.l	r0,@(28,r8)
	sub	r9,r0
	mov	r0,r9			! r9 = clocks
	tst	r10,r10
	movt	r3
	mov	#1,r2
	sub	r3,r2			! 1 when a transport packet goes first
	add	r2,r9			! r9 = packets
	tst	r9,r9
	bt	out

	mov.l	p_fill,r1
	jsr	@r1
	nop
	mov	r0,r4
	mov.l	p_bufidx,r1
	mov.w	@(2,r1),r0
	shll2	r0
	mov.l	p_lens,r2
	mov.l	@(r0,r2),r11		! r11 = bytes the generator committed
	mov	#40,r1
	shll2	r1
	sub	r11,r1
	shlr2	r1			! r1 = packets that still fit
	cmp/hi	r1,r9
	bf	1f
	mov	r1,r9
1:	tst	r9,r9
	bt	out
	add	r11,r4
	mov	r9,r7
	mov	#0x0F,r1
	mov	#0,r3
2:	mov	r10,r2
	tst	r2,r2
	bf	3f
	mov	#0xF8,r2
	extu.b	r2,r2
3:	mov	#0,r10
	mov.b	r1,@r4
	add	#1,r4
	mov.b	r2,@r4
	add	#1,r4
	mov.b	r3,@r4
	add	#1,r4
	mov.b	r3,@r4
	add	#1,r4
	dt	r7
	bf	2b
	mov	r9,r4
	shll2	r4			! 4 bytes per packet
	add	r11,r4
	mov.l	p_commit,r1
	jsr	@r1
	nop

out:
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

	.p2align 2
p_gate:		.long 0x10DE1FA4
p_fill:		.long 0x084F511E
p_commit:	.long 0x084F5148
p_bufidx:	.long 0x0BD4B0F0	! send index, fill index
p_lens:		.long 0x0BD4B0F4	! committed length per buffer

	.p2align 4
tick:					! main_usbmiditick starts here
