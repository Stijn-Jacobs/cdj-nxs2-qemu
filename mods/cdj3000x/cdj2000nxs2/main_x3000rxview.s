! SPDX-License-Identifier: GPL-2.0-or-later
!
! The stock view of a 1024-byte HID Output report, for the CDJ-2000NXS2 MAIN
! firmware 1.87 (SH7724, SH-4, little-endian).
!
! x3000rxbuf reads each report from Serato into a 1024-byte buffer. Everything
! after the read takes the report from the stock buffer 0x0BD50FDC: the
! report router 0x084F9504 (called with the count the read returned) once the
! handshake has started, 0x084FF8E2 before. The router copies 64 bytes into
! its stack frame and every handler it feeds appends count - 6 bytes per
! fragment into records sized for 58, so it must never see a count above 64.
!
! Hook: the instructions after the read returns, which keep the count in r5
! and compare it with -5 (the read timed out) for the bt/s that follows. On
! any count that is not an error this copies the report's first 64 bytes into
! the stock buffer and caps the count at 64, so the stock code sees exactly
! what a 64-byte read gave it. T is put back as the compare left it.
!
! The list Serato shows on the deck (type 0x2a: the prompt, the deck choice,
! the library) does not fit 64 bytes. A CDJ-3000X gets it as records, each
! row, flag|style, two more header bytes, four bytes the NXS2 does not have,
! then UTF-16LE text ending in 0x0000, in one report or in fragments of 1018
! bytes. The NXS2's own record is the same without those four bytes. Once the
! deck is authenticated and the task reads reports for the router (state word
! 0x0BD50E90 == 2), the fragments of a 0x2a are put together in a stage (up to
! four, in order; one out of turn is dropped), and the whole list is rewritten
! in the NXS2 form and handed to the report framework 0x084F9DB2 itself, which
! appends it to the list record and draws it; the stock view of every fragment
! is retyped 0x3F, a type the router and the framework ignore. The walk ends at
! the first all-zero record after the first one, the padding of the report.
! The NXS2 has nine rows, a header and eight entries, and stops at the first
! record beyond them; Serato sends a header and up to eleven, the cursor (flag
! bit 7) four rows down while it scrolls. So row 0 is kept and eight rows from
! four above the cursor (no further than the eighth from the last, at least
! row 1) become rows 1-8. A track (style 4) has no BPM or key column on the
! NXS2: its text gets " 102.5 1A", the BPM in bytes 4-5 with the tenths in
! byte 3, and the key in byte 6 when it is a minor one (even, A minor = 2, a
! semitone a step); both agree with the BPM and key text Serato sends for the
! highlighted track. A major key (odd) is left out: which is which is not known.
! With x3000browse, which shows the list ahead of Serato from the HID IN task,
! a whole list is committed instead (see commit): kept as the answer the
! prediction starts from, counted against the oldest step, and shown with the
! steps still outstanding applied.
!
! The control report (type 0x21) a 3000X gets has the NXS2's fields up to
! byte 20 and then an extra byte: the BPM, the tempo, the cue point and the
! second time are each one byte further on, so the NXS2 would show BPM 0 and
! a garbage tempo. The stock view gets bytes 22-42 at 21-41, the second
! time's minutes without the valid flag the 3000X sets in them, and zeros from
! byte 42 on (a 3000X sends its track BPM where the NXS2 reads beat segments).
! Byte 6 bit 0, which the 3000X sets and the NXS2 takes as "read the phase
! meter", is cleared. Bit 7 of the elapsed seconds is the sign: the time is
! negative while the playhead is before the track's start, for a few seconds
! after a load. The NXS2 counts from 0:00.000 at the start and refuses a
! seconds byte above 59 (it then reads both times as 0), so a negative elapsed
! becomes 0:00.000.
! The loop size (byte 10, low 7 bits) is the same table on both decks from 3,
! 1/128 beat, to 21, 512 beats; the NXS2 lights a beat loop key for the sizes
! it has one for and none for the rest. The 3000X's sizes above 21 (3/2, 3, 5,
! 6, 7 and 9 beats) have no value in the NXS2 table, and the 3000X shows a size
! only on a beat loop (byte 10 bit 7 clear) that exists (byte 9 bit 3), so any
! other size becomes 0x80, the "no loop" Serato sends. Bit 7 of the loop in
! and out times' top bytes (27 and 30 in the stock view) is a flag the NXS2
! would add to the time as 0x800000 ms, and is cleared.
!
! The overview (0x2c) and the beat grid (0x2f) come in fragments of 1018
! bytes, and the stock records take them in the NXS2 layout: the overview a
! flag byte and 600 columns of 7 bytes, the grid a u16 count and 4-byte
! entries. Through the 64-byte view the stock arm would append 58 bytes of
! each, so every fragment is handed to the framework whole instead, cut at
! the end of the message (1 + 600 x 7 bytes; 2 + 4 x count), so the zeros
! padding the last fragment do not push the overview past its 4,240-byte cap.
! An empty overview (flag 0x80, without bit 6) keeps the 64-byte view the
! stock arm has always taken it in: whole, its fragments' own flag bytes would
! publish as a strip of columns.
! The grid is held to the 24,000 entries its record has room for, and the
! downbeat Serato numbers 0 after the first bar becomes the NXS2's 1.
!
! The detail waveform comes as a track length (0x35: u32 N at bytes 2-5,
! one report) and then windows of points (0x36: u32 count at 6, u32 start
! at 10, count u16 points from 14, one report each, streamed ahead of the
! playhead). The point is the NXS2's own detail point. The length goes to the
! framework as the first fragment of a whole detail (0x2e) of N points, at
! most the 540,007 the buffer holds, which builds the stock detail header;
! the points are then cleared, unless the same N is announced again (Serato
! repeats the 0x35 of a load while it already streams its windows). Each
! window inside N is copied to the header's point array. The stock publisher
! sends the whole detail from byte 0 each time it is asked, 8 or 9 chunks of
! 8 KiB plus the grid for a few minutes of track, and the display takes in no
! deck state until that train has gone through. So a train of only the new
! chunks is armed here instead (see train) once 10 s of points (1,500) have
! come in since the last one, and when a window ends the track: the first
! 67 s of a track arrive in a few seconds, and while it plays the detail stays
! at least 57 s ahead of the playhead. A seek makes Serato stream windows
! around the new position. One that is neither at the front of what has come
! in (within two windows of the highest point) nor inside what the last train
! covered (up to the highest point it had then) moves the count to 450 points
! short of the 10 s, so the display has the new position a few seconds later.
! An N of 0 leaves the stock "no wave" path to the framework.
!
! The memory cues and loops (0x2d) are the track length ([min][sec][ms u16]),
! a count and up to ten records. A 3000X's record is six bytes: the kind (1 a
! memory cue, 2 and 3 the others; any other is skipped), a colour 1-8 (any
! other shows red, 2), minutes, seconds and ms u16. The NXS2's arm 0x084FAE5E
! takes the same header and five-byte records, bit 7 of the first byte set
! for anything but a memory cue and the colour in the rest, and its converter
! 0x084D6A52 reads at most the first ten. A count of 0 or above ten is an
! empty list on the 3000X. The records are rewritten in that form in the stock
! view, the count to the records kept and the rest of the ten zero.
!
! The hot cues (0x30) need nothing: the length and eight six-byte slots (kind,
! colour 0-64 on the same hue wheel, minutes, seconds, ms u16) are the layout
! the NXS2's own arm reads, and they reach it as they come.
!
! r4 = the saved r0-r7, r5 at offset 20. r0-r7 are free; r8-r11 are saved.

	.text
x3000rxview:
	sts.l	pr,@-r15
	movt	r0
	mov.l	r0,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov	r4,r8
	mov.l	p_browse,r1
	mov.l	@r1,r0
	cmp/eq	#1,r0
	bf	1f
	bsr	commit
	nop
1:	mov.l	@(20,r8),r0
	cmp/pz	r0
	bf	done
	mov	r0,r9
	mov	#64,r1
	cmp/hi	r1,r0
	bf	1f
	mov.l	r1,@(20,r8)
1:	mov.l	p_buffer,r5
	mov.l	p_stock,r6
	mov	#16,r2
2:	mov.l	@r5+,r0
	dt	r2
	mov.l	r0,@r6
	bf/s	2b
	add	#4,r6

	! the 3000X layouts, while the router is fed and the deck authenticated
	mov	#7,r1
	cmp/hs	r1,r9
	bf	done
	mov.l	p_state,r1
	mov.l	@r1,r0
	cmp/eq	#2,r0
	bf	done
	mov.l	p_auth,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	done
	mov.l	p_buffer,r10
	mov.b	@(1,r10),r0
	cmp/eq	#0x21,r0
	bt	control
	cmp/eq	#0x2a,r0
	bt	1f
	bra	waves
	nop
1:	bra	list
	nop

done:
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	mov.l	@r15+,r7
	lds.l	@r15+,pr
	rts
	shlr	r7

	! the control report: r10 the 3000X report, r6 the stock view of it
control:
	mov.l	p_stock,r6
	mov.b	@(6,r6),r0
	and	#0xfe,r0
	mov.b	r0,@(6,r6)
	mov.b	@(12,r6),r0
	tst	#0x80,r0
	bt	7f
	mov	#0,r0
	mov.b	r0,@(11,r6)
	mov.b	r0,@(12,r6)
	mov.b	r0,@(13,r6)
	mov.b	r0,@(14,r6)
	! the loop size, on a beat loop that exists and with a key on the NXS2
7:	mov.b	@(9,r6),r0
	tst	#8,r0
	bt	8f
	mov.b	@(10,r6),r0
	add	#-3,r0
	mov	#19,r1
	cmp/hs	r1,r0
	bf	11f
8:	mov	#0x80,r0
	mov.b	r0,@(10,r6)
11:	mov	r10,r4
	add	#22,r4
	mov	r6,r5
	add	#21,r5
	mov	#21,r2
9:	mov.b	@r4+,r0
	dt	r2
	mov.b	r0,@r5
	bf/s	9b
	add	#1,r5
	mov	#0,r0
	mov	#22,r2
10:	dt	r2
	mov.b	r0,@r5
	bf/s	10b
	add	#1,r5
	mov	r6,r1
	add	#27,r1
	mov.b	@r1,r0
	and	#0x7f,r0
	mov.b	r0,@r1
	mov.b	@(3,r1),r0
	and	#0x7f,r0
	mov.b	r0,@(3,r1)
	mov.b	@(7,r1),r0
	and	#0x7f,r0
	bsr	train
	mov.b	r0,@(7,r1)
	bra	done
	nop

	! r2 the records left, r3 those kept, r4 the 3000X record, r6 the NXS2 one
markers:
	mov.l	p_stock,r6
	add	#11,r6
	mov	r10,r4
	add	#11,r4
	mov.b	@(10,r10),r0
	extu.b	r0,r2
	mov	r2,r0
	add	#-1,r0
	mov	#10,r1
	cmp/hs	r1,r0
	bf	1f
	mov	#0,r2
1:	mov	#0,r3
2:	tst	r2,r2
	bt	5f
	add	#-1,r2
	mov.b	@r4,r0
	extu.b	r0,r5
	mov.b	@(1,r4),r0
	extu.b	r0,r7
	add	#-1,r5
	mov	#3,r1
	cmp/hs	r1,r5
	bt/s	2b
	add	#6,r4
	mov	r7,r0
	add	#-1,r0
	mov	#8,r1
	cmp/hs	r1,r0
	bf	3f
	mov	#2,r7
3:	tst	r5,r5
	bt/s	4f
	mov	r7,r0
	or	#0x80,r0
4:	mov.b	r0,@r6
	mov	r4,r1
	add	#-4,r1
	mov.b	@r1+,r0
	mov.b	r0,@(1,r6)
	mov.b	@r1+,r0
	mov.b	r0,@(2,r6)
	mov.b	@r1+,r0
	mov.b	r0,@(3,r6)
	mov.b	@r1,r0
	mov.b	r0,@(4,r6)
	add	#5,r6
	bra	2b
	add	#1,r3
5:	mov.l	p_stock,r1
	mov	r3,r0
	mov.b	r0,@(10,r1)
	add	#11 + 10 * 5,r1
	mov	#0,r0
6:	cmp/hs	r1,r6
	bt	7f
	mov.b	r0,@r6
	bra	6b
	add	#1,r6
7:	bra	done
	nop

	! the waveform types, r0 the type
waves:
	cmp/eq	#0x2c,r0
	bt	overview
	cmp/eq	#0x2f,r0
	bt	grid
	cmp/eq	#0x35,r0
	bt	detail
	cmp/eq	#0x36,r0
	bt	window
	cmp/eq	#0x2d,r0
	bt	markers
stock:
	bra	done
	nop

	! an empty overview (flag without bit 6) is left to the stock path, which
	! takes it as before; the flag is in the first fragment, then in the record
overview:
	mov.l	p_overview,r1
	mov.w	@(2,r10),r0
	cmp/eq	#1,r0
	bf	1f
	mov	r10,r1
	add	#6,r1
1:	mov.b	@r1,r0
	tst	#0x40,r0
	bt	stock
	mov.w	c_overview,r1
	bra	fragment
	nop

	! r1 the bytes of the grid message: from the first fragment's count,
	! later from the count that fragment left in the grid record
grid:
	mov.l	p_grid,r1
	mov.w	@(2,r10),r0
	cmp/eq	#1,r0
	bf	1f
	mov.w	@(6,r10),r0
	extu.w	r0,r0
	mov.w	c_beats,r1
	cmp/hi	r1,r0
	bf	2f
	mov	r1,r0
	bra	2f
	mov.w	r0,@(6,r10)
1:	mov.w	@(16,r1),r0
	extu.w	r0,r0
2:	shll2	r0
	add	#2,r0
	mov	r0,r1

	! r1 the bytes of the message; r2 where this fragment starts in it,
	! (index - 1) x 1018, r3 the bytes of it that belong to the message
fragment:
	mov.w	@(2,r10),r0
	extu.w	r0,r2
	add	#-1,r2
	mov	r2,r0
	shll8	r0
	shll2	r0
	shll	r2
	sub	r2,r0
	shll	r2
	sub	r2,r0
	mov	r0,r2
	mov	r9,r3
	add	#-6,r3
	mov	r1,r0
	cmp/hi	r2,r0
	bt/s	3f
	sub	r2,r0
	mov	#0,r3
3:	cmp/hi	r0,r3
	bf	4f
	mov	r0,r3
4:	mov.b	@(1,r10),r0
	cmp/eq	#0x2f,r0
	bf	6f
	! the beat in bar of each entry, at message offsets 2 + 4k
	mov	#2,r4
	sub	r2,r4
	mov	#3,r0
	and	r0,r4
	mov	r10,r5
	add	#6,r5
5:	cmp/hs	r3,r4
	bt	6f
	mov	r4,r0
	mov.b	@(r0,r5),r6
	tst	r6,r6
	bf/s	5b
	add	#4,r4
	mov	#1,r6
	bra	5b
	mov.b	r6,@(r0,r5)
6:	mov	r3,r5
	add	#6,r5
	mov	r10,r4
	! r4 a report for the framework, r5 its bytes
reinject:
	mov.l	p_frame,r1
	jsr	@r1
	nop
	bsr	asked
	nop
	bra	consumed
	nop

! the overview message: its flag byte and 600 columns
c_overview:	.word 1 + 600 * 7
! the entries the grid record has room for
c_beats:	.word 24000

consumed:
	mov.l	p_stock,r1
	mov	#0x3f,r0
	bra	done
	mov.b	r0,@(1,r1)

	! r11 the stock detail header: N at +0, the point array at +16
detail:
	mov.w	@(4,r10),r0
	shll16	r0
	mov	r0,r1
	mov.w	@(2,r10),r0
	extu.w	r0,r0
	or	r1,r0
	mov.l	c_points,r1
	cmp/hi	r1,r0
	bf	1f
	mov	r1,r0
1:	mov.l	p_detail,r11
	mov.l	@r11,r1
	cmp/eq	r1,r0
	bt	consumed
	mov	r0,r1
	mov	#0x2e,r0
	mov.b	r0,@(1,r10)
	mov	#1,r0
	mov.w	r0,@(2,r10)
	mov.w	r0,@(4,r10)
	mov	r1,r0
	mov.w	r0,@(6,r10)
	shlr16	r0
	mov.w	r0,@(8,r10)
	mov.l	p_frame,r1
	mov	#10,r5
	jsr	@r1
	mov	r10,r4
	mov.l	p_pending,r1
	mov	#0,r0
	mov.l	r0,@r1
	mov.l	p_highest,r1
	mov.l	r0,@r1
	mov.l	p_published,r1
	mov.l	r0,@r1
	mov.l	p_owed,r1
	mov.l	r0,@r1
	bsr	asked
	nop
	mov.l	@r11,r6
	shll	r6
	mov	#0,r5
	mov.l	p_memset,r1
	jsr	@r1
	mov.l	@(16,r11),r4
	bra	consumed
	nop

	! r1 the points of the window, r2 its first point
window:
	mov.w	@(8,r10),r0
	shll16	r0
	mov	r0,r1
	mov.w	@(6,r10),r0
	extu.w	r0,r0
	or	r0,r1
	mov.w	@(12,r10),r0
	shll16	r0
	mov	r0,r2
	mov.w	@(10,r10),r0
	extu.w	r0,r0
	or	r0,r2
	mov	r9,r3
	add	#-14,r3
	cmp/pz	r3
	bf	consumed
	shlr	r3
	mov	r1,r0
	add	#-1,r0
	cmp/hs	r3,r0
	bt	consumed
	mov.l	p_detail,r11
	mov.l	@r11,r3
	mov.l	c_points,r0
	cmp/hi	r0,r3
	bt	consumed
	cmp/hi	r3,r2
	bt	consumed
	mov	r3,r0
	sub	r2,r0
	cmp/hi	r0,r1
	bt	consumed
	! r6 the points, r7 where the window ends
	mov	r1,r6
	mov	r2,r7
	add	r1,r7
	mov.l	p_published,r4
	mov.l	@r4,r0
	cmp/hs	r0,r2
	bf	2f
	mov.l	p_highest,r5
	mov.l	@r5,r0
	mov	r2,r4
	sub	r0,r4
	add	#38,r4
	mov	#76,r0
	cmp/hi	r0,r4
	bf	2f
	mov.l	p_pending,r4
	mov.w	c_seek,r0
	mov.l	@r4,r5
	cmp/hs	r0,r5
	bt	2f
	mov.l	r0,@r4
2:	mov.l	p_highest,r5
	mov.l	@r5,r0
	cmp/hs	r7,r0
	bt	3f
	mov.l	r7,@r5
3:	mov.l	p_low,r5
	mov.l	@r5,r0
	cmp/hs	r0,r2
	bt	5f
	mov.l	r2,@r5
5:	mov.l	@(16,r11),r4
	shll	r2
	add	r2,r4
	mov	r10,r5
	add	#14,r5
	shll	r1
7:	mov.b	@r5+,r0
	dt	r1
	mov.b	r0,@r4
	bf/s	7b
	add	#1,r4
	mov.l	p_pending,r5
	mov.l	@r5,r0
	add	r6,r0
	cmp/eq	r3,r7
	bt	8f
	mov.w	c_publish,r1
	cmp/hs	r1,r0
	bt	8f
	bra	9f
	mov.l	r0,@r5
8:	mov.l	p_highest,r4
	mov.l	@r4,r0
	mov.l	p_published,r4
	mov.l	r0,@r4
	mov	#0,r0
	mov.l	r0,@r5
	mov.l	p_owed,r4
	mov.l	@r4,r0
	tst	r0,r0
	bf	9f
	mov	#1,r0
	mov.l	r0,@r4
9:	bsr	train
	nop
	bra	consumed
	nop

! the points that ask for a train: 10 s at 150 points a second
c_publish:	.word 1500
! the count a seek moves it to
c_seek:		.word 1500 - 450


	.p2align 2
p_buffer:	.long 0x081FFBE0
p_stock:	.long 0x0BD50FDC
p_state:	.long 0x0BD50E90
p_auth:		.long 0x0BD4B144
p_frame:	.long 0x084F9DB2
p_overview:	.long 0x0BE53428
p_grid:		.long 0x0BF5BFFC
p_detail:	.long 0x0BE54510
p_memset:	.long 0x08533D50
p_pending:	.long 0x081FEF98
p_highest:	.long 0x081FEF94
p_published:	.long 0x081FEF90
p_low:		.long 0x081FEF8C
p_owed:		.long 0x081FEF88
p_browse:	.long 0x081FB540
! the points the detail buffer holds
c_points:	.long 540007

list:
	! r1 the fragments of the list, r2 this one, r3 where the one expected next is kept
	mov.w	@(4,r10),r0
	extu.w	r0,r1
	mov.w	@(2,r10),r0
	extu.w	r0,r2
	mov	#4,r0
	cmp/hi	r0,r1
	bt	consumed
	tst	r2,r2
	bt	consumed
	cmp/hi	r1,r2
	bt	consumed
	mov.l	p_next,r3
	mov	r2,r0
	cmp/eq	#1,r0
	bt	1f
	mov.l	@r3,r0
	cmp/eq	r2,r0
	bf	consumed
1:	mov	r2,r0
	cmp/eq	r1,r2
	bf/s	2f
	add	#1,r0
	mov	#0,r0
2:	mov.l	r0,@r3
	! a list still owed is lost once the stage is written again
	mov.l	p_listowed,r1
	mov	#0,r0
	mov.l	r0,@r1
	! r4 the fragment, r5 where it goes in the stage, r6 its bytes
	add	#-1,r2
	mov.w	c_fragment,r0
	mul.l	r0,r2
	sts	macl,r5
	mov.l	p_stage,r0
	add	r0,r5
	mov	r10,r4
	add	#6,r4
	mov	r9,r6
	add	#-6,r6
3:	mov.b	@r4+,r0
	dt	r6
	mov.b	r0,@r5
	bf/s	3b
	add	#1,r5
	mov.l	@r3,r0
	tst	r0,r0
	bf	consumed
	mov.l	p_listbrowse,r1
	mov.l	@r1,r0
	cmp/eq	#1,r0
	bt	browse
	mov.l	p_list,r11
	mov.l	p_stage,r4
	bsr	walk
	nop
	bra	reinject
	mov	r11,r4

	! with x3000browse the list is committed once the guard is free: now, or
	! at the next report
browse:
	mov.l	p_stage,r0
	sub	r0,r5
	mov.l	p_listowed,r1
	bsr	commit
	mov.l	r5,@r1
	bra	consumed
	nop

	.p2align 2
p_listowed:	.long 0x081FB56C
p_listbrowse:	.long 0x081FB540

	! The records from r4 to r5 are walked twice into the list at r11. The
	! first walk finds the cursor's row (r12) and the last row (r13); the
	! second, with r8 the first row of the window, writes row 0 and rows
	! r8..r8+7 as rows 1-8. Returns r5 = the bytes of the list and r7 = the
	! records walked; uses r0-r4, r6, r8-r10 and r14.
walk:
	sts.l	pr,@-r15
	mov.l	r4,@-r15
	mov	#0,r8
	mov	#0,r12
	mov	#0,r13
pass:
	! r4 the record, r6 the output, r7 records seen
	mov.l	@r15,r4
	mov	r11,r6
	add	#6,r6
	mov	#0,r7
record:
	mov	r4,r0
	add	#8,r0
	cmp/hi	r5,r0
	bt	rows
	mov	#12,r1
	cmp/hs	r1,r7
	bt	rows
	mov	r6,r0
	sub	r11,r0
	mov.w	c_room,r1
	cmp/hi	r1,r0
	bt	rows
	tst	r7,r7
	bt	3f
	mov	r4,r0
	add	#10,r0
	cmp/hi	r5,r0
	bt	rows
	mov	r4,r1
	mov	#10,r2
	mov	#0,r3
4:	mov.b	@r1+,r0
	dt	r2
	bf/s	4b
	or	r0,r3
	tst	r3,r3
	bt	rows
	! r10 set: the record is left out
3:	mov.b	@r4,r0
	extu.b	r0,r0
	tst	r8,r8
	bf/s	7f
	mov	#1,r10
	cmp/hi	r13,r0
	bf	8f
	mov	r0,r13
8:	mov	r0,r1
	mov.b	@(1,r4),r0
	tst	#0x80,r0
	bt	5f
	bra	5f
	mov	r1,r12
7:	tst	r0,r0
	bt	9f
	sub	r8,r0
	mov	#8,r1
	cmp/hs	r1,r0
	bt	5f
	add	#1,r0
9:	mov	#0,r10
	mov	r4,r14
	mov.b	r0,@r6
	mov.b	@(1,r4),r0
	mov.b	r0,@(1,r6)
	mov.b	@(2,r4),r0
	mov.b	r0,@(2,r6)
	mov.b	@(3,r4),r0
	mov.b	r0,@(3,r6)
	add	#4,r6
	! r9 the characters left; a track keeps 12 for its BPM and key
	mov.b	@(1,r4),r0
	and	#0x7f,r0
	cmp/eq	#4,r0
	bf/s	5f
	mov	#127,r9
	mov	#115,r9
5:	add	#8,r4
unit:
	mov	r4,r0
	add	#2,r0
	cmp/hi	r5,r0
	bt	text_end
	mov.b	@r4+,r2
	mov.b	@r4+,r3
	extu.b	r2,r0
	extu.b	r3,r1
	or	r1,r0
	tst	r0,r0
	bt	text_end
	tst	r10,r10
	bf	unit
	tst	r9,r9
	bt	unit
	mov.b	r2,@r6
	mov	r3,r0
	mov.b	r0,@(1,r6)
	add	#2,r6
	bra	unit
	add	#-1,r9
text_end:
	tst	r10,r10
	bf	6f
	mov.b	@(1,r14),r0
	and	#0x7f,r0
	cmp/eq	#4,r0
	bf	8f
	! a track gets " 102.5 1A": the BPM (b4-b5, the tenths in b3) and,
	! when b6 is a minor key, its Camelot number
	mov.w	@(4,r14),r0
	extu.w	r0,r1
	tst	r1,r1
	bt	8f
	mov	#0x20,r0
	mov.w	r0,@r6
	bsr	decimal
	add	#2,r6
	mov	#0x2e,r0
	mov.w	r0,@r6
	add	#2,r6
	mov.b	@(3,r14),r0
	bsr	decimal
	extu.b	r0,r1
	mov.b	@(6,r14),r0
	extu.b	r0,r1
	shlr	r1
	bt	8f
	add	#-1,r1
	mov	#12,r0
	cmp/hs	r0,r1
	bt	8f
	mova	c_camelot,r0
	mov.b	@(r0,r1),r1
	mov	#0x20,r0
	mov.w	r0,@r6
	bsr	decimal
	add	#2,r6
	mov	#0x41,r0
	mov.w	r0,@r6
	add	#2,r6
8:	mov	#0,r0
	mov.w	r0,@r6
	add	#2,r6
6:	bra	record
	add	#1,r7

rows:
	tst	r8,r8
	bf	header
	! the window starts 4 rows above the cursor, no lower than 7 rows above
	! the last row, and at row 1 at the earliest
	mov	r12,r8
	add	#-4,r8
	mov	r13,r0
	add	#-7,r0
	cmp/gt	r0,r8
	bf	1f
	mov	r0,r8
1:	mov	#1,r0
	cmp/gt	r8,r0
	bf	2f
	mov	r0,r8
2:	bra	pass
	nop
header:
	mov.l	c_head,r0
	mov.l	r0,@r11
	mov	#1,r0
	mov.w	r0,@(4,r11)
	mov	r6,r5
	sub	r11,r5
	add	#4,r15
	lds.l	@r15+,pr
	rts
	nop

! the output may hold 2,400 bytes after its header; a record adds at most 260
c_room:		.word 6 + 2400 - 260
! the bytes of a fragment after its header
c_fragment:	.word 1018

	! r1 in decimal at r6, without leading zeros; uses r0, r2, r3 and r9
decimal:
	mova	c_powers,r0
	mov	r0,r2
	mov	#0,r3
1:	mov.w	@r2+,r0
	tst	r0,r0
	bt	4f
	mov	#0x30,r9
2:	cmp/hs	r0,r1
	bf	3f
	sub	r0,r1
	bra	2b
	add	#1,r9
3:	mov	r9,r0
	cmp/eq	#0x30,r0
	bf	5f
	tst	r3,r3
	bt	1b
5:	mov	#1,r3
	mov.w	r0,@r6
	bra	1b
	add	#2,r6
4:	mov	r1,r0
	add	#0x30,r0
	mov.w	r0,@r6
	rts
	add	#2,r6

	.p2align 2
c_head:		.long 0x00012a00
p_list:		.long 0x081FEFE0
p_stage:	.long 0x081FDFA0
p_next:		.long 0x081FDF9C
! the Camelot number of each minor key, from A minor a semitone apart
c_camelot:	.byte 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1
c_powers:	.word 10000, 1000, 100, 10, 0

	! A train owed (1) is armed once the send state and the grid after it
	! are idle, with the stock armer, which copies the header, every point
	! and the grid to the send buffer; the train is then cut to start at the
	! chunk of the first point written since the last one and end just past
	! the highest point.
	! The display stages each chunk at its index and copies the detail up to
	! the end of the last one, so the chunks before stay as it has them. The
	! grid (0x2b) that has to follow for the display to take the detail is
	! armed here too and cut to its last chunk, which the display handles the
	! same way, and the record's word +44 asks for the beat ticks (0x38) once
	! the grid is sent, as the publisher does. Owed 2 waits for the publisher
	! to take a request of the framework, then puts the length back. r12 the
	! status register, r13 the owed word; interrupts are masked while the send
	! states are written.
train:
	sts.l	pr,@-r15
	mov.l	q_owed,r13
	mov.l	@r13,r0
	cmp/eq	#2,r0
	bf	1f
	mov.l	q_request,r1
	mov.l	@r1,r1
	tst	r1,r1
	bf	9f
	mov.l	q_saved,r1
	mov.l	@r1,r1
	mov.l	q_detail,r2
	mov.l	r1,@(4,r2)
	mov	#1,r0
	mov.l	r0,@r13
1:	cmp/eq	#1,r0
	bf	9f
	mov.l	q_highest,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	9f
	stc	sr,r12
	mov	r12,r0
	or	#0xf0,r0
	ldc	r0,sr
	mov.l	q_send,r4
	mov.b	@r4,r0
	mov.l	q_train,r1
	mov.b	@r1,r2
	or	r2,r0
	tst	r0,r0
	bf	8f
	mov.b	r0,@(1,r1)
	mov.l	q_gridsend,r2
	mov.b	r0,@r2
	mov	#1,r0
	mov.b	r0,@r1
	mov.l	q_detail,r2
	mov.l	@(4,r2),r0
	add	#20,r0
	mov.l	q_record,r2
	mov.l	r0,@(32,r2)
	mov	#1,r0
	mov.l	r0,@(44,r2)
	mov.l	q_armer,r1
	jsr	@r1
	nop
	! r2 the index of the first chunk, r3 where the train ends, just past
	! the highest point: a point p is byte 32 + 2p of the train
	mov.l	q_send,r4
	mov.l	q_low,r1
	mov.l	@r1,r2
	shll	r2
	add	#32,r2
	shlr8	r2
	shlr2	r2
	shlr2	r2
	shlr	r2
	mov.l	q_highest,r1
	mov.l	@r1,r3
	shll	r3
	add	#32,r3
	mov.l	@(8,r4),r1
	cmp/hi	r1,r3
	bf	2f
	mov	r1,r3
2:	mov.l	r2,@(24,r4)
	shll8	r2
	shll2	r2
	shll2	r2
	shll	r2
	mov.l	r2,@(12,r4)
	mov	r3,r0
	sub	r2,r0
	mov.l	r0,@(16,r4)
	mov	r3,r0
	add	#-1,r0
	shlr8	r0
	shlr2	r0
	shlr2	r0
	shlr	r0
	add	#1,r0
	mov.l	r0,@(20,r4)
	mov.l	q_gridarmer,r1
	jsr	@r1
	nop
	mov.l	q_gridsend,r4
	mov.l	@(20,r4),r2
	add	#-1,r2
	mov.l	r2,@(24,r4)
	shll8	r2
	shll2	r2
	shll2	r2
	shll	r2
	mov.l	r2,@(12,r4)
	mov.l	@(8,r4),r3
	sub	r2,r3
	mov.l	r3,@(16,r4)
	mov	#0,r0
	mov.l	r0,@r13
	mov	#-1,r0
	mov.l	q_low,r1
	mov.l	r0,@r1
8:	ldc	r12,sr
9:	lds.l	@r15+,pr
	rts
	nop

	! The framework asks the publisher for the whole detail when a detail,
	! grid or overview it is handed is complete. Until the publisher has
	! taken the request the header's length reads 0, so that train is the
	! header alone (and the grid after it); the publisher still fills the
	! record with the grid. The display then has none of the points, and
	! the next train starts at chunk 1. Uses r0-r2.
asked:
	mov.l	q_request,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	2f
	mov.l	q_owed,r2
	mov.l	@r2,r0
	cmp/eq	#2,r0
	bt	1f
	mov	#2,r0
	mov.l	r0,@r2
	mov.l	q_detail,r1
	mov.l	@(4,r1),r0
	mov.l	q_saved,r2
	mov.l	r0,@r2
	mov	#0,r0
	mov.l	r0,@(4,r1)
1:	mov.l	q_low,r1
	mov	#0,r0
	mov.l	r0,@r1
2:	rts
	nop

	.p2align 2
q_owed:		.long 0x081FEF88
q_low:		.long 0x081FEF8C
q_highest:	.long 0x081FEF94
q_saved:	.long 0x081FDF98
q_detail:	.long 0x0BE54510
q_request:	.long 0x0A35F404
q_send:		.long 0x09FF85EC
q_train:	.long 0x10DC1544
q_gridsend:	.long 0x09FF8608
q_record:	.long 0x0B55059C
q_armer:	.long 0x08391D7C
q_gridarmer:	.long 0x08391F2E

	! A whole list while x3000browse runs, its bytes in the owed word (0 for
	! none). x3000browse calls the framework from the HID IN task, so the
	! list record, the answer and the counters are kept behind a guard byte
	! taken with tas.b. The HID IN task has the lower priority: this task can
	! find the guard taken but never waits for it, since the holder cannot run
	! until it returns. The list stays owed and is committed at the next
	! report, or at the read's timeout; in every list Serato sent, the next
	! two reports came within 2 ms. Uses r0-r7.
	!
	! The answer is kept for the prediction (see show). A library list is a
	! header of style 0, at least eight rows and the cursor. It answers the
	! oldest step not answered yet, if there is one, and the rows above row 1
	! become what the prediction of that step made them; a cursor above row 5
	! puts the crate's top on row 1. Any other list is shown as it is.
commit:
	mov.l	b_owed,r1
	mov.l	@r1,r6
	tst	r6,r6
	bt	9f
	mov.l	b_guard,r2
	tas.b	@r2
	bf	9f
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov	#0,r0
	mov.l	r0,@r1
	mov.l	b_length,r1
	mov.l	r6,@r1
	mov.l	b_stage,r1
	mov.l	b_answer,r4
	mov	r4,r5
1:	mov.b	@r1+,r0
	dt	r6
	mov.b	r0,@r5
	bf/s	1b
	add	#1,r5
	mov.l	b_list,r11
	bsr	walk
	nop
	mov	#9,r0
	cmp/hs	r0,r7
	bf	2f
	tst	r12,r12
	bt	2f
	mov.l	b_answer,r0
	mov.b	@(1,r0),r0
	tst	#0x7f,r0
	bt	3f
2:	mov	#0,r0
	mov.l	b_live,r1
	mov.l	r0,@r1
	bsr	deliver
	nop
	bra	8f
	nop
	! r1 the step answered; r6, r7 the answer's cursor and last row
3:	mov.l	b_steps,r1
	mov.l	@r1,r0
	mov.l	b_answered,r2
	mov.l	@r2,r3
	sub	r3,r0
	cmp/pl	r0
	bf	5f
	mov	r3,r0
	add	#1,r3
	mov.l	r3,@r2
	and	#31,r0
	mov.l	b_ring,r1
	mov.b	@(r0,r1),r1
	mov.l	b_live,r2
	mov.l	@r2,r0
	cmp/eq	#1,r0
	bf	5f
	mov	r12,r6
	mov	r13,r7
	mov	#0,r8
	mov	#1,r9
	mov.l	b_last,r0
	mov.l	@r0,r10
	mov.l	b_cursor,r0
	mov.l	@r0,r12
	mov.l	b_top,r0
	bsr	step
	mov.l	@r0,r13
	mov.l	b_top,r0
	mov.l	r13,@r0
	mov	r6,r12
	mov	r7,r13
5:	mov	#5,r0
	cmp/ge	r0,r12
	bt	6f
	mov.l	b_top,r1
	mov	#0,r0
	mov.l	r0,@r1
6:	mov.l	b_cursor,r1
	mov.l	r12,@r1
	mov.l	b_last,r1
	mov.l	r13,@r1
	mova	show,r0
	mov.l	b_show,r1
	mov.l	r0,@r1
	mov.l	b_live,r1
	mov	#1,r0
	bsr	show
	mov.l	r0,@r1
8:	mov.l	b_guard,r1
	mov	#0,r0
	mov.b	r0,@r1
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
9:	rts
	nop

	! The list the deck shows ahead of Serato's answer: the last answer with
	! every step not answered yet applied to it (see step), rewritten as a
	! 3000X list in the scratch stage and walked like one. Rows the deck does
	! not have (a new top row) are blank. Called with the guard taken, from
	! commit and from x3000browse in the HID IN task; returns r0 = 1 when the
	! list went to the framework.
	.p2align 2
show:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov	#0,r8
	mov	#1,r9
	mov.l	b_last,r0
	mov.l	@r0,r10
	mov.l	b_cursor,r0
	mov.l	@r0,r12
	mov.l	b_top,r0
	mov.l	@r0,r13
	mov.l	b_answered,r0
	mov.l	@r0,r11
	mov.l	b_steps,r0
	mov.l	@r0,r14
1:	mov	r14,r0
	sub	r11,r0
	cmp/pl	r0
	bf	2f
	mov	r11,r0
	and	#31,r0
	mov.l	b_ring,r1
	mov.b	@(r0,r1),r1
	bsr	step
	add	#1,r11
	bra	1b
	nop

	! r4 the record, r5 the end of the answer, r6 the rewrite, r7 records
	! seen, r3 the record's row in the prediction, r11 set when it is left out
2:	mov.l	b_answer,r4
	mov.l	b_length,r0
	mov.l	@r0,r5
	add	r4,r5
	mov.l	b_scratch,r6
	mov	#0,r7
rewrite:
	mov	r4,r0
	add	#8,r0
	cmp/hi	r5,r0
	bt	rewritten
	mov	#12,r0
	cmp/hs	r0,r7
	bt	rewritten
	tst	r7,r7
	bt	4f
	mov	r4,r0
	add	#10,r0
	cmp/hi	r5,r0
	bt	rewritten
	mov	r4,r1
	mov	#10,r2
	mov	#0,r3
3:	mov.b	@r1+,r0
	dt	r2
	bf/s	3b
	or	r0,r3
	tst	r3,r3
	bt	rewritten
4:	mov.b	@r4,r0
	extu.b	r0,r3
	mov	#0,r11
	tst	r3,r3
	bt	6f
	cmp/ge	r9,r3
	bf	5f
	cmp/gt	r10,r3
	bt	5f
	bra	6f
	sub	r8,r3
5:	mov	#1,r11
6:	tst	r11,r11
	bf	8f
	! the row, the cursor flag on the cursor's row (row 0 keeps its own),
	! the other six bytes as they are
	mov	r3,r0
	mov.b	r0,@r6
	mov.b	@(1,r4),r0
	tst	r3,r3
	bt	7f
	and	#0x7f,r0
	cmp/eq	r3,r12
	bf	7f
	or	#0x80,r0
7:	mov.b	r0,@(1,r6)
	mov	r4,r1
	add	#2,r1
	add	#2,r6
	mov	#6,r2
9:	mov.b	@r1+,r0
	dt	r2
	mov.b	r0,@r6
	bf/s	9b
	add	#1,r6
	add	#-8,r6
8:	add	#8,r4
	tst	r11,r11
	bf	unit2
	add	#8,r6
unit2:
	mov	r4,r0
	add	#2,r0
	cmp/hi	r5,r0
	bt	blank
	mov.b	@r4+,r1
	mov.b	@r4+,r2
	tst	r11,r11
	bf	1f
	mov.b	r1,@r6
	mov	r2,r0
	mov.b	r0,@(1,r6)
	add	#2,r6
1:	extu.b	r1,r0
	extu.b	r2,r1
	or	r1,r0
	tst	r0,r0
	bf	unit2

	! after the first record, the rows above the first one the answer has:
	! 1 up to a - s - 1, no further than the last row
blank:
	tst	r7,r7
	bf	3f
	mov	r9,r1
	sub	r8,r1
	mov	r10,r2
	sub	r8,r2
	add	#1,r2
	cmp/gt	r2,r1
	bf	1f
	mov	r2,r1
1:	mov	#1,r3
2:	cmp/ge	r1,r3
	bt	3f
	mov	r3,r0
	mov.b	r0,@r6
	mov	#0,r0
	cmp/eq	r3,r12
	bf	4f
	mov	#-128,r0
4:	mov.b	r0,@(1,r6)
	mov	#0,r0
	mov	r6,r2
	add	#2,r2
	mov	#8,r14
5:	dt	r14
	mov.b	r0,@r2
	bf/s	5b
	add	#1,r2
	add	#10,r6
	bra	2b
	add	#1,r3
3:	bra	rewrite
	add	#1,r7

rewritten:
	mov	r6,r5
	mov.l	b_scratch,r4
	mov.l	b_list,r11
	bsr	walk
	nop
	bsr	deliver
	nop
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

	! The list in the list buffer, r5 bytes, to the framework unless it is
	! the one handed over last; r0 = 1 when it went. Uses r0-r7.
deliver:
	mov.l	b_sentlength,r7
	mov.l	b_list,r4
	mov.l	b_sent,r6
	mov.l	@r7,r0
	cmp/eq	r5,r0
	bf	2f
	mov	r4,r1
	mov	r6,r2
	mov	r5,r3
1:	mov.b	@r1+,r0
	mov.b	@r2+,r7
	cmp/eq	r7,r0
	bf	2f
	dt	r3
	bf	1b
	rts
	mov	#0,r0
2:	mov.l	b_sentlength,r7
	mov.l	r5,@r7
	mov	r4,r1
	mov	r5,r3
3:	mov.b	@r1+,r0
	dt	r3
	mov.b	r0,@r6
	bf/s	3b
	add	#1,r6
	sts.l	pr,@-r15
	mov.l	b_frame,r1
	jsr	@r1
	nop
	lds.l	@r15+,pr
	rts
	mov	#1,r0

	! One step r1 (1 down, -1 up) applied as Serato answers it: the cursor
	! moves while it is above row 5 or on the last row, and on row 5 the rows
	! move instead, unless the crate's top is on row 1. Row r of the
	! prediction is row r + s of the answer, which is known for rows a..b,
	! and the last row is b - s: r8 = s, r9 = a, r10 = b, r12 the cursor, r13
	! the rows above row 1 (negative while not known). Uses r0 and r2.
step:
	mov	r10,r2
	sub	r8,r2
	mov	#5,r0
	cmp/pl	r1
	bf	up
	cmp/gt	r12,r0
	bt	1f
	cmp/ge	r2,r12
	bf	2f
1:	cmp/ge	r2,r12
	bt	3f
	rts
	add	#1,r12
3:	rts
	mov	r2,r12
	! the rows move up: the first known row goes
2:	mov	r8,r0
	add	#2,r0
	cmp/gt	r9,r0
	bf	4f
	mov	r0,r9
4:	add	#1,r8
	cmp/pz	r13
	bf	5f
	add	#1,r13
5:	rts
	nop
up:
	cmp/gt	r0,r12
	bf	6f
	rts
	add	#-1,r12
6:	tst	r13,r13
	bt	7f
	cmp/ge	r0,r12
	bt	8f
7:	mov	#1,r0
	cmp/gt	r0,r12
	bf	9f
	add	#-1,r12
9:	rts
	nop
	! the rows move down: the last row goes and row 1 is new
8:	add	#-1,r10
	add	#-1,r8
	cmp/pl	r13
	bf	9b
	rts
	add	#-1,r13

	.p2align 2
b_owed:		.long 0x081FB56C
b_guard:	.long 0x081FB544
b_length:	.long 0x081FB558
b_stage:	.long 0x081FDFA0
b_answer:	.long 0x081FCF80
b_list:		.long 0x081FEFE0
b_live:		.long 0x081FB550
b_steps:	.long 0x081FB54C
b_answered:	.long 0x081FB548
b_ring:		.long 0x081FB520
b_last:		.long 0x081FB560
b_cursor:	.long 0x081FB55C
b_top:		.long 0x081FB564
b_show:		.long 0x081FB554
b_scratch:	.long 0x081FBF00
b_sentlength:	.long 0x081FB568
b_sent:		.long 0x081FB580
b_frame:	.long 0x084F9DB2
