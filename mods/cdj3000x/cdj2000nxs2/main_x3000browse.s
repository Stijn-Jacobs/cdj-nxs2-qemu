! SPDX-License-Identifier: GPL-2.0-or-later
!
! A local cursor for the browse list Serato shows on a CDJ-3000X, for the
! CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4, little-endian).
!
! Serato answers each step of the browse encoder with a list of its own, 38 to
! 64 ms (median 51) after the state report carrying it, so the list follows
! the encoder that much late. Its answer is predictable from the step alone:
! it sends 11 rows with the cursor four rows down, and the deck shows eight
! of them (x3000rxview), so the rows a step brings on screen are mostly ones
! the deck already has. x3000rxview keeps Serato's last answer and shows it
! with every step not answered yet applied (its show and step); this hook
! counts the steps and shows the prediction as soon as the encoder moves.
!
! Hook: the HID IN task, after x3000in has put a 0x20 report in the 3000X
! layout (the encoder a u16 at bytes 20-21, byte 5 bit 7 while the library is
! open), before it goes to the IN pipe. Each step taken while x3000rxview has
! a library list on screen goes into a ring of 32 (1 down, -1 up), as far as
! 32 not answered; x3000rxview answers them one per list, oldest first. The
! prediction goes to the report framework 0x084F9DB2 the way x3000rxview
! hands it a list. The framework takes no lock and waits on nothing for a
! list; its only kernel call is set_flg, which any task may make.
!
! The HID OUT task runs at priority 3 and this one at 4, so it can preempt
! this hook at any instruction but never the other way round. A guard byte
! taken with tas.b covers the list record, Serato's last answer and the
! counters: x3000rxview finds it taken only while this hook holds it, and
! then commits its list at its next report instead of waiting. Here the
! guard is always free; if it is not, the steps stay counted and the next
! report shows them.
!
! At most one list every 30 ms, about a display frame: a step inside that
! shows with the next report or with Serato's answer. A step Serato does not
! answer (at the top or the bottom of a crate) is dropped 150 ms after the
! last step, so the deck goes back to Serato's last answer; its slowest
! answer took 64 ms.
!
! The guard is erased flash (0xFF, taken) until the first report clears it.
!
! r4 = the saved r0-r7: r0 = slot index x 4 at offset 0, r6 = length at 24,
! r7 = the slot pointer table at 28. r0-r7 are free; r8-r9 are saved.

	.text
x3000browse:
	mov.l	@(24,r4),r6
	mov	#30,r1
	cmp/ge	r1,r6
	bf	9f
	mov	#64,r1
	cmp/gt	r1,r6
	bt	9f
	mov.l	@r4,r0
	mov.l	@(28,r4),r1
	mov.l	@(r0,r1),r5
	mov.b	@(1,r5),r0
	cmp/eq	#0x20,r0
	bt	report
9:	rts
	nop
report:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	p_on,r1
	mov.l	@r1,r0
	cmp/eq	#1,r0
	bt	1f
	mov.l	p_guard,r2
	mov	#0,r0
	mov.b	r0,@r2
	mov.l	p_owed,r2
	mov.l	r0,@r2
	mov	#1,r0
	mov.l	r0,@r1
1:	mov.l	p_state,r1
	mov.l	@r1,r0
	cmp/eq	#2,r0
	bf	out
	mov.l	p_auth,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	out

	! r8 the time in ms, r3 the steps the encoder moved (none the first time)
	mov.l	p_tick,r1
	mov.l	@r1,r8
	mov	r5,r1
	add	#20,r1
	mov.b	@r1+,r0
	extu.b	r0,r2
	mov.b	@r1,r0
	extu.b	r0,r0
	shll8	r0
	or	r0,r2
	mov.l	p_encoder,r1
	mov.l	@r1,r3
	mov.l	r2,@r1
	mov	r3,r0
	shlr16	r0
	tst	r0,r0
	bf	due
	sub	r3,r2
	exts.w	r2,r3
	tst	r3,r3
	bt	due
	mov.b	@(5,r5),r0
	tst	#0x80,r0
	bt	due
	mov.l	p_live,r1
	mov.l	@r1,r0
	cmp/eq	#1,r0
	bf	due
	mov	#1,r1
	cmp/pz	r3
	bt	2f
	neg	r3,r3
	mov	#-1,r1
	! r4 the steps counted, r5 answered, r6 the ring
2:	mov.l	p_steps,r4
	mov.l	p_answered,r5
	mov.l	p_ring,r6
3:	mov.l	@r4,r0
	mov.l	@r5,r2
	sub	r2,r0
	mov	#32,r2
	cmp/ge	r2,r0
	bt	4f
	mov.l	@r4,r0
	and	#31,r0
	mov.b	r1,@(r0,r6)
	mov.l	@r4,r0
	add	#1,r0
	dt	r3
	bf/s	3b
	mov.l	r0,@r4
4:	mov.l	p_steptime,r1
	mov.l	r8,@r1

due:
	mov.l	p_guard,r9
	tas.b	@r9
	bf	out
	mov.l	p_live,r1
	mov.l	@r1,r0
	cmp/eq	#1,r0
	bf	release
	mov.l	p_steps,r4
	mov.l	p_answered,r5
	mov.l	@r4,r0
	mov.l	@r5,r1
	sub	r1,r0
	cmp/pl	r0
	bf	5f
	mov.l	p_steptime,r2
	mov.l	@r2,r0
	mov	r8,r2
	sub	r0,r2
	mov.w	c_unanswered,r0
	cmp/hs	r0,r2
	bf	5f
	mov.l	r1,@r4
	! r6 the steps counted, shown when they changed since the last list
5:	mov.l	@r4,r6
	mov.l	p_shown,r1
	mov.l	@r1,r0
	cmp/eq	r6,r0
	bt	release
	mov.l	p_showntime,r2
	mov.l	@r2,r0
	mov	r8,r3
	sub	r0,r3
	mov	#30,r0
	cmp/hs	r0,r3
	bf	release
	mov.l	r6,@r1
	mov.l	p_show,r1
	mov.l	@r1,r1
	jsr	@r1
	nop
	tst	r0,r0
	bt	release
	mov.l	p_showntime,r1
	mov.l	r8,@r1
release:
	mov	#0,r0
	mov.b	r0,@r9
out:
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

! how long a step waits for its answer, in ms
c_unanswered:	.word 150

	.p2align 2
p_on:		.long 0x081FB540
p_guard:	.long 0x081FB544
p_owed:		.long 0x081FB56C
p_state:	.long 0x0BD50E90
p_auth:		.long 0x0BD4B144
p_tick:		.long 0x0B0C5258
p_encoder:	.long 0x081FB570
p_live:		.long 0x081FB550
p_steps:	.long 0x081FB54C
p_answered:	.long 0x081FB548
p_ring:		.long 0x081FB520
p_steptime:	.long 0x081FB574
p_shown:	.long 0x081FB57C
p_showntime:	.long 0x081FB578
p_show:		.long 0x081FB554
