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
! Serato shifts the key itself and sends the 0x27 text once per load, so a
! shift made later only shows in the control report (0x21): byte 57 (low 6
! bits) is the key, 1 to 34, byte 58 the shift, 13 + semitones, and byte 7
! bits 0-1 are 3 when the track has no key. The 3000X looks the key up in its
! own tables (pitch class of the major key, or of the relative major of a minor
! one, and major or minor) and moves it by the shift; the table below is a copy
! of them. x3000rxview zeroes the view from byte 42 on, so the bytes are read
! from the whole report in the receive buffer.
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
! A 0x21 sends the shifted key as the same message, as its Camelot code, but
! only when key and shift differ from the last ones sent: Serato sends the
! report 64 times a second. A 0x27 forgets them, so the control report that
! follows a load puts the shift back over the 0x27's key.
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
	cmp/eq	#0x21,r0
	bt	shift
	cmp/eq	#0x27,r0
	bf	done
	mov.w	@(2,r8),r0
	cmp/eq	#1,r0
	bf	done
	mov.w	@(4,r8),r0
	cmp/eq	#1,r0
	bf	done

	mov.l	p_shown,r1
	mov	#-1,r0
	mov.l	r0,@r1
	bsr	message
	add	#-32,r15
	mov	r8,r9
	add	#6,r9

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
named:
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

	! r4 = what is shown: 0 no key, else (key << 8) | shift; r7 = key - 1
shift:
	mov.l	p_report,r1
	mov	#0,r4
	mov.b	@(7,r1),r0
	and	#3,r0
	cmp/eq	#3,r0
	bt	check
	mov	#57,r0
	mov.b	@(r0,r1),r0
	and	#0x3F,r0
	mov	r0,r7
	add	#-1,r7
	mov	#34,r2
	cmp/hs	r2,r7
	bt	check
	mov	#58,r0
	mov.b	@(r0,r1),r3
	extu.b	r3,r3
	mov	r7,r4
	add	#1,r4
	shll8	r4
	or	r3,r4

check:
	mov.l	p_shown,r1
	mov.l	@r1,r0
	cmp/eq	r4,r0
	bt	done
	mov.l	r4,@r1
	bsr	message
	add	#-32,r15
	tst	r4,r4
	bt	send

	! the pitch class moved by shift - 13, as its Camelot number and letter
	mova	keys,r0
	mov.b	@(r0,r7),r1
	mov	r1,r0
	and	#15,r0
	add	r3,r0
	add	#11,r0
	mov	#12,r2
4:	cmp/hs	r2,r0
	bf	5f
	bra	4b
	add	#-12,r0
5:	mov	r0,r5
	mova	camelot,r0
	mov.b	@(r0,r5),r2
	mov	r1,r3
	shlr2	r3
	shlr2	r3
	mov	#10,r1
	cmp/hs	r1,r2
	bf	digit
	bra	named
	add	#-10,r2
digit:
	mov	r2,r0
	add	#0x30,r0
	mov.b	r0,@r6
	mov	r3,r0
	add	#0x41,r0
	bra	send
	mov.b	r0,@(1,r6)

	! the 0x41 message in the 32 bytes at r15: zeros with the type in byte 1,
	! r6 = its name
message:
	mov	r15,r1
	mov	#8,r2
	mov	#0,r0
1:	mov.l	r0,@r1
	dt	r2
	bf/s	1b
	add	#4,r1
	mov	#0x41,r0
	mov.b	r0,@(1,r15)
	mov	r15,r6
	rts
	add	#2,r6

! per key 1 to 34: the pitch class of its major key, C = 0, plus 0x10 for a
! major key (the 3000X's tables at 0x271C740 and 0x271C768)
	.p2align 2
keys:
	.byte	0x10, 0x00, 0x11, 0x01, 0x12, 0x02, 0x13, 0x03, 0x14, 0x04, 0x15, 0x05
	.byte	0x16, 0x06, 0x17, 0x07, 0x18, 0x08, 0x19, 0x09, 0x1A, 0x0A, 0x1B, 0x0B
	.byte	0x11, 0x01, 0x13, 0x04, 0x16, 0x06, 0x18, 0x09, 0x1A, 0x0B

! the Camelot number of each pitch class: C 8, Db 3, D 10, ...
	.p2align 2
camelot:
	.byte	8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1

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
p_report:	.long 0x081FFBE0
p_shown:	.long 0x081FEFDC
