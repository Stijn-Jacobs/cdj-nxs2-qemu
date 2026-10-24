! SPDX-License-Identifier: GPL-2.0-or-later
!
! The deck's state report in the CDJ-3000X layout, for the CDJ-2000NXS2 MAIN
! firmware 1.87 (SH7724, SH-4, little-endian).
!
! The deck reports its controls to the host in report 0x20. A CDJ-3000X has
! its hot cue pads one byte further on (byte 16, NXS2 15) and its analog block
! too: the browse encoder at bytes 20-21 (NXS2 19-20), then tempo, jog count,
! jog speed and needle position up to byte 29. Serato reads a deck with the
! 3000X identity in the 3000X layout, so with the NXS2's own report it never
! sees the encoder turn that selects a deck. Byte 6 holds the slip-reverse
! and reverse bits (6 and 5), which the NXS2 sets while they are off; a 3000X
! sets them while they are on, so Serato played every track backwards.
!
! Hook: the HID IN task (USBHIDI), right after it takes the next report from
! the two-slot ring the report sender 0x084F4ECA fills, before the class send
! 0x08500B7A writes it to the IN pipe. The slot is the task's own: it is
! rewritten in place. Only a 0x20 report is changed.
!
! r4 = the saved r0-r7: r0 = slot index x 4 at offset 0, r6 = length at 24,
! r7 = the slot pointer table at 28. r0-r7 are free; r8-r14 are kept.

	.text
x3000in:
	mov.l	@(24,r4),r6
	mov	#30,r1
	cmp/ge	r1,r6
	bf	out
	mov	#64,r1
	cmp/gt	r1,r6
	bt	out
	mov.l	@r4,r0
	mov.l	@(28,r4),r1
	mov.l	@(r0,r1),r5
	mov.b	@(1,r5),r0
	cmp/eq	#0x20,r0
	bf	out
	mov.b	@(6,r5),r0
	xor	#0x60,r0
	mov.b	r0,@(6,r5)
	! hot cues 15 -> 16
	mov.b	@(15,r5),r0
	mov	#0,r2
	mov	r5,r3
	add	#16,r3
	mov.b	r0,@r3
	mov.b	r2,@-r3
	! 19-28 -> 20-29, from the top down
	mov	r5,r3
	add	#28,r3
	mov	r5,r7
	add	#29,r7
	mov	#10,r1
1:	mov.b	@r3,r0
	add	#-1,r3
	dt	r1
	mov.b	r0,@r7
	bf/s	1b
	add	#-1,r7
	mov.b	r2,@r7
out:
	rts
	nop
