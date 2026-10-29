! SPDX-License-Identifier: GPL-2.0-or-later
!
! Key shift for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian), the readout: while a shift is set, the key in the title bar
! shows the shifted key and the shift, Em with +2 as F#m+2.
!
! Hook: the pad pass's mode switch 0x08442D96, after main_keyshiftpad.s, so
! a shift is shown on the pass that sets it.
!
! The title bar is drawn from the display model, which the display task copies
! into the screen message on every tick (0x0838EEE0, 0x2E3C bytes from
! 0x0B53FB50) with dispatching disabled (0x08404A38 / 0x08404A5E), sends when
! a field asks for it and then clears every field's request (0x0838F336). Its
! text fields are 0x220 bytes: a flags byte at +12 whose bit 2 is the request,
! the item kind at +0x14 (the database's: 4 title, 0x0F key), the length in
! UTF-16 units at +0x28 and the text at +0x2A. The title is the field at
! 0x0B53FBA4 and the key the one 0x132C past it. This routine writes the key
! field the same way, with dispatching disabled, so the task never copies or
! clears it halfway.
!
! The track's own key is taken from the field whenever its text is not what
! this routine last wrote, which a key the firmware writes never is while a
! shift is shown. The shifted key is spelled from it: classic names (C#m,
! Ebm, F#) by pitch class, from one table of 24 names, and Camelot names (8A,
! 11B) by seven steps a semitone, keeping the notation the export uses.
! Anything else is left as it is, and with no shift the track's own key goes
! back.
!
! The shift is 0x100 | (n & 0xFF); main_keyshift.s sends it.
!
! r0-r7 are free; r8-r14 are kept.

	.text
keyshiftkey:
	sts.l	pr,@-r15
	mov.l	p_field,r7
	mov	#0x14,r0
	mov.w	@(r0,r7),r0
	cmp/eq	#15,r0
	bf	done
	mov.l	p_state,r6
	mov	r7,r1
	add	#0x28,r1
	mov	r6,r2
	bsr	same
	add	#16,r2
	bt	compose
	mov	r7,r1
	add	#0x28,r1
	mov	r6,r2
	bsr	copy
	mov	#4,r3

compose:
	mov.l	p_shift,r1
	mov.l	@r1,r1
	mov	r1,r0
	shlr8	r0
	cmp/eq	#1,r0
	bf	keep
	exts.b	r1,r5
	tst	r5,r5
	bt	keep
	mov	r6,r1
	mov.w	@r1+,r0
	extu.w	r0,r3
	tst	r3,r3
	bt	keep
	mov	#4,r0
	cmp/hi	r0,r3
	bt	keep
	mov.w	@r1+,r0
	add	#-1,r3
	extu.w	r0,r4
	add	#-65,r4
	mov	#6,r2
	cmp/hi	r2,r4
	bt	camelot
	bra	letters
	nop

keep:
	mov.w	@r6,r0
	extu.w	r0,r0
	mov	#4,r1
	cmp/hi	r1,r0
	bt	done
	mov	r6,r1
	mov	r6,r2
	add	#16,r2
	bsr	copy
	mov	#4,r3

store:
	mov	r7,r1
	add	#0x28,r1
	mov	r6,r2
	bsr	same
	add	#16,r2
	bt	done
	mov.l	p_lock,r1
	jsr	@r1
	nop
	mov.l	r0,@-r15
	mov.l	p_field,r7
	mov.l	p_state,r1
	add	#16,r1
	mov	r7,r2
	add	#0x28,r2
	bsr	copy
	mov	#7,r3
	mov	#0,r0
	mov.w	r0,@r2
	mov.b	@(12,r7),r0
	or	#4,r0
	mov.b	r0,@(12,r7)
	mov.l	p_unlock,r1
	jsr	@r1
	mov.l	@r15+,r4
	sts	fpscr,r0
	mov.l	p_mask,r1
	and	r1,r0
	lds	r0,fpscr
done:
	lds.l	@r15+,pr
	rts
	nop

letters:
	mova	pitch,r0
	mov.b	@(r0,r4),r4
	mov	#0,r2
	tst	r3,r3
	bt	classic
	mov.w	@r1,r0
	cmp/eq	#35,r0
	bf	flat
	bra	accidental
	add	#1,r4
flat:
	cmp/eq	#98,r0
	bf	minor
	add	#-1,r4
accidental:
	add	#2,r1
	add	#-1,r3
minor:
	tst	r3,r3
	bt	classic
	mov.w	@r1,r0
	cmp/eq	#109,r0
	bf	keep
	add	#-1,r3
	tst	r3,r3
	bf	keep
	mov	#1,r2
classic:
	add	r5,r4
	add	#24,r4
	mov	#12,r0
1:	cmp/hs	r0,r4
	bf	2f
	bra	1b
	sub	r0,r4
2:	shll2	r4
	tst	r2,r2
	bf	3f
	add	#48,r4
3:	mova	names,r0
	add	r0,r4
	mov	r6,r1
	add	#18,r1
	mov	#0,r3
	mov	#3,r2
4:	mov.b	@r4+,r0
	tst	r0,r0
	bt	suffix
	mov.w	r0,@r1
	add	#2,r1
	dt	r2
	bf/s	4b
	add	#1,r3
	bra	suffix
	nop
fail:
	bra	keep
	nop

camelot:
	add	#17,r4
	mov	#9,r2
	cmp/hi	r2,r4
	bt	fail
	tst	r4,r4
	bt	fail
	tst	r3,r3
	bt	fail
	mov.w	@r1,r0
	extu.w	r0,r0
	add	#-48,r0
	cmp/hi	r2,r0
	bt	letter
	mov	r4,r2
	shll2	r4
	add	r2,r4
	add	r4,r4
	add	r0,r4
	add	#2,r1
	add	#-1,r3
	tst	r3,r3
	bt	fail
letter:
	mov	#12,r2
	cmp/hi	r2,r4
	bt	fail
	mov.w	@r1,r0
	add	#-1,r3
	tst	r3,r3
	bf	fail
	cmp/eq	#65,r0
	bt	5f
	cmp/eq	#66,r0
	bf	fail
5:	mov	r0,r2
	mov	r5,r0
	add	#24,r0
	mov	#12,r3
6:	cmp/hs	r3,r0
	bf	7f
	bra	6b
	sub	r3,r0
7:	add	#-1,r4
	tst	r0,r0
	bt	9f
8:	add	#7,r4
	cmp/hs	r3,r4
	bf	10f
	sub	r3,r4
10:	dt	r0
	bf	8b
9:	add	#1,r4
	mov	r6,r1
	add	#18,r1
	mov	#0,r3
	mov	#10,r0
	cmp/hs	r0,r4
	bf	11f
	mov	#49,r0
	mov.w	r0,@r1
	add	#2,r1
	add	#1,r3
	add	#-10,r4
11:	mov	r4,r0
	add	#48,r0
	mov.w	r0,@r1
	add	#2,r1
	mov.w	r2,@r1
	add	#2,r1
	add	#2,r3

! The title bar has room for five units after the key's symbol: a longer
! name, a three-unit key with a shift of 10 or more, goes without the shift.
suffix:
	mov	r3,r2
	mov	#43,r0
	cmp/pz	r5
	bt	12f
	mov	#45,r0
	neg	r5,r5
12:	mov.w	r0,@r1
	add	#2,r1
	add	#1,r3
	mov	#10,r0
	cmp/hs	r0,r5
	bf	13f
	mov	#49,r0
	mov.w	r0,@r1
	add	#2,r1
	add	#1,r3
	add	#-10,r5
13:	mov	r5,r0
	add	#48,r0
	mov.w	r0,@r1
	add	#1,r3
	mov	#5,r0
	cmp/hi	r0,r3
	bf	20f
	mov	r2,r3
20:	mov	r6,r1
	add	#16,r1
	bra	store
	mov.w	r3,@r1

! T = 1 when the texts at r1 and r2 (a length, then the units) are the same.
same:
	mov.w	@r1+,r0
	mov.w	@r2+,r3
	cmp/eq	r0,r3
	bf	15f
	extu.w	r0,r0
	mov	#7,r3
	cmp/hi	r3,r0
	bt	16f
	tst	r0,r0
	bt	15f
14:	mov.w	@r1+,r3
	mov.w	@r2+,r4
	cmp/eq	r3,r4
	bf	15f
	dt	r0
	bf	14b
15:	rts
	nop
16:	rts
	clrt

! The text at r1 to r2: its length, then at most r3 of its units.
copy:
	mov.w	@r1+,r0
	mov.w	r0,@r2
	add	#2,r2
	extu.w	r0,r0
	cmp/hi	r3,r0
	bf	17f
	mov	r3,r0
17:	tst	r0,r0
	bt	19f
18:	mov.w	@r1+,r4
	mov.w	r4,@r2
	dt	r0
	bf/s	18b
	add	#2,r2
19:	rts
	nop

	.p2align 2
p_field:	.long 0x0B540ED0
p_state:	.long 0x081FB500
p_shift:	.long 0x081FFFEC
p_lock:		.long 0x08404A38
p_unlock:	.long 0x08404A5E
p_mask:		.long 0xFFE7FFFF
! Minor keys first, then major, by pitch class from C.
names:
	.ascii	"Cm\0\0C#m\0Dm\0\0Ebm\0Em\0\0Fm\0\0F#m\0Gm\0\0G#m\0Am\0\0Bbm\0Bm\0\0"
	.ascii	"C\0\0\0Db\0\0D\0\0\0Eb\0\0E\0\0\0F\0\0\0F#\0\0G\0\0\0Ab\0\0A\0\0\0Bb\0\0B\0\0\0"
! The pitch class of A..G.
pitch:
	.byte	9, 11, 0, 2, 4, 5, 7
