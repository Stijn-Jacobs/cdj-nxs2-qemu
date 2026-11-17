! SPDX-License-Identifier: GPL-2.0-or-later
!
! Real 3-band detail waveform for the CDJ-2000NXS2 MAIN firmware (SH7724,
! SH-4, little-endian): writes the 3-band words main_wave3data.s packed into
! the waveform record the deck sends to the display.
!
! Hook: the waveform requester, right after the firmware has re-encoded the
! builder's PWV5 record into its own form and stored the new record at
! 0x0B531EB8, before the record is published. That re-encoding leaves bits
! 1-0 of every word clear, which is what lets the display take a word with
! bit 0 set as 3-band. The words at 0x081FFFF4 (buffer) and 0x081FFFF8 (entry
! count) are the current load's; with no buffer, or a record that is not the
! same 2-byte-entry length, the stock words go out untouched.
!
! r0-r3 are the only registers used; the trampoline restores the span's own.

	.text
wave3detail:
	mov.l	p_words,r0
	mov.l	@r0,r1			! packed words, 0 or -1 when there are none
	cmp/pl	r1
	bf	out
	mov.l	@(4,r0),r2		! n
	mov.l	p_record,r3
	mov.l	@r3,r3			! the re-encoded record
	tst	r3,r3
	bt	out
	mov.w	@(8,r3),r0
	cmp/eq	#2,r0
	bf	out
	mov.l	@r3,r0
	cmp/eq	r2,r0
	bf	out
	mov.l	@(16,r3),r3
word:
	mov.w	@r1+,r0
	mov.w	r0,@r3
	add	#2,r3
	dt	r2
	bf	word
out:
	rts
	nop

	.p2align 2
p_words:	.long 0x081FFFF4
p_record:	.long 0x0B531EB8
