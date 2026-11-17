! SPDX-License-Identifier: GPL-2.0-or-later
!
! Ableton Link measurement responder for the CDJ-2000NXS2 MAIN firmware
! (SH7724, SH-4, little-endian). A Link app that sees the deck's ALIVE
! (main_abletonlink.s) measures the deck's clock with PING datagrams to the
! endpoint the ALIVE advertised, UDP 50000, the Pro DJ Link socket; it drops a
! peer that does not answer a PING with a PONG within 50 ms. This routine
! answers them.
!
! Hook: the Pro DJ Link receive task, right after its call that takes a memory
! block for the datagram it has just received into the 128-byte buffer at r10,
! a hook on the patch engine's trampoline (see sigpatch.trampoline()).
! Besides the site's own registers it uses:
!
!     r14         length of the datagram
!     r5          the task's frame: *(r5) is the memory block and the sender's
!                 address, a T_IPV4EP, is at r5+12
!     block r0    the block call's result, 0 = a block was taken
!
! A datagram that starts with the Link header "_link_v" 0x01 is not Pro DJ
! Link traffic and never reaches the Pro DJ Link layer: the block is given
! back and r0 is set to 1, which makes the task's own test loop back to the
! receive. A PING (type 1, a payload of 1 to 87 bytes) is answered first. The
! PONG is built in the receive buffer and sent from the socket the PING
! arrived on, so its source port is 50000, the port the app pings:
!
!     "_link_v" 01  02  "sess" 8 <session id>  "__gt" 8 <ghost time>  <the
!     PING's payload, unchanged>
!
! The session id is the deck's node id (main_link_shared.inc) and the ghost
! time the clock the ALIVE's timeline uses. The send is udp_snd_dat(cep,
! sender, buffer, length, TMO_FEVR); a failed send drops the PONG and the app
! pings again.

	.text
	.global abletonlinkpong
abletonlinkpong:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov	r4,r9			! the saved r0
	mov	r5,r8			! the task's frame

	mov.l	@r10,r1
	mov.l	m_link0,r2
	cmp/eq	r2,r1
	bf	out
	mov.l	@(4,r10),r1
	mov.l	m_link1,r2
	cmp/eq	r2,r1
	bf	out

	mov.b	@(8,r10),r0
	cmp/eq	#1,r0
	bf	swallow
	mov	r14,r2
	add	#-9,r2			! payload length
	cmp/pl	r2
	bf	swallow
	mov	#96,r1
	cmp/hi	r1,r14
	bt	swallow

	mov	r10,r1
	add	r14,r1			! end of the PING
	mov	r1,r3
	add	#32,r3			! end of the PONG
1:	add	#-1,r1
	mov.b	@r1,r0
	mov.b	r0,@-r3
	dt	r2
	bf	1b

	mov	#2,r0
	mov.b	r0,@(8,r10)
	mov	r10,r4
	add	#9,r4
	mova	entries,r0
	mov	r0,r5
	mov	#32,r2
1:	mov.b	@r5+,r0
	mov.b	r0,@r4
	add	#1,r4
	dt	r2
	bf	1b

	mov	r10,r4
	add	#17,r4
	bsr	node_id
	nop
	tst	r0,r0
	bt	swallow
	mov	r10,r4
	add	#33,r4
	bsr	ghost_time
	nop

	mov	#-1,r1
	mov.l	r1,@-r15
	mov.l	p_cep,r1
	mov.l	@r1,r4
	mov	r8,r5
	add	#12,r5
	mov	r10,r6
	mov	r14,r7
	add	#32,r7
	mov.l	p_snd,r1
	jsr	@r1
	nop
	add	#4,r15

swallow:
	mov.l	@r9,r0
	tst	r0,r0
	bf	out
	mov.l	p_pool,r1
	mov.l	@r1,r4
	mov.l	@r8,r5
	mov.l	p_rel,r1
	jsr	@r1
	nop
	mov	#1,r0
	mov.l	r0,@r9

out:
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

	.p2align 2
m_link0:	.long 0x6E696C5F	! "_lin"
m_link1:	.long 0x01765F6B	! "k_v", 1
p_cep:		.long 0x0AB84CE8
p_pool:		.long 0x0AB84CDC
p_snd:		.long 0x08233CF6
p_rel:		.long 0x08518DA8

! The PONG's two entries, their values zeroed for the code above to fill in.
entries:
	.ascii	"sess"
	.byte	0,0,0,8
	.space	8
	.ascii	"__gt"
	.byte	0,0,0,8
	.space	8

	.include "main_link_shared.inc"
