! SPDX-License-Identifier: GPL-2.0-or-later
!
! Real 3-band detail waveform data for the CDJ-2000NXS2 MAIN firmware
! (SH7724, SH-4, little-endian). MAIN reads the track's own PWV7 (3-band
! detail) tag from its .2EX and packs one 3-band word per entry for
! main_wave3detail.s, which writes the words into the waveform record the
! deck sends to the display.
!
! A tag fetch hands back a 20-byte record, not the tag: +0 entry count, +4
! payload bytes, +8 bytes per entry, +16 the payload (the entries, after
! the tag header), which sits in the same block at +20. The size it reports
! is 20 + the payload bytes.
!
! Each word is high5<<11 | mid5<<6 | low5<<1 | 1, in MAIN's native byte
! order. The firmware re-encodes the builder's PWV5 record into its own
! 150-entries-a-second form afterwards and leaves bits 1-0 of every word
! clear, so bit 0 marks a word as 3-band for the display and stock words
! never carry it. Only a PWV7 with the same entry count as PWV5 is used.
!
! Hook: the detail-waveform builder (the function that also drives PWV5)
! fetches PWV5 through a generic ANLZ tag primitive while the fetch's own
! ANLZ handle is still open in a register. This mod's call site sits right
! after that PWV5 fetch returns, replacing the instructions that carry the
! fetch's own hash argument and restore the register the resumed code needs
! for its own close-handle call afterward -- both reproduced verbatim below
! before anything new happens, so the RGB detail waveform keeps working
! exactly as before whether or not this mod's own fetch succeeds.
!
! fetch_pwv7 is the same generic-fetch routine the firmware itself uses for
! PWV5 (same handle, same hash key, same reply-wait and own-copy-allocation
! dance), transcribed instruction-for-instruction with only the tag/extension
! string swapped -- not reverse-engineered logic of ours, so it inherits that
! routine's own correctness.
!
! Each raw PWV7 byte validates to 0..128 (not a 7-bit mask); low/mid pack as
! ceil(31*v/128) floored at 1, high through the curve5 table (129 entries,
! ceil(31*C(high)) floored at 1).
!
! A missing .2EX, a missing tag, a count other than PWV5's or a failed
! allocation leaves the stock waveform alone.
!
! The packed words are handed over through two words at 0x081FFFF4 (the
! buffer) and 0x081FFFF8 (its entry count), next to the overview's word at
! 0x081FFFFC: the last words of the erased run the mods are placed in, which
! the placed routines never reach. The buffer reads 0xFFFFFFFF until the
! first load and 0 when the current track has no detail data; it is set only
! once the buffer is complete, and the previous track's is dropped first.

	.text
wave3band:
	! The blob is copied into its cave as it is, so every .long below that
	! names a label in this file holds an offset from wave3band until this
	! loop adds the address the blob was placed at (r3). A word already at
	! or past the blob's size has been done by an earlier call. r4, r10 and
	! r14 are live into the call site, so they are left alone.
	mova	p_pwv5rd,r0
	mov.l	p_poolofs,r1
	sub	r1,r0
	mov	r0,r3
	mova	reloc_tab,r0
	mov	r0,r2
	mov.l	p_blobsize,r5
reloc_next:
	mov.w	@r2+,r0			! offset of the next pool word, 0 ends the table
	tst	r0,r0
	bt	reloc_done
	add	r3,r0
	mov.l	@r0,r1
	cmp/hs	r5,r1
	bt	reloc_next
	add	r3,r1
	bra	reloc_next
	mov.l	r1,@r0
reloc_done:
	! bsr/jsr all clobber pr, so save the return address this routine was
	! entered with before the first call, and restore it right before this
	! routine's own rts -- in static storage, not the stack, since this
	! call site's own stack frame (used below and again by the resumed
	! code) is not this mod's to resize.
	sts	pr,r0
	mov.l	p_savedpr,r1
	mov.l	r0,@r1

	! -- reproduce the instructions this call site replaces, so the
	! existing PWV5 fetch (and the close-handle call it leads into) keeps
	! working unchanged: r6 = &out_size, r7 = &out_ptr, r5 = the hash this
	! call already carried in r10, and the fetch's delay-slot store of the
	! FPSCR mask. r10 keeps the hash for the PWV7 fetch below (the
	! fetch primitive files it in its request record and waits for the
	! reply under it; a zero there never gets its reply) and is switched
	! to the close-handle address only at restorepr.
	mov	r15,r7
	mov	r15,r6
	add	#8,r7
	mov	r10,r5
	add	#12,r6
	mov.l	p_pwv5rd,r1
	jsr	@r1
	mov.l	r14,@(20,r15)

	! Drop the buffer the previous track left behind, so a load that fails
	! partway sends no 3-band words rather than the last track's. The cell
	! is cleared first: its reader runs in another task.
	mov.l	p_detail,r0
	mov.l	@r0,r1
	mov	#0,r2
	mov.l	r2,@r0
	cmp/pl	r1			! 0 = none, -1 = never written
	bf	1f
	mov.l	p_free,r2
	jsr	@r2
	mov	r1,r4
1:
	mov.l	@(16,r15),r4		! the ANLZ handle this call site opened
	tst	r4,r4
	bt	restorepr

	! The PWV5 record the fetch above left at [r15+8] gives the entry count.
	mov.l	@(8,r15),r2
	tst	r2,r2
	bt	restorepr
	mov.w	@(8,r2),r0
	cmp/eq	#2,r0			! PWV5 entries are 2 bytes
	bf	restorepr
	mov.l	@r2,r0			! n
	tst	r0,r0
	bt	restorepr
	mov.l	p_scratch,r1
	mov.l	r0,@(12,r1)		! scratch.n = n
	mov	#0,r0
	mov.l	r0,@r1			! the fetch reads its out_ptr on some failures
	mov.l	r0,@(4,r1)
	mov	r10,r5			! the PWV5 fetch's own hash, see above
	mov	r1,r7			! r7 = &scratch.ptr (out_ptr)
	mov	r1,r6
	add	#4,r6			! r6 = &scratch.size (out_size)
	bsr	fetch_pwv7
	nop

	mov.l	p_scratch,r0
	mov.l	@r0,r1			! the PWV7 record
	tst	r1,r1
	bt	restorepr		! no PWV7 tag on this track
	mov.l	@r1,r2
	mov.l	@(12,r0),r3
	cmp/eq	r3,r2			! same entry count as PWV5?
	bf	pwv7_freeraw
	mov.w	@(8,r1),r0
	cmp/eq	#3,r0			! PWV7 entries are 3 bytes
	bf	pwv7_freeraw

	mov.l	p_scratch,r0
	mov.l	@(12,r0),r4
	shll	r4			! 2 bytes per packed word
	mov.l	p_malloc,r2
	jsr	@r2
	nop
	tst	r0,r0
	bt	pwv7_freeraw
	mov.l	p_scratch,r1
	mov.l	r0,@(8,r1)		! scratch.words = the packed buffer
	mov	r0,r4			! r4 = packed dst
	mov.l	@r1,r5
	mov.l	@(16,r5),r5		! r5 = the PWV7 entries
	mov.l	@(12,r1),r6		! r6 = n, loop counter
pwv7_pack:
	! low/mid validate to 0..128 (not a 7-bit mask -- 128 is in range) and
	! pack as ceil(31*v/128) floored at 1, so a nonzero byte never packs to
	! a zero-height band; 128 is a compile-time power of two, so this is a
	! shift, not a divide.
	mov.b	@r5,r0
	extu.b	r0,r0
	mov	#64,r1
	shll	r1
	cmp/hs	r1,r0
	bf	20f
	mov	r1,r0
20:					! r0 = low, clamped 0..128
	mov	#31,r1
	mul.l	r1,r0
	sts	macl,r0
	add	#127,r0
	mov	#-7,r1
	shld	r1,r0			! r0 = (31*low+127) >> 7
	tst	r0,r0
	bf	21f
	mov	#1,r0
21:					! r0 = low5, floored at 1
	shll	r0
	or	#1,r0			! low5 << 1, and the 3-band flag
	mov	r0,r2

	mov.b	@(1,r5),r0
	extu.b	r0,r0
	mov	#64,r1
	shll	r1
	cmp/hs	r1,r0
	bf	22f
	mov	r1,r0
22:					! r0 = mid, clamped
	mov	#31,r1
	mul.l	r1,r0
	sts	macl,r0
	add	#127,r0
	mov	#-7,r1
	shld	r1,r0
	tst	r0,r0
	bf	23f
	mov	#1,r0
23:					! r0 = mid5, floored at 1
	shll2	r0
	shll2	r0
	shll2	r0			! << 6
	or	r0,r2

	mov.b	@(2,r5),r0
	extu.b	r0,r0
	mov	#64,r1
	shll	r1
	cmp/hs	r1,r0
	bf	24f
	mov	r1,r0
24:					! r0 = high, clamped -- direct index,
					! no mask: curve5 is 129 entries (0..128)
					! and already ceil(31*C(h)) floored at 1.
	mov.l	p_curve5,r1
	mov.b	@(r0,r1),r0
	extu.b	r0,r0
	shll8	r0
	shll2	r0
	shll	r0			! << 11
	or	r0,r2

	mov.w	r2,@r4
	add	#3,r5
	add	#2,r4
	dt	r6
	bf	pwv7_pack

	mov.l	p_scratch,r0
	mov.l	@(12,r0),r1
	mov.l	p_detail,r2
	mov.l	r1,@(4,r2)
	mov.l	@(8,r0),r1
	mov.l	r1,@r2

pwv7_freeraw:
	mov.l	p_scratch,r0
	mov.l	@r0,r1
	tst	r1,r1
	bt	restorepr
	mov.l	p_free,r2
	jsr	@r2
	mov	r1,r4

restorepr:
	mov.l	p_savedpr,r0
	mov.l	@r0,r1
	lds	r1,pr
	mov.l	p_close,r10
	rts
	nop

	.p2align 2
p_pwv5rd:	.long 0x08289E98	! MAIN's own PWV5/EXT reader (v1.87)
p_close:	.long 0x0827A34E	! the ANLZ close-handle call the resumed code makes
p_malloc:	.long 0x08344A9C
p_free:		.long 0x08344B42
p_detail:	.long 0x081FFFF4
p_scratch:	.long scratch
p_savedpr:	.long savedpr
p_curve5:	.long curve5
p_poolofs:	.long p_pwv5rd - wave3band
p_blobsize:	.long blob_end - wave3band

! Offsets of the pool words that hold a label of this file, for the loop
! at the top of wave3band.
	.p2align 2
reloc_tab:
	.word p_scratch - wave3band, p_savedpr - wave3band, p_curve5 - wave3band
	.word p_tag_pwv7 - wave3band, p_ext_2ex_7 - wave3band
	.word 0

	! -- PWV7 fetch, the firmware's own generic-tag-fetch routine
	! (transcribed, see the header comment) reading "PWV7"/"2EX" instead
	! of "PWV5"/"EXT". In: r4 = handle, r5 = the hash, r6 = &out_size, r7 = &out_ptr.
fetch_pwv7:
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	sts.l	macl,@-r15
	add	#-60,r15
	mov	#0,r2
	mov	r7,r13
	cmp/eq	r2,r4
	mov	r6,r14
	mov.l	r2,@(8,r15)
	mov.l	r2,@(4,r15)
	bt/s	1f
	mov.l	r2,@r15
	cmp/eq	r2,r14
	bt	1f
	cmp/eq	r2,r13
	bf	2f
1:
	bra	13f
	mov	#-1,r0
2:
	mov	r15,r1
	mov.l	r1,@-r15
	mov	r15,r6
	add	#8,r6
	mov.l	r6,@-r15
	mov	r15,r7
	add	#16,r7
	mov.l	r7,@-r15
	mov.l	p_fetchfn_7,r2
	mov.l	p_ext_2ex_7,r7
	mov.l	p_tag_pwv7,r6
	jsr	@r2
	nop	
	sts	fpscr,r2
	mov.l	p_fpmask_7,r1
	mov.l	r0,@(52,r15)
	mov.l	r1,@(56,r15)
	and	r1,r2
	lds	r2,fpscr
	cmp/pz	r0
	bt/s	3f
	add	#12,r15
	bra	11f
	nop	
3:
	mov.l	@(4,r15),r5
	mov	#0,r1
	cmp/eq	r1,r5
	bf	4f
	bra	12f
	nop	
4:
	mov.l	p_helper1_7,r2
	mov	r15,r4
	mov	#0,r7
	add	#12,r4
	mov	#4,r6
	jsr	@r2
	mov.l	r7,@(12,r15)
	sts	fpscr,r1
	mov.l	@(44,r15),r2
	mov.l	@(4,r15),r4
	add	#4,r4
	and	r2,r1
	lds	r1,fpscr
	mov	#0,r1
	cmp/eq	r1,r4
	bf/s	5f
	mov.l	r4,@(48,r15)
	bra	9f
	nop	
5:
	mov.l	@(12,r15),r6
	tst	r6,r6
	bf	6f
	bra	9f
	nop	
6:
	mov.l	p_helper1_7,r7
	mov	r15,r4
	mov.l	@(48,r15),r5
	add	#16,r4
	jsr	@r7
	mov	#24,r6
	sts	fpscr,r2
	mov.l	@(44,r15),r1
	mov.l	p_helper2_7,r7
	mov	r15,r4
	mov.l	p_tag_pwv7,r5
	add	#16,r4
	mov	#4,r6
	and	r1,r2
	jsr	@r7
	lds	r2,fpscr
	sts	fpscr,r4
	tst	r0,r0
	mov.l	@(44,r15),r2
	and	r2,r4
	bt/s	7f
	lds	r4,fpscr
	bra	9f
	nop	
7:
	mov.l	@(20,r15),r7
	mov	#-1,r4
	shll8	r4
	shll16	r4
	mov	r7,r5
	mov	r7,r1
	and	r4,r5
	shlr16	r5
	shlr8	r5
	shlr8	r4
	extu.b	r5,r2
	mov	r7,r5
	and	r4,r1
	shlr8	r4
	shlr8	r1
	and	r4,r5
	and	r4,r1
	shll8	r5
	shll8	r4
	shll16	r7
	or	r1,r2
	and	r4,r5
	shll8	r7
	shll8	r4
	or	r5,r2
	and	r4,r7
	or	r7,r2
	mov.l	@(24,r15),r7
	mov.l	r2,@(20,r15)
	mov	r7,r2
	mov	r7,r5
	and	r4,r2
	shlr16	r2
	shlr8	r2
	shlr8	r4
	extu.b	r2,r1
	mov	r7,r2
	and	r4,r5
	shlr8	r4
	shlr8	r5
	and	r4,r2
	and	r4,r5
	shll8	r2
	shll8	r4
	shll16	r7
	or	r5,r1
	and	r4,r2
	shll8	r7
	shll8	r4
	or	r2,r1
	and	r4,r7
	or	r7,r1
	mov.l	r1,@(24,r15)
	mov	#28,r1
	mov.l	p_mask_ff00_7,r2
	add	r15,r1
	mov.l	r2,@(52,r15)
	mov.w	@r1,r0
	extu.w	r0,r5
	mov.w	@(2,r1),r0
	extu.b	r5,r6
	and	r2,r5
	mov	#-8,r7
	shad	r7,r5
	mov.w	p_ff00_7,r7
	shll8	r6
	extu.b	r5,r5
	and	r7,r6
	or	r6,r5
	mov.w	r5,@r1
	extu.w	r0,r5
	extu.b	r5,r6
	and	r2,r5
	mov	#-8,r2
	shad	r2,r5
	mov.l	@(32,r15),r2
	shll8	r6
	extu.b	r5,r0
	and	r7,r6
	or	r6,r0
	mov	r2,r6
	and	r4,r6
	mov.w	r0,@(2,r1)
	shlr8	r4
	mov.w	@(8,r1),r0
	shlr16	r6
	and	r4,r2
	shlr8	r6
	shlr8	r2
	shlr8	r4
	extu.b	r6,r5
	and	r4,r2
	or	r2,r5
	mov.l	@(32,r15),r2
	mov	r2,r6
	shll16	r2
	and	r4,r6
	shll8	r6
	shll8	r4
	and	r4,r6
	or	r6,r5
	shll8	r2
	shll8	r4
	extu.w	r0,r6
	and	r4,r2
	mov.l	@(52,r15),r4
	or	r2,r5
	mov.l	r5,@(32,r15)
	mov	r6,r5
	extu.b	r6,r6
	and	r4,r5
	mov	#-8,r4
	shad	r4,r5
	mov.l	@(24,r15),r4
	shll8	r6
	extu.b	r5,r0
	and	r7,r6
	or	r6,r0
	mov.w	r0,@(8,r1)
	mov.l	@(20,r15),r1
	cmp/hs	r4,r1
	bt	9f
	mov.l	@(32,r15),r6
	tst	r6,r6
	bt	9f
	mov.w	@(30,r15),r0
	mov.l	@(32,r15),r2
	extu.w	r0,r7
	mov.l	@(24,r15),r4
	mul.l	r7,r2
	mov.l	@(20,r15),r5
	sts	macl,r1
	sub	r5,r4
	cmp/hs	r1,r4
	bf	9f
	add	#20,r4
	tst	r4,r4
	bt/s	11f
	mov.l	r4,@r14
	mov.l	p_malloc_7,r2
	jsr	@r2
	nop	
	sts	fpscr,r5
	mov	#0,r1
	cmp/eq	r1,r0
	mov.l	@(44,r15),r2
	mov	r0,r4
	mov.l	r0,@r13
	and	r2,r5
	bt/s	8f
	lds	r5,fpscr
	mov.l	p_memset_7,r7
	mov	#0,r5
	jsr	@r7
	mov.l	@r14,r6
	mov.l	@r13,r4
	mov.l	@(32,r15),r2
	mov.l	r2,@r4
	mov.l	@r13,r5
	mov.w	@(30,r15),r0
	mov.w	r0,@(8,r5)
	mov	#36,r0
	mov.l	@r13,r7
	mov.l	@(24,r15),r14
	mov.l	@(20,r15),r4
	sub	r4,r14
	mov.l	r14,@(4,r7)
	mov.l	@r13,r1
	mov.w	@(r0,r15),r0
	mov.w	r0,@(10,r1)
	mov	#38,r0
	mov.l	@r13,r2
	mov.b	@(r0,r15),r0
	mov.b	r0,@(12,r2)
	mov	#39,r0
	mov.l	@r13,r4
	mov.b	@(r0,r15),r0
	mov.b	r0,@(13,r4)
	mov.l	@r13,r14
	mov	r14,r5
	add	#20,r5
	mov.l	r5,@(16,r14)
	mov.l	@r13,r1
	mov.l	@(48,r15),r5
	mov.l	@(20,r15),r14
	add	r14,r5
	mov.l	@(44,r15),r14
	mov.l	@(4,r1),r6
	mov.l	@(16,r1),r4
	sts	fpscr,r1
	and	r14,r1
	lds	r1,fpscr
	mov.l	p_helper1_7,r1
	jsr	@r1
	mov.l	r5,@(56,r15)
	sts	fpscr,r13
	and	r14,r13
	bra	11f
	lds	r13,fpscr
8:
	mov	#0,r6
	mov.l	r6,@r14
9:
	mov.l	@r13,r4
	mov	#0,r1
	cmp/eq	r1,r4
	bt	10f
	mov.l	p_free_7,r1
	jsr	@r1
	nop	
	sts	fpscr,r5
	mov.l	p_fpmask_7,r2
	mov	#0,r1
	mov.l	r1,@r13
	and	r2,r5
	lds	r5,fpscr
10:
	mov	#0,r6
	mov.l	r6,@r14
11:
	mov.l	@(4,r15),r4
	mov	#0,r1
	cmp/eq	r1,r4
	bt	12f
	mov.l	p_free_7,r6
	jsr	@r6
	nop	
12:
	mov.l	@(40,r15),r0
13:
	add	#60,r15
	lds.l	@r15+,macl
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	rts	
	mov.l	@r15+,r13
	.p2align 2
p_tag_pwv7:	.long str_pwv7
p_ext_2ex_7:	.long str_2ex
p_fetchfn_7:	.long 0x082545F8
p_fpmask_7:	.long 0xFFE7FFFF
p_helper1_7:	.long 0x085336DC
p_helper2_7:	.long 0x085341D4
p_mask_ff00_7:	.long 0x0000FF00
p_malloc_7:	.long 0x08344A9C
p_memset_7:	.long 0x08533D50
p_free_7:	.long 0x08344B42
p_ff00_7:	.word 0xFF00

	.p2align 2
str_pwv7:	.ascii "PWV7"
		.byte 0,0,0,0
str_2ex:	.ascii "2EX"
		.byte 0

! ceil(31*C(h)) floored at 1, with C(h) = 0.5-0.5*cos(pi*h/128), h = 0..128
! (129 entries -- PWV7 bytes validate to that range, not 0..127): the detail
! path's own rounding rule, so a nonzero high byte never packs to zero height.
	.p2align 2
curve5:
	.byte	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2
	.byte	2,2,2,2,2,3,3,3,3,3,4,4,4,4,5,5
	.byte	5,5,6,6,6,6,7,7,7,8,8,8,9,9,9,10
	.byte	10,10,11,11,12,12,12,13,13,13,14,14,14,15,15,16
	.byte	16,16,17,17,18,18,18,19,19,19,20,20,20,21,21,22
	.byte	22,22,23,23,23,24,24,24,25,25,25,26,26,26,26,27
	.byte	27,27,27,28,28,28,28,29,29,29,29,29,30,30,30,30
	.byte	30,30,31,31,31,31,31,31,31,31,31,31,31,31,31,31
	.byte	31

! scratch: the fetch's out_ptr (+0) and out_size (+4), the packed buffer
! while it is being built (+8) and the entry count (+12).
	.p2align 2
scratch:
	.long 0
	.long 0
	.long 0
	.long 0

savedpr:
	.long 0
blob_end:
