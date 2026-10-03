! SPDX-License-Identifier: GPL-2.0-or-later
!
! TCNet node announcement for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4,
! little-endian), the 1 Hz half of the tcnet mod (the Time packets are
! main_tcnettime.s). Once per second of deck clock the deck broadcasts, to UDP
! port 60000 on the subnet broadcast address:
!
!     Opt-IN (message type 2, 68 bytes), which puts the deck in a receiver's
!     node list: node count 0, listener port 65023, uptime = the deck clock
!     in seconds modulo 12 hours, vendor "cdj-nxs2-qemu", device
!     "CDJ-2000NXS2", version 1.87.0.
!
!     Status (message type 5, 300 bytes), the layer list. Layer p (the
!     player, 1..4) carries, every other layer is zero:
!
!         layer source   @34+(p-1)     p
!         layer status   @42+(p-1)     status record +0x78, the play-state
!                                      code, through layer_state
!         track id       @50+4(p-1)    status record +0x2C, the rekordbox id
!         layer name     @172+16(p-1)  "DECK p"
!
! The node count is 0 because this half only sends: it keeps no list of the
! nodes it would hear. The listener port is the spec's default; nothing on the
! deck binds it.
!
! Hook: the Sentinel task's call to the status serialiser, on the patch
! engine's trampoline, so it runs right after the serialiser returns with
! r11 = the deck's own status record. The routine touches r0-r7 and r8, r9,
! r13 (saved); r11 is only read.
!
! Mod state, written at run time inside this file: the deck clock of the last
! announcement and whether there has been one.

	.text
	.global tcnet
tcnet:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r13,@-r15
	mov.l	p_device,r1
	mov.b	@r1,r0
	extu.b	r0,r13			! the player
	mov	r13,r0
	add	#-1,r0
	mov	#3,r1
	cmp/hi	r1,r0
	bt	out

	mov.l	p_tick,r1
	mov.l	@r1,r9
	mova	slow,r0
	mov	r0,r8
	mov.l	@(4,r8),r2
	tst	r2,r2
	bt	1f
	mov.l	@r8,r2
	mov	r9,r3
	sub	r2,r3
	mov.w	c_1000,r2
	cmp/hs	r2,r3
	bf	out
1:	bsr	address
	nop
	tst	r0,r0
	bt	out
	mov.l	r9,@r8
	mov	#1,r1
	mov.l	r1,@(4,r8)
	mov	r0,r9			! the deck's address

	mova	optin,r0
	mov	r0,r4
	mov	r9,r6
	mov	r13,r7
	bsr	optin_fill
	add	#0x30,r7
	mov	#68,r5
	mov.l	p_port,r6
	bsr	send
	nop

	mova	status,r0
	mov	r0,r4
	mov	r9,r6
	mov	r13,r7
	bsr	header
	add	#0x30,r7
	mova	status,r0
	mov	r0,r8
	mov	r8,r1
	add	#34,r1
	mov	#24,r2
	mov	#0,r3
1:	mov.w	r3,@r1			! every layer's source, status and track id
	add	#2,r1
	dt	r2
	bf	1b
	mov.w	o_names,r1
	add	r8,r1
	mov	#32,r2
1:	mov.l	r3,@r1			! every layer's name
	add	#4,r1
	dt	r2
	bf	1b

	mov	r13,r5
	add	#-1,r5			! the layer's index
	mov	r8,r1
	add	r5,r1
	add	#34,r1
	mov.b	r13,@r1
	add	#8,r1
	mov	#0x78,r0
	mov.b	@(r0,r11),r0
	bsr	layer_state
	extu.b	r0,r0
	mov.b	r0,@r1
	mov	r5,r1
	shll2	r1
	add	r8,r1
	add	#50,r1
	mov	#0x2C,r0
	mov.l	@(r0,r11),r2
	mov	r2,r0
	mov.w	r0,@(0,r1)
	shlr16	r2
	mov	r2,r0
	mov.w	r0,@(2,r1)
	mov	r5,r1
	shll2	r1
	shll2	r1
	add	r8,r1
	mov.w	o_names,r0
	add	r0,r1
	mov.l	c_deck,r2
	mov.l	r2,@r1
	mov	#0x20,r0
	mov.b	r0,@(4,r1)
	mov	r13,r0
	add	#0x30,r0
	mov.b	r0,@(5,r1)
	mov	r8,r4
	mov.w	c_length,r5
	mov.l	p_port,r6
	bsr	send
	nop

out:
	mov.l	@r15+,r13
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

o_names:	.word 172
c_length:	.word 300
	.p2align 2
p_port:		.long 60000
c_deck:		.ascii "DECK"
slow:
	.long	0			! deck clock at the last announcement
	.long	0			! 1 once there has been one

	.include "main_tcnet_shared.inc"

	.p2align 2
optin:
	tcn_optin

	.p2align 2
status:
	tcn_header 5
	.short	0			! node count
	.short	65023			! listener port
	.space	300 - 28
