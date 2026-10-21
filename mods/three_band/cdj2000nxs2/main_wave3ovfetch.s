! SPDX-License-Identifier: GPL-2.0-or-later
!
! Real 3-band overview data for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): reads the track's own PWV6 tag from its .2EX and packs it
! for main_wave3ovdata.s, which writes it into the overview payload.
!
! A tag fetch hands back a 20-byte record, not the tag: +0 entry count, +4
! payload bytes, +8 bytes per entry, +16 the payload (the entries, after
! the tag header), which sits in the same block at +20.
!
! Hook: the colour-preview reader, right before it reads the PWV4 tag. That
! is the same ANLZ handle (r13) and hash (the saved r5) the reader is about
! to use, and it runs before the preview is published, so the packed
! overview is always the one of the track being loaded.
!
! fetch_pwv6 is the generic ANLZ tag fetch the firmware itself uses for
! PWV5, transcribed instruction for instruction with only the tag and
! extension strings swapped, so it inherits that routine's own correctness.
!
! PWV6 (1200 raw entries, required by format) downsamples 2:1 into the 600
! columns the NXS2's own colour-preview overview already uses. As the
! CDJ-2000NXS reference computes it: a first pass finds D, the track max of
! the UNweighted low+mid+curve8[high] (curve8 holds 128*C(high)), clamped to
! 1..128; the second pass keeps, per column, whichever of the pair's two raw
! entries has the larger WEIGHTED stack F = 1.2*low+0.6*mid+153.6*C(high)
! (wstack below, integer *5 fixed point), then packs the winner's L/M/F
! extents as ceil(40*S/D) floored at 1 and capped at 40, the strip
! renderer's own row count. A loud track can still clip at row 40 (D is
! unweighted, the stack it bounds is weighted); the reference does the same.
!
! A missing .2EX, a missing tag, a count other than 1200 or a failed
! allocation leaves the stock overview alone.
!
! The packed overview (600 x ABC, AB, A extents, 1800 bytes) is handed over
! through one word at 0x081FFFFC, the last word of the erased run the mods
! are placed in, which the placed routines never reach. It reads 0xFFFFFFFF
! until the first load, 0 when the current track has no overview, and is set
! only once the buffer is complete. The previous track's buffer is dropped
! first, so a load that fails partway publishes no overview rather than the
! last track's.

	.text
wave3ovfetch:
	! The blob is copied into its cave as it is, so every .long below that
	! names a label in this file holds an offset from wave3ovfetch until
	! this loop adds the address the blob was placed at (r3). A word
	! already at or past the blob's size has been done by an earlier call.
	! r4, which points at the span's saved registers, is left alone.
	mova	p_malloc,r0
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
	mov.l	@(20,r4),r0		! the hash the reader is about to be given
	sts.l	pr,@-r15
	mov.l	r0,@-r15

	mov.l	p_shared,r0
	mov.l	@r0,r1
	mov	#0,r2
	mov.l	r2,@r0			! cleared first: its reader runs in another task
	cmp/pl	r1			! 0 = none, -1 = never written
	bf	1f
	mov.l	p_free,r2
	jsr	@r2
	mov	r1,r4
1:
	mov.l	p_scratch,r7
	mov	#0,r0
	mov.l	r0,@r7			! the fetch reads its out_ptr on some failures
	mov.l	r0,@(4,r7)
	mov	r13,r4			! the open ANLZ handle
	mov.l	@r15,r5
	mov	r7,r6
	add	#4,r6
	bsr	fetch_pwv6
	nop

	mov.l	p_scratch,r0
	mov.l	@r0,r4			! the PWV6 record
	tst	r4,r4
	bt	out
	mov.l	@r4,r5
	mov.l	p_1200,r6
	cmp/eq	r6,r5
	bf	pwv6_freeraw
	mov.w	@(8,r4),r0
	cmp/eq	#3,r0			! PWV6 entries are 3 bytes
	bf	pwv6_freeraw

	! -- first pass: D = clamp(max over all 1200 raw entries of
	! low+mid+curve8[high], each byte clamped to 0..128 first) to 1..128.
	! the CDJ-2000NXS reference's preview normaliser D.
	mov.l	@(16,r4),r5		! r5 = raw src
	mov.l	p_1200,r6		! r6 = loop counter
	mov	#1,r7			! r7 = running D, floored at 1
d_scan:
	mov	#64,r2			! clamp constant, reloaded every pass
	shll	r2
	mov.b	@r5,r0
	extu.b	r0,r1
	cmp/hs	r2,r1
	bf	140f
	mov	r2,r1
140:					! r1 = low, clamped
	mov.b	@(1,r5),r0
	extu.b	r0,r0
	cmp/hs	r2,r0
	bf	141f
	mov	r2,r0
141:					! r0 = mid, clamped
	add	r0,r1			! r1 = low+mid
	mov.b	@(2,r5),r0
	extu.b	r0,r0
	cmp/hs	r2,r0
	bf	142f
	mov	r2,r0
142:					! r0 = high, clamped -- index for the lookup
	mov.l	p_curve8,r2
	mov.b	@(r0,r2),r0
	extu.b	r0,r0
	add	r0,r1			! r1 = low+mid+curve8[high]
	cmp/hs	r7,r1			! T = (r1 >= r7)
	bf	143f
	mov	r1,r7			! r7 = new running max
143:
	add	#3,r5
	dt	r6
	bf	d_scan
	mov	#64,r0
	shll	r0
	cmp/hs	r0,r7
	bf	144f
	mov	r0,r7
144:					! r7 = D, clamped to 1..128
	mov.l	p_dnorm,r0
	mov.l	r7,@r0			! dnorm = D -- reloaded after malloc below,
					! since r7 is caller-saved across a jsr

	mov.l	p_1800,r4		! 600 columns * 3 bytes (ABC, AB, A)
	mov.l	p_malloc,r2
	jsr	@r2
	nop
	tst	r0,r0
	bt	pwv6_freeraw

	mov.l	p_scratch,r1
	mov.l	r0,@(8,r1)		! scratch.pwv6 = the packed buffer, shared once complete
	mov	r0,r4			! r4 = packed dst
	mov.l	@r1,r5
	mov.l	@(16,r5),r5		! r5 = raw src
	mov.l	p_600,r6		! r6 = column counter
	mov.l	p_dnorm,r7
	mov.l	@r7,r7			! r7 = D, for the whole second pass
pwv6_col:
	! -- pick the louder entry of this column's raw pair by weighted
	! F = 1.2*low + 0.6*mid + 153.6*C(high) (integer *5 scale: F5 =
	! 6*low + 3*mid + 6*curve8[high], since curve8 already holds
	! 128*C(high)); wstack below computes L5/M5/F5 together, so both
	! candidates go through it once for F alone, then the winner goes
	! through it again for its own L5/M5/F5 -- cheaper and far less
	! error-prone than threading three partial sums through the compare.
	! candidate 0's raw bytes are at r5+0/+1/+2 (low/mid/high). The
	! displaced byte load (mov.b @(disp,Rn),Rd) only ever targets R0 on
	! SH-4, so mid and high are extracted through r0 first and moved out;
	! low is read last, straight into r0 where wstack wants it.
	mov.b	@(1,r5),r0
	extu.b	r0,r1
	mov	#64,r2
	shll	r2
	cmp/hs	r2,r1
	bf	150f
	mov	r2,r1
150:					! r1 = mid0, clamped
	mov.b	@(2,r5),r0
	extu.b	r0,r2
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r2
	bf	151f
	mov	r3,r2
151:					! r2 = high0, clamped
	mov.b	@r5,r0
	extu.b	r0,r0
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r0
	bf	152f
	mov	r3,r0
152:					! r0 = low0, clamped
	bsr	wstack
	nop
	mov.l	r2,@-r15		! push F0 (wstack's r2 output)

	! candidate 1's raw bytes are at r5+3/+4/+5, same order.
	mov.b	@(4,r5),r0
	extu.b	r0,r1
	mov	#64,r2
	shll	r2
	cmp/hs	r2,r1
	bf	153f
	mov	r2,r1
153:					! r1 = mid1, clamped
	mov.b	@(5,r5),r0
	extu.b	r0,r2
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r2
	bf	154f
	mov	r3,r2
154:					! r2 = high1, clamped
	mov.b	@(3,r5),r0
	extu.b	r0,r0
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r0
	bf	155f
	mov	r3,r0
155:					! r0 = low1, clamped
	bsr	wstack
	nop
	mov.l	@r15+,r3		! r3 = F0
	cmp/hs	r3,r2			! T = (F1 >= F0)
	bt	winner1

	! candidate 0 wins -- reread and reclamp its bytes for wstack's own
	! L5/M5/F5 output (cheaper and safer than a computed base register,
	! since the displaced load can only ever target r0).
	mov.b	@(1,r5),r0
	extu.b	r0,r1
	mov	#64,r2
	shll	r2
	cmp/hs	r2,r1
	bf	156f
	mov	r2,r1
156:
	mov.b	@(2,r5),r0
	extu.b	r0,r2
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r2
	bf	157f
	mov	r3,r2
157:
	mov.b	@r5,r0
	extu.b	r0,r0
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r0
	bf	158f
	mov	r3,r0
158:
	bra	159f
	nop

winner1:
	mov.b	@(4,r5),r0
	extu.b	r0,r1
	mov	#64,r2
	shll	r2
	cmp/hs	r2,r1
	bf	160f
	mov	r2,r1
160:
	mov.b	@(5,r5),r0
	extu.b	r0,r2
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r2
	bf	161f
	mov	r3,r2
161:
	mov.b	@(3,r5),r0
	extu.b	r0,r0
	mov	#64,r3
	shll	r3
	cmp/hs	r3,r0
	bf	162f
	mov	r3,r0
162:
159:					! r0=low, r1=mid, r2=high of the winner
	bsr	wstack
	nop
					! r0=L5, r1=M5, r2=F5 for the winner

	mov.l	r0,@-r15		! push L5
	mov.l	r1,@-r15		! push M5

	! extents: ceil(40*S/D) = ceil(8*S5/D) (S5 already *5, 40/5=8), floored
	! at 1, capped at 40 (the NXS2 overview strip's own row count,
	! the strip renderer's own clamp 0x28).
	mov	r2,r0
	shll2	r0
	shll	r0			! r0 = 8*F5
	mov	r7,r1			! r1 = D
	bsr	divceil
	nop
	mov	#40,r1
	cmp/hs	r1,r0
	bf	160f
	mov	r1,r0
160:	tst	r0,r0
	bf	161f
	mov	#1,r0
161:	mov.b	r0,@r4			! ABC/F byte

	mov.l	@r15+,r1		! pop M5
	mov	r1,r0
	shll2	r0
	shll	r0			! r0 = 8*M5
	mov	r7,r1
	bsr	divceil
	nop
	mov	#40,r1
	cmp/hs	r1,r0
	bf	162f
	mov	r1,r0
162:	tst	r0,r0
	bf	163f
	mov	#1,r0
163:	mov.b	r0,@(1,r4)		! AB/M byte

	mov.l	@r15+,r1		! pop L5
	mov	r1,r0
	shll2	r0
	shll	r0			! r0 = 8*L5
	mov	r7,r1
	bsr	divceil
	nop
	mov	#40,r1
	cmp/hs	r1,r0
	bf	164f
	mov	r1,r0
164:	tst	r0,r0
	bf	165f
	mov	#1,r0
165:	mov.b	r0,@(2,r4)		! A/L byte

	add	#3,r4
	add	#6,r5
	dt	r6
	bf	pwv6_col

	mov.l	p_scratch,r0
	mov.l	@(8,r0),r1
	mov.l	p_shared,r2
	mov.l	r1,@r2


pwv6_freeraw:
	mov.l	p_scratch,r0
	mov.l	@r0,r1
	tst	r1,r1
	bt	out
	mov.l	p_free,r2
	jsr	@r2
	mov	r1,r4
out:
	add	#4,r15
	lds.l	@r15+,pr
	rts
	nop

! wstack: the overview's weighted band stack, shared by the F-selection
! compare and the final per-column extent math. In: r0=low, r1=mid, r2=high, each already clamped
! 0..128. Out: r0=L5 (6*low), r1=M5 (L5+3*mid), r2=F5 (M5+6*curve8[high]) --
! *5 fixed-point versions of 1.2*low, 1.2*low+0.6*mid and the full
! 1.2*low+0.6*mid+153.6*C(high), since curve8[high] already equals
! 128*C(high) and 5*153.6/128 = 6. Only r0-r3 touched (leaves the caller's
! r4-r7 loop state alone), so it is safe to bsr from inside pwv6_col.
	.p2align 2
wstack:
	mov	r0,r3			! r3 = low (stashed)
	mov	r2,r0			! r0 = high -- the table index register
	mov.l	p_curve8,r2
	mov.b	@(r0,r2),r0
	extu.b	r0,r2			! r2 = curve8[high]

	mov	r3,r0
	shll2	r0			! r0 = 4*low
	shll	r3			! r3 = 2*low
	add	r3,r0			! r0 = 6*low = L5

	mov	r1,r3
	shll	r3			! r3 = 2*mid
	add	r1,r3			! r3 = 3*mid
	add	r0,r3			! r3 = L5 + 3*mid = M5
	mov	r3,r1			! r1 = M5

	mov	r2,r3
	shll2	r3			! r3 = 4*curve8[high]
	shll	r2			! r2 = 2*curve8[high]
	add	r3,r2			! r2 = 6*curve8[high]
	add	r1,r2			! r2 = M5 + 6*curve8[high] = F5

	rts
	nop

! divceil: unsigned ceiling division by shift-and-subtract over the 16 low
! bits of the dividend (the largest dividend here is 8*1920). A div1 loop
! is not usable: the loop counter's dt overwrites the T bit div1 carries
! from one step to the next. In: r0=dividend, r1=divisor (D, 1..128).
! Out: r0=ceil(dividend/divisor). r2/r3 are scratch; r4/r5 are saved.
	.p2align 2
divceil:
	mov.l	r4,@-r15
	mov.l	r5,@-r15
	mov	r0,r5
	shll16	r5			! dividend bit 15 at the top
	mov	#0,r2			! r2 = running remainder
	mov	#0,r3			! r3 = quotient
	mov	#16,r4
divloop:
	shll	r5			! T = next dividend bit
	rotcl	r2			! remainder = remainder*2 + bit
	shll	r3
	cmp/hs	r1,r2
	bf	170f
	sub	r1,r2
	add	#1,r3
170:	dt	r4
	bf	divloop
	tst	r2,r2
	bt	171f
	add	#1,r3			! nonzero remainder -- round up
171:	mov	r3,r0
	mov.l	@r15+,r5
	rts
	mov.l	@r15+,r4

	.p2align 2
p_malloc:	.long 0x08344A9C
p_free:		.long 0x08344B42
p_shared:	.long 0x081FFFFC
p_scratch:	.long scratch
p_dnorm:	.long dnorm
p_curve8:	.long curve8
p_600:		.long 600
p_1200:		.long 1200
p_1800:		.long 1800
p_poolofs:	.long p_malloc - wave3ovfetch
p_blobsize:	.long blob_end - wave3ovfetch

! Offsets of the pool words that hold a label of this file, for the loop
! at the top of wave3ovfetch.
	.p2align 2
reloc_tab:
	.word p_scratch - wave3ovfetch, p_dnorm - wave3ovfetch, p_curve8 - wave3ovfetch
	.word p_tag_pwv6 - wave3ovfetch, p_ext_2ex_6 - wave3ovfetch
	.word 0

	! -- PWV6 fetch, same routine, "PWV6"/"2EX".
fetch_pwv6:
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
	mov.l	p_fetchfn_6,r2
	mov.l	p_ext_2ex_6,r7
	mov.l	p_tag_pwv6,r6
	jsr	@r2
	nop	
	sts	fpscr,r2
	mov.l	p_fpmask_6,r1
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
	mov.l	p_helper1_6,r2
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
	mov.l	p_helper1_6,r7
	mov	r15,r4
	mov.l	@(48,r15),r5
	add	#16,r4
	jsr	@r7
	mov	#24,r6
	sts	fpscr,r2
	mov.l	@(44,r15),r1
	mov.l	p_helper2_6,r7
	mov	r15,r4
	mov.l	p_tag_pwv6,r5
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
	mov.l	p_mask_ff00_6,r2
	add	r15,r1
	mov.l	r2,@(52,r15)
	mov.w	@r1,r0
	extu.w	r0,r5
	mov.w	@(2,r1),r0
	extu.b	r5,r6
	and	r2,r5
	mov	#-8,r7
	shad	r7,r5
	mov.w	p_ff00_6,r7
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
	mov.l	p_malloc_6,r2
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
	mov.l	p_memset_6,r7
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
	mov.l	p_helper1_6,r1
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
	mov.l	p_free_6,r1
	jsr	@r1
	nop	
	sts	fpscr,r5
	mov.l	p_fpmask_6,r2
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
	mov.l	p_free_6,r6
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
p_tag_pwv6:	.long str_pwv6
p_ext_2ex_6:	.long str_2ex
p_fetchfn_6:	.long 0x082545F8
p_fpmask_6:	.long 0xFFE7FFFF
p_helper1_6:	.long 0x085336DC
p_helper2_6:	.long 0x085341D4
p_mask_ff00_6:	.long 0x0000FF00
p_malloc_6:	.long 0x08344A9C
p_memset_6:	.long 0x08533D50
p_free_6:	.long 0x08344B42
p_ff00_6:	.word 0xFF00

	.p2align 2
str_pwv6:	.ascii "PWV6"
		.byte 0,0,0,0
str_2ex:	.ascii "2EX"
		.byte 0

! Raised-cosine widening curve round(128*C(h)), C(h) = 0.5-0.5*cos(pi*h/128),
! h = 0..128 (129 entries; PWV6 bytes validate to that range). Used for the
! normaliser D and the per-column weighted-F comparison.
	.p2align 2
curve8:
	.byte	0,0,0,0,0,0,1,1,1,2,2,2,3,3,4,4
	.byte	5,5,6,7,8,8,9,10,11,12,13,14,15,16,17,18
	.byte	19,20,21,22,23,25,26,27,28,30,31,32,34,35,37,38
	.byte	40,41,42,44,45,47,48,50,52,53,55,56,58,59,61,62
	.byte	64,66,67,69,70,72,73,75,76,78,80,81,83,84,86,87
	.byte	88,90,91,93,94,96,97,98,100,101,102,103,105,106,107,108
	.byte	109,110,111,112,113,114,115,116,117,118,119,120,120,121,122,123
	.byte	123,124,124,125,125,126,126,126,127,127,127,128,128,128,128,128
	.byte	128

! scratch: the fetch's out_ptr (+0) and out_size (+4), then the packed buffer
! while it is being built (+8).
	.p2align 2
scratch:
	.long 0
	.long 0
	.long 0

! dnorm: the overview normaliser D, written by the first pass over all
! 1200 raw PWV6 entries and read back by the second (column) pass -- a
! global rather than a register because a jsr (malloc) sits between the
! two passes and r0-r7 are caller-saved across it.
dnorm:
	.long 0
blob_end:
