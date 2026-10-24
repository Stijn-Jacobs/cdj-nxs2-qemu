! SPDX-License-Identifier: GPL-2.0-or-later
!
! The CDJ-3000X answer to Serato's HID handshake, for the CDJ-2000NXS2 MAIN
! firmware 1.87 (SH7724, SH-4, little-endian).
!
! The deck opens the 0xF0 handshake with its hello (stage 0x11). A host that
! knows the deck as an NXS2 answers with stage 0x12 and the five-stage
! challenge runs. Serato knows a deck with the 3000X identity as a 3000X and
! answers the hello with stage 0x30 instead: `30 05 01 03 <host id>`, the id
! 0x31 for Serato DJ Pro. A CDJ-3000X (v1.40) then sends stage 0x31, its
! identity as TLVs (type, length counting type and length, data), and is
! done:
!
!   1  vendor id, big-endian
!   2  "AlphaTheta"
!   3  product id, big-endian
!   4  "CDJ3000X"
!   5  the serial number
!   6  firmware version, major and minor: 1, 40
!
! Hook: the entry of the host-report router 0x084F9504, which hands a received
! report to the handshake handler while the deck is not authenticated and to
! the report dispatchers once it is. The router is called from a different
! task than the one that runs the handshake state machine (0x084FE9BC), so the
! state word 0x0BD4B148 is not a reliable "a 0x30 is due" test here; the
! authenticated flag 0x0BD4B144 is. This answers a single-fragment 0x30 while
! that flag is still 0, which is exactly the one Serato sends after the deck's
! hello. The reply goes out through the deck's own report sender 0x084F4ECA,
! from a buffer on the stack (the sender copies it). Then the deck takes the
! state a verified stage 0x14 leaves it in, written as the stage-5 arm of
! 0x084FE9BC writes it: state 6 under the lock whose id is at 0x10DE1BE4
! (taken only when 0x08518068 says the caller may wait), then the flag = 1.
! With the flag set the router passes this report, and every one after it, to
! the dispatchers, and the deck stops re-sending its hello; a later 0x30 is
! left alone because the flag is no longer 0.
!
! The router is entered with the 64-byte report in r4; a hook reaches it at
! offset 16 of the saved r0-r7 the shared trampoline hands it in r4.
!
! The vendor and product ids are read from the device descriptor the deck
! serves, the RAM copy at 0x10DDFBA0 that the USB bring-up makes of the
! table in the image, so they are whatever the identity patch put there. The serial is
! the one the factory wrote to the flash's last sector at 0xA07FE000, ASCII,
! up to the first byte that is not printable or 16 bytes.
!
! r0-r7 are free; r8-r14 are kept.

	.text
x3000hs:
	mov.l	p_auth,r1
	mov.l	@r1,r0
	tst	r0,r0
	bf	out
	mov.l	@(16,r4),r6
	mov.b	@(1,r6),r0
	cmp/eq	#-16,r0
	bf	out
	mov.b	@(2,r6),r0
	cmp/eq	#1,r0
	bf	out
	mov.b	@(3,r6),r0
	tst	r0,r0
	bf	out
	mov.b	@(4,r6),r0
	cmp/eq	#1,r0
	bf	out
	mov.b	@(5,r6),r0
	tst	r0,r0
	bf	out
	mov.b	@(6,r6),r0
	cmp/eq	#0x30,r0
	bf	out

	sts.l	pr,@-r15
	add	#-64,r15
	mov.l	p_memset,r1
	mov	r15,r4
	mov	#0,r5
	jsr	@r1
	mov	#64,r6

	! the fixed head: report header, TLVs 1-4
	mova	head,r0
	mov	r0,r5
	mov	r15,r4
	mov	#head_end-head,r6
1:	mov.b	@r5+,r0
	dt	r6
	mov.b	r0,@r4
	bf/s	1b
	add	#1,r4
	mov.l	p_device,r1
	mov.b	@(9,r1),r0
	mov.b	r0,@(10,r15)
	mov.b	@(8,r1),r0
	mov.b	r0,@(11,r15)
	mov	r15,r2
	add	#26,r2
	mov.b	@(11,r1),r0
	mov.b	r0,@r2
	mov.b	@(10,r1),r0
	mov.b	r0,@(1,r2)

	! TLV 5, the serial; r4 points at its type byte
	mov	r4,r7
	add	#2,r4
	mov.l	p_serial,r5
	mov	#0,r6
2:	mov.b	@r5+,r0
	extu.b	r0,r0
	mov	#0x20,r1
	cmp/hs	r1,r0
	bf	3f
	mov	#0x7F,r1
	cmp/hs	r1,r0
	bt	3f
	mov.b	r0,@r4
	add	#1,r4
	add	#1,r6
	mov	#16,r1
	cmp/hs	r1,r6
	bf	2b
3:	mov	#5,r0
	mov.b	r0,@r7
	mov	r6,r0
	add	#2,r0
	mov.b	r0,@(1,r7)

	! TLV 6, the version, and the length of everything after the header
	mov	#6,r0
	mov.b	r0,@r4
	mov	#4,r0
	mov.b	r0,@(1,r4)
	mov	#1,r0
	mov.b	r0,@(2,r4)
	mov	#40,r0
	mov.b	r0,@(3,r4)
	! from the stage on: the head past its six header bytes, TLV 5, TLV 6
	mov	r6,r0
	add	#head_end-head-6+2+4,r0
	mov.b	r0,@(7,r15)

	mov.l	p_send,r1
	mov	r15,r4
	jsr	@r1
	mov	#64,r5

	mov.l	p_in_task,r1
	jsr	@r1
	nop
	tst	r0,r0
	bf/s	4f
	mov	#1,r6
	mov.l	p_lock_id,r1
	mov.l	p_lock,r2
	mov.l	@r1,r4
	jsr	@r2
	mov	#-1,r5
	mov	r0,r6
4:	mov.l	p_state,r1
	mov	#6,r0
	tst	r6,r6
	bf/s	5f
	mov.l	r0,@r1
	mov.l	p_lock_id,r1
	mov.l	p_unlock,r2
	jsr	@r2
	mov.l	@r1,r4
5:	mov.l	p_auth,r1
	mov	#1,r0
	mov.l	r0,@r1

	add	#64,r15
	lds.l	@r15+,pr
out:
	rts
	nop

	.p2align 2
head:
	.byte	0x00, 0xF0, 0x01, 0x00, 0x01, 0x00, 0x31, 0x00
	.byte	0x01, 0x04, 0x00, 0x00
	.byte	0x02, 0x0C
	.ascii	"AlphaTheta"
	.byte	0x03, 0x04, 0x00, 0x00
	.byte	0x04, 0x0A
	.ascii	"CDJ3000X"
head_end:
	.p2align 2
p_state:	.long 0x0BD4B148
p_auth:		.long 0x0BD4B144
p_memset:	.long 0x08533D50
p_send:		.long 0x084F4ECA
p_in_task:	.long 0x08518068
p_lock:		.long 0x0851A580
p_unlock:	.long 0x0851A7C4
p_lock_id:	.long 0x10DE1BE4
p_device:	.long 0x10DDFBA0
p_serial:	.long 0xA07FE000
