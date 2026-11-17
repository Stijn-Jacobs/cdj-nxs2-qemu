! SPDX-License-Identifier: GPL-2.0-or-later
!
! Gate Cue for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian), the press half: notes a hot cue recalled from its pad while
! the deck is paused. main_gatecue.s acts on the release.
!
! Hook: the entry of the hot cue recall 0x08435384, after its register
! pushes. The slot is the recall's r4 (1..8 for pads A..H), at offset 16 of
! the saved r0-r7 the trampoline hands in r4. The pad pass calls the recall on
! the press edge of a pad whose slot holds a cue. On its way the recall clears
! the Player's paused flag (byte 0x0B058C10, 1 while PLAY has paused the deck;
! the PLAY key tests it to choose between play and pause), so the deck's state
! is read here, before.
!
! The deck is not playing when that flag is 1, or when the Player state word
! 0x0B058BFC is 7: the CUE key's return to the cue, and this gate's, leave the
! deck stopped at the cue with the flag cleared and state 7 (0x084330E4).
!
! A recall of slot n while the pad of slot n on the deck's own panel is down
! (Player key bank word 0x0B056EDC + 4n) arms the gate with n when the deck is
! not playing, and disarms it when the deck is playing. A recall with that pad
! up comes from elsewhere and leaves the gate as it is.
!
! The armed slot is one word at the end of the erased run; anything outside
! 1..8 means none, which includes the erased 0xFFFFFFFF it starts as.
!
! r0-r7 are free; r8-r14 are kept.

	.text
gatecuepress:
	mov.l	@(16,r4),r2
	mov	r2,r0
	add	#-1,r0
	mov	#8,r1
	cmp/hs	r1,r0
	bt	done
	mov	r2,r0
	shll2	r0
	mov.l	p_pads,r1
	add	r1,r0
	mov.l	@r0,r0
	tst	r0,r0
	bt	done
	mov.l	p_paused,r1
	mov.b	@r1,r0
	cmp/eq	#1,r0
	bt	1f
	mov.l	p_state,r1
	mov.l	@r1,r0
	cmp/eq	#7,r0
	bt	1f
	mov	#0,r2
1:	mov.l	p_armed,r1
	rts
	mov.l	r2,@r1
done:
	rts
	nop

	.p2align 2
p_pads:		.long 0x0B056EDC
p_paused:	.long 0x0B058C10
p_state:	.long 0x0B058BFC
p_armed:	.long 0x081FFFF8
