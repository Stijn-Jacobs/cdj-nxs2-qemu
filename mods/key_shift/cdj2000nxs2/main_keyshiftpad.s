! SPDX-License-Identifier: GPL-2.0-or-later
!
! Key shift for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian), the control: while MASTER TEMPO is held, pad A lowers the
! shift by a semitone, pad B raises it and pad C puts it back to 0, within
! -12..12. Those pads do nothing else while MASTER TEMPO is held.
!
! Hook: the entry of the pad pass's mode switch 0x08442D96, which the pad
! pass 0x08443050 calls on every Player pass once the pad edges are queued,
! before any pad action is taken.
!
! MASTER TEMPO's key state is the word the panel decoder sets to 1 while the
! key is down, 0x0B54CB0C. The pad of slot n (1..8 for pads A..H) on the
! deck's own panel is the Player key bank word 0x0B056EDC + 4n. A press is
! taken on its edge against the pads' state on the pass before, which this
! routine keeps in bits 0-2 of a word of its own.
!
! The switch acts on a pad in one of two ways, by the pad mode word
! 0x0B05651C. In mode 0 it takes the head of the edge queue 0x0B056BB4 and
! skips it when it equals the last one it handled, 0x10DE00F8; in the other
! modes it compares a pad's word with its copy from the pass before,
! 0x0B057CD8 + 4n. While MASTER TEMPO is held, pads A..C are taken from both:
! a queue head of slot 1..3 is marked handled, and the copies of pads A..C
! are set to the pads' words.
!
! The shift is 0x100 | (n & 0xFF); main_keyshift.s sends it.
!
! r0-r7 are free; r8-r14 are kept.

	.text
keyshiftpad:
	mov.l	p_pads,r1
	mov.l	@(4,r1),r0
	tst	r0,r0
	movt	r3
	mov.l	@(8,r1),r0
	tst	r0,r0
	movt	r0
	add	r0,r0
	or	r0,r3
	mov.l	@(12,r1),r0
	tst	r0,r0
	movt	r0
	shll2	r0
	or	r0,r3
	mov	#7,r0
	xor	r0,r3
	mov.l	p_seen,r2
	mov.l	@r2,r6
	mov.l	r3,@r2
	mov.l	p_mtkey,r2
	mov.l	@r2,r0
	cmp/eq	#1,r0
	bf	done

	not	r6,r6
	and	r3,r6
	mov.l	p_shift,r2
	mov.l	@r2,r0
	mov	r0,r7
	shlr8	r7
	mov	#1,r5
	cmp/eq	r5,r7
	bt	1f
	mov	#0,r0
1:	exts.b	r0,r7
	mov	r6,r0
	tst	#1,r0
	bt	2f
	mov	#-12,r5
	cmp/gt	r5,r7
	bf	2f
	add	#-1,r7
2:	tst	#2,r0
	bt	3f
	mov	#12,r5
	cmp/ge	r5,r7
	bt	3f
	add	#1,r7
3:	tst	#4,r0
	bt	4f
	mov	#0,r7
4:	extu.b	r7,r7
	mov	#1,r5
	shll8	r5
	or	r5,r7
	mov.l	r7,@r2

	mov.l	p_mode,r2
	mov.l	@r2,r0
	tst	r0,r0
	bf	5f
	mov.l	p_queue,r2
	mov.l	@r2,r0
	add	#-1,r0
	mov	#3,r5
	cmp/hs	r5,r0
	bt	done
	add	#1,r0
	mov.l	p_last,r2
	rts
	mov.l	r0,@r2

5:	mov.l	p_prev,r2
	mov.l	@(4,r1),r0
	mov.l	r0,@(4,r2)
	mov.l	@(8,r1),r0
	mov.l	r0,@(8,r2)
	mov.l	@(12,r1),r0
	mov.l	r0,@(12,r2)
done:
	rts
	nop

	.p2align 2
p_pads:		.long 0x0B056EDC
p_prev:		.long 0x0B057CD8
p_mtkey:	.long 0x0B54CB0C
p_mode:		.long 0x0B05651C
p_queue:	.long 0x0B056BB4
p_last:		.long 0x10DE00F8
p_seen:		.long 0x081FFFE8
p_shift:	.long 0x081FFFEC
