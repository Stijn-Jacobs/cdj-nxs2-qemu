! SPDX-License-Identifier: GPL-2.0-or-later
!
! The beat segment fields of the control report, for the CDJ-2000NXS2 MAIN
! firmware 1.87 (SH7724, SH-4, little-endian).
!
! The NXS2 reads four u16 at bytes 47, 49, 51 and 53 of the control report
! (type 0x21): the length of the bar and the position in it, for the deck's
! own row of the beat boxes, then the same for the second row. It shows the
! quarter of the length the position falls in. A CDJ-3000X sends none of them
! (the control report has the track's BPM there, which x3000rxview clears);
! a 3000X works the beat out itself from the beat grid and the position.
!
! Hooked after x3000rxview on the read's end, so the stock buffer holds the
! control report in the NXS2's layout with the held position. The beat grid
! message x3000rxview hands to the framework stays as it came in the grid
! holder: a u16 count at +16, then 4-byte entries (beat in bar 1 to 4, the
! time in ms, 24 bits little-endian) from +18. The entry of the beat the
! position is in is found by a binary search, the length of the bar is four
! times that beat's length (the next entry's time minus its own, the last
! entry takes the one before) and the position in it is the beats gone by
! plus the time since the beat, so the quarter shown is the beat in the bar
! and the box fills as the beat goes. Before the first beat, with no grid, or
! for a beat longer than 16 s the fields stay as x3000rxview left them (zero).
! The second row is left at zero.
!
! r4 = the saved r0-r7, r5 at offset 20. r0-r7 are free; r8-r11 are saved.

	.text
x3000segments:
	sts.l	pr,@-r15
	movt	r0
	mov.l	r0,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	@(20,r4),r0
	mov	#55,r1
	cmp/hs	r1,r0
	bf	done
	mov.l	p_state,r1
	mov.l	@r1,r0
	cmp/eq	#2,r0
	bf	done
	mov.l	p_auth,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	done
	mov.l	p_stock,r8
	mov.b	@(1,r8),r0
	cmp/eq	#0x21,r0
	bf	done
	mov.l	p_grid,r1
	mov.w	@(16,r1),r0
	extu.w	r0,r11
	tst	r11,r11
	bt	done
	add	#18,r1

	! r9 the position in ms
	mov.b	@(11,r8),r0
	extu.b	r0,r2
	mov.w	c_minute,r3
	mulu.w	r3,r2
	sts	macl,r9
	mov.b	@(12,r8),r0
	extu.b	r0,r2
	mov.w	c_second,r3
	mulu.w	r3,r2
	sts	macl,r2
	add	r2,r9
	mov.b	@(13,r8),r0
	extu.b	r0,r2
	mov.b	@(14,r8),r0
	extu.b	r0,r3
	shll8	r3
	or	r3,r2
	add	r2,r9

	! r2 the first entry after the position
	mov	#0,r2
	mov	r11,r3
1:	cmp/hs	r3,r2
	bt	3f
	mov	r2,r4
	add	r3,r4
	shlr	r4
	mov	r4,r5
	shll2	r5
	bsr	time_at
	add	r1,r5
	cmp/hi	r9,r6
	bt	2f
	add	#1,r4
	bra	1b
	mov	r4,r2
2:	bra	1b
	mov	r4,r3

	! r2 the entry of the beat, r3 its time, r9 the time since it
3:	tst	r2,r2
	bt	done
	add	#-1,r2
	mov	r2,r5
	shll2	r5
	mov	r5,r0
	mov.b	@(r0,r1),r10
	bsr	time_at
	add	r1,r5
	mov	r6,r3
	sub	r3,r9

	! r6 the length of the beat
	mov	r2,r4
	add	#1,r4
	cmp/hs	r11,r4
	bt	4f
	mov	r4,r5
	shll2	r5
	bsr	time_at
	add	r1,r5
	bra	5f
	sub	r3,r6
4:	tst	r2,r2
	bt	done
	mov	r2,r5
	add	#-1,r5
	shll2	r5
	bsr	time_at
	add	r1,r5
	sub	r6,r3
	mov	r3,r6
5:	tst	r6,r6
	bt	done
	mov.w	c_longest,r0
	cmp/hi	r0,r6
	bt	done
	cmp/hs	r6,r9
	bf	6f
	mov	r6,r9
	add	#-1,r9
6:	extu.b	r10,r0
	add	#-1,r0
	and	#3,r0
	mulu.w	r6,r0
	sts	macl,r7
	add	r9,r7
	mov	r6,r5
	shll2	r5
	mov	r8,r1
	add	#47,r1
	mov	r5,r0
	mov.b	r0,@r1
	shlr8	r0
	mov.b	r0,@(1,r1)
	mov	r7,r0
	mov.b	r0,@(2,r1)
	shlr8	r0
	mov.b	r0,@(3,r1)
done:
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	mov.l	@r15+,r7
	lds.l	@r15+,pr
	rts
	shlr	r7

	! r5 the entry, r6 its time in ms; r0 and r7 are used
time_at:
	mov.w	@r5,r0
	extu.w	r0,r6
	shlr8	r6
	mov.w	@(2,r5),r0
	extu.w	r0,r7
	shll8	r7
	rts
	or	r7,r6

c_minute:	.word 60000
c_second:	.word 1000
! the longest beat taken, so that four of them fit the u16 length
c_longest:	.word 16000

	.p2align 2
p_stock:	.long 0x0BD50FDC
p_state:	.long 0x0BD50E90
p_auth:		.long 0x0BD4B144
p_grid:		.long 0x0BF5BFFC
