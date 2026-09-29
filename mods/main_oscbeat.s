! SPDX-License-Identifier: GPL-2.0-or-later
!
! On-deck beat output for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian): every time MAIN sends its Pro DJ Link beat packet, send one
! more small datagram, an OSC message, to UDP port 50010 on the subnet
! broadcast address. A laptop-less rig (a lighting desk, a drum machine, a
! script on any host) can then follow the deck's beat without decoding Pro DJ
! Link. 50010 sits next to Pro DJ Link's 50000-50002 but is not one of them.
!
!     /cdj/beat  ,iiii  player  beat_in_bar  bpm_x100  pitch
!
! Four OSC int32 arguments, copied byte for byte from the fields of the beat
! packet MAIN has just built (offsets into the 0x60-byte packet):
!
!     player       +0x5F  device number
!     beat_in_bar  +0x5C  1..4
!     bpm_x100     +0x5A  16-bit, the packet's own BPM x 100
!     pitch        +0x54  32-bit, 0x100000 = 0 percent
!
! Nothing is recomputed. The beat packet carries no running beat count, so
! there is none here.
!
! Hook: the beat loop's call to the UDP send wrapper, a hook on the patch
! engine's trampoline (see sigpatch.trampoline()), so it runs right after
! the stock send, with r5 = the beat function's frame, r12 = the send
! wrapper and r10 = the fpscr mask the firmware applies around every call.
! The beat packet is *(frame+0x218) and the stock send descriptor is at
! frame+0x150. The OSC message goes through the same wrapper with a second
! descriptor: the stock one copied, with its length and port replaced. The
! descriptor is not modified in place, so the next stock send still finds it
! as MAIN left it. Buffers are on the stack (the wrapper copies the payload
! before it returns); r12 and r10 are only read.

	.text
	.global oscbeat
oscbeat:
	sts.l	pr,@-r15
	add	#-56,r15		! 36 bytes of message, 20 of descriptor

	mova	message,r0
	mov	r0,r6
	mov	r15,r7
	mov	#9,r3
1:	mov.l	@r6+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r3
	bf	1b			! r7 = r15 + 36, the descriptor copy

	mov.w	o_desc,r0
	mov	r5,r6
	add	r0,r6			! r6 = the stock descriptor
	mov.w	o_packet,r0
	mov.l	@(r0,r5),r4		! the beat packet
	mov	r15,r5
	add	#20,r5			! r5 = the four arguments
	mov	#0x5F,r0
	mov.b	@(r0,r4),r0
	mov.b	r0,@(3,r5)		! player, low byte of a big-endian int32
	mov	#0x5C,r0
	mov.b	@(r0,r4),r0
	mov.b	r0,@(7,r5)		! beat_in_bar
	mov	#0x5A,r0
	mov.b	@(r0,r4),r0
	mov.b	r0,@(10,r5)		! bpm_x100
	mov	#0x5B,r0
	mov.b	@(r0,r4),r0
	mov.b	r0,@(11,r5)
	add	#0x54,r4
	mov.b	@r4+,r0
	mov.b	r0,@(12,r5)		! pitch
	mov.b	@r4+,r0
	mov.b	r0,@(13,r5)
	mov.b	@r4+,r0
	mov.b	r0,@(14,r5)
	mov.b	@r4+,r0
	mov.b	r0,@(15,r5)

	mov	#5,r3
2:	mov.l	@r6+,r1
	mov.l	r1,@r7
	add	#4,r7
	dt	r3
	bf	2b

	mov	r15,r5
	add	#36,r5			! r5 = the descriptor copy
	mov	#36,r1
	mov.l	r1,@r5			! length
	mov.l	p_port,r1
	mov.l	r1,@(16,r5)		! port
	mov	r15,r4			! r4 = the message

	sts	fpscr,r2
	and	r10,r2
	jsr	@r12
	lds	r2,fpscr

	add	#56,r15
	lds.l	@r15+,pr
	rts
	nop

o_desc:		.word 0x150
o_packet:	.word 0x218
	.p2align 2
p_port:		.long 50010

! "/cdj/beat" and ",iiii", each NUL-padded to a multiple of four, then the
! four zeroed arguments the code above fills in.
message:
	.ascii	"/cdj/beat"
	.byte	0,0,0
	.ascii	",iiii"
	.byte	0,0,0
	.long	0,0,0,0
