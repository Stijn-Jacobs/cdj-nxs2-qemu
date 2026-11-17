! SPDX-License-Identifier: GPL-2.0-or-later
!
! Key shift for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724, SH-4,
! little-endian), the wire half: hands the deck's semitone shift to the DSP
! in the id-block, where the DSP half of the mod reads it.
!
! Hook: DSPoutCTL's compare of its 8-word speed block against the mirror it
! last sent, 0x082DF60C. Every mode arm of the block builder joins there, so
! the block in the frame (r5 = the routine's r15, words at +0x1D4..+0x1F0) is
! finished. The builder writes word 5 as mode << 16 on every tick
! (0x082DEF08), and the id-block builder FUN_082D7C88 sends it as id-block
! word 2; its low half is 0 in every mode. The DSP parser stores word 2 whole
! and nothing in the stock DSP program reads its low half, so a stock DSP
! ignores what this writes there. The stock compare after the hook sends the
! block only when a word changed, so the shift goes out once per change.
!
! The shift is one word at the end of the erased run, 0x100 | (n & 0xFF) for
! n semitones (-12..12, main_keyshiftpad.s steps it, main_keyshiftload.s sets
! it to 0 on a load); any other value, the erased 0xFFFFFFFF it starts as
! among them, means no shift.
!
! With a shift the DSP runs its MASTER TEMPO engine whatever the MT key says.
! That engine does not engage for a speed strictly between 0xFFF2F and
! 0x1000D1 other than 1.0 (the DSP's setter 0x8000CDF8), and MAIN moves such
! speeds out of that band only while MASTER TEMPO is on (FUN_082DFCC6, which
! the block builder calls with the MT flag). With MASTER TEMPO off and a
! shift, the routine runs the same function on the speed word with the flag
! set.
!
! r0-r7 are free; r8-r14 are kept.

	.text
keyshift:
	mov.l	p_shift,r1
	mov.l	@r1,r2
	mov	r2,r0
	shlr8	r0
	cmp/eq	#1,r0
	bf	done
	mov	r2,r0
	extu.b	r0,r0
	tst	r0,r0
	bt	done
	exts.b	r0,r2
	extu.w	r2,r2
	mov.w	o_word5,r0
	mov.l	@(r0,r5),r1
	or	r2,r1
	mov.l	r1,@(r0,r5)
	mov.w	o_flags,r0
	mov.l	@(r0,r5),r1
	mov.l	p_mt,r2
	tst	r2,r1
	bf	done

	sts.l	pr,@-r15
	add	#-4,r15
	mov.w	o_speed,r0
	add	r5,r0
	mov	r0,r6
	mov.l	@r0,r4
	mov	#1,r5
	mov	r15,r7
	mov.l	p_snap,r1
	jsr	@r1
	nop
	sts	fpscr,r0
	mov.l	p_mask,r1
	and	r1,r0
	lds	r0,fpscr
	add	#4,r15
	lds.l	@r15+,pr
done:
	rts
	nop

o_speed:	.word 0x1D4
o_flags:	.word 0x1D8
o_word5:	.word 0x1E8
	.p2align 2
p_shift:	.long 0x081FFFEC
p_mt:		.long 0x40000000
p_snap:		.long 0x082DFCC6
p_mask:		.long 0xFFE7FFFF
