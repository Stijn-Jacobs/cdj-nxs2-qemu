! SPDX-License-Identifier: GPL-2.0-or-later
!
! The track key from Serato, for the CDJ-2000NXS2 MAIN firmware 1.87 (SH7724,
! SH-4, little-endian).
!
! Serato sends a CDJ-3000X the key of the loaded track as type 0x27: one
! report, UTF-16LE text from byte 6 ("1A", "11A": the Camelot number and
! letter), an empty string when the track has none. The NXS2's report router
! has no arm for 0x27. It does take the key as type 0x41, which the report
! framework 0x084F9DB2 reads without a fragment header: a name of 5 ASCII
! characters at byte 2, the colour index at 7, then 16 bytes the key does not
! use. The name is turned into the key number by the firmware's own parser,
! which takes the Camelot numbers 1A to 9B and the note names ("Am", "F#m",
! "D"), but not 10 to 12.
!
! Hook: the entry of the host-report router 0x084F9504, after x3000hs, which
! hooks the same place. The report is the 64-byte view of the stock buffer (at
! offset 16 of the saved r0-r7 the trampoline hands in r4, its length at 20),
! which holds a key whole. Once the deck is authenticated (flag 0x0BD4B144), a
! single-report 0x27 is rewritten as a 0x41 on the stack and handed to the
! framework. The text is copied one character per unit, up to 5; a unit that
! is not ASCII leaves the name empty (no key). 10A to 12B become the note
! names the same keys have on the Camelot wheel: 10A Bm, 10B D, 11A F#m,
! 11B A, 12A C#m, 12B E. The colour index is left 0: Serato sends none.
!
! r0-r7 are free; r8-r14 are kept.

	.text
x3000key:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	@(20,r4),r0
	mov	#12,r1
	cmp/hs	r1,r0
	bf	done
	mov.l	p_auth,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	done
	mov.l	@(16,r4),r8
	mov.b	@(1,r8),r0
	cmp/eq	#0x27,r0
	bf	done
	mov.w	@(2,r8),r0
	cmp/eq	#1,r0
	bf	done
	mov.w	@(4,r8),r0
	cmp/eq	#1,r0
	bf	done

	! the 0x41 message, 32 bytes of zeros with the type in byte 1
	add	#-32,r15
	mov	r15,r1
	mov	#8,r2
	mov	#0,r0
1:	mov.l	r0,@r1
	dt	r2
	bf/s	1b
	add	#4,r1
	mov	#0x41,r0
	mov.b	r0,@(1,r15)
	mov	r8,r9
	add	#6,r9
	mov	r15,r6
	add	#2,r6

	! "10" to "12" and a letter A or B, then the end: the note name from the table
	mov.b	@r9,r0
	cmp/eq	#0x31,r0
	bf	plain
	mov.b	@(2,r9),r0
	add	#-0x30,r0
	mov	r0,r2
	mov	#2,r1
	cmp/hi	r1,r0
	bt	plain
	mov.b	@(4,r9),r0
	add	#-0x41,r0
	mov	#1,r1
	cmp/hi	r1,r0
	bt	plain
	mov	r0,r3
	mov.b	@(6,r9),r0
	tst	r0,r0
	bf	plain
	mov	r2,r1
	shll	r1
	add	r3,r1
	mov	r1,r2
	shll2	r1
	add	r2,r1
	mova	names,r0
	add	r0,r1
	mov	#5,r2
2:	mov.b	@r1+,r0
	dt	r2
	mov.b	r0,@r6
	bf/s	2b
	add	#1,r6
	bra	send
	nop

plain:
	mov	r9,r5
	mov	#5,r2
3:	mov.b	@r5,r0
	tst	r0,r0
	bt	send
	mov.b	@(1,r5),r0
	tst	r0,r0
	bf	blank
	mov.b	@r5,r0
	mov.b	r0,@r6
	add	#1,r6
	add	#2,r5
	dt	r2
	bf	3b
	bra	send
	nop

blank:
	mov	r15,r6
	add	#2,r6
	mov	#0,r0
	mov.b	r0,@r6
	mov.b	r0,@(1,r6)
	mov.b	r0,@(2,r6)
	mov.b	r0,@(3,r6)
	mov.b	r0,@(4,r6)

send:
	mov.l	p_frame,r1
	mov	#24,r5
	jsr	@r1
	mov	r15,r4
	add	#32,r15

done:
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

! 10A, 10B, 11A, 11B, 12A, 12B
	.p2align 2
names:
	.ascii	"Bm\0\0\0"
	.ascii	"D\0\0\0\0"
	.ascii	"F#m\0\0"
	.ascii	"A\0\0\0\0"
	.ascii	"C#m\0\0"
	.ascii	"E\0\0\0\0"

	.p2align 2
p_auth:		.long 0x0BD4B144
p_frame:	.long 0x084F9DB2
