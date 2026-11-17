! SPDX-License-Identifier: GPL-2.0-or-later
!
! Receive Serato's 1024-byte HID Output reports whole, for the CDJ-2000NXS2
! MAIN firmware 1.87 (SH7724, SH-4, little-endian).
!
! Serato DJ Pro sends a CDJ-3000X its detail waveform only when the deck's
! report descriptor declares a 1024-byte Output report (x3000out). The HID
! OUT endpoint stays at 64 bytes, so each report arrives as 16 packets of one
! transfer. The deck's HID OUT task reads a report with the channel read
! 0x0850074C(handle, buffer, length): its pipe reader keeps taking full
! packets until the length is reached or a short packet ends the transfer,
! and returns the count. The task asks for the word at 0x0BD4B108, which it
! sets to 64 once at start, into the 128-byte buffer 0x0BD50FDC; asked for 64
! while the host sends 1024, the read ends after the first packet and the
! other fifteen come in as reports of their own.
!
! Hook: the instructions in front of the read call, which load r13 = that
! length word, r14 = the read, r5 = the buffer and r4 = the channel handle.
! This points the read at a 1024-byte buffer of its own and sets the length
! word to 1024; the call that follows loads r6 from it in its delay slot.
! x3000rxview then hands the stock code the first 64 bytes in its own buffer.
!
! The buffer sits near the end of the erased run the routines are placed in,
! clear of the words they hand over to each other in its last 16 bytes.
!
! r4 = the saved r0-r7, r5 at offset 20. r0-r7 are free; r8-r14 are kept.

	.text
x3000rxbuf:
	mov.l	p_buffer,r0
	mov.l	r0,@(20,r4)
	mov	#4,r0
	shll8	r0
	rts
	mov.l	r0,@r13

	.p2align 2
p_buffer:	.long 0x081FFBE0
