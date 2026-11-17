! SPDX-License-Identifier: GPL-2.0-or-later
!
! TCNet data for the CDJ-2000NXS2 MAIN firmware (SH7724, SH-4, little-endian),
! the listener half of the tcnet mod (Opt-IN and Status are main_tcnet.s, Time
! is main_tcnettime.s). It opens the node's listener port, UDP 65023, as a
! second socket of the firmware's own UDP layer (udp_vcre_cep with the same
! T_UDP_CCEP shape as the Pro DJ Link socket's in ROM, port 0xC350) and polls
! it (udp_rcv_dat with TMO_POL) for up to four datagrams a pass:
!
!     Opt-IN (type 2)     the sender joins the node list at its address and
!                         the listener port it advertises @26
!     Application data    data identifiers @24 0xFF, 0xFF: the sender joins
!       (type 30)         the node list at its address and the listener port
!                         @46 (the second field of the 20 data bytes Resolume
!                         Arena unicasts every few seconds; its source port
!                         is not its listener)
!     Request (type 20)   the sender joins the node list at the endpoint it
!                         sent from, unless its address is there already,
!                         and gets the Data packet it asks for: Metrics (data
!                         type 2) or MetaData (4) for layer p; any other data
!                         type or layer gets an Error packet (type 13) with
!                         code 14, empty
!
! The node list holds four addresses, a new one replacing the oldest. A node
! added or moved to another port by an Opt-IN or application packet gets
! MetaData and Metrics at once. Every node gets the deck's Opt-IN and MetaData
! once a second, Metrics every 50 ms while the layer is playing or looping and
! at once when its state, speed or BPM changes, and MetaData when the status
! record's rekordbox id changes. Every packet goes out with udp_snd_dat from
! the listener socket, so its source port is 65023.
!
! Metrics (message type 200, data type 2, 122 bytes), layer p; status record
! R = 0x0A371D3C + p * 0x124:
!
!     layer id        @25   p
!     layer state     @27   R+0x78, the play-state code, through layer_state
!     sync master     @29   R+0x89 bit 0x20, the tempo master flag
!     beat marker     @31   R+0xA6, the beat in the bar 1..4, 0 otherwise
!     track length    @32   0x0B5125E8, the length in seconds, x 1000
!     track position  @36   0x09947488, the play position in ms
!     speed           @40   R+0x8C, the pitch (0x100000 = 100 %) >> 5, so
!                           32768 = 100 %
!     beat number     @57   R+0xA0, 0 when negative
!     BPM x 100       @112  R+0x90 low 16 bits when its bit 31 is set, else
!                           0 (the track's BPM), x the pitch / 0x100000
!     track id        @118  R+0x2C, the rekordbox id
!
! The pitch bend (@116) stays 0: no bend word of the deck is known.
!
! MetaData (message type 200, data type 4, 548 bytes), layer p: artist @29
! and title @285, 256 bytes of UTF-16LE each, widened from the loaded track's
! ASCII strings at 0x0994E040 and 0x0994DE40 (two fields of the DJcont record
! 0x0994DC28, NUL-terminated in 0x200-byte slots); key @541 stays 0; track id
! @543 = R+0x2C.
!
! Hook: the DJcont output task's loop (DJCONT_PASS, as main_tcnettime.s). The
! routine touches r0-r7 and r8-r14 (saved). Mod state, written at run time
! inside this file: the socket id, the node list and the values of the last
! Metrics push.

	.equ	D_CRE, 4		! the firmware functions, after the
	.equ	D_RCV, 8		! fpscr mask at data + 0
	.equ	D_SND, 12
	.equ	D_TCN, 16
	.equ	D_CEP, 20
	.equ	D_TRY, 24
	.equ	D_OPTIN, 28
	.equ	D_PUSH, 32
	.equ	D_STATE, 36
	.equ	D_BPM, 40
	.equ	D_SPEED, 44
	.equ	D_TRACK, 48
	.equ	D_NEXT, 52
	.equ	D_FROM, 56
	.equ	D_CCEP, 64
	.equ	D_NODES, 80
	.equ	D_RX, 112

! Calls the firmware function at data + \fn with the fpscr mask the firmware
! applies before every call. Clobbers r1-r3.
	.macro	fw fn
	mov.l	@(\fn,r8),r1
	mov.l	@r8,r2
	sts	fpscr,r3
	and	r2,r3
	jsr	@r1
	lds	r3,fpscr
	.endm

	.text
	.global tcnetdata
tcnetdata:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.l	k_device,r1
	mov.b	@r1,r0
	extu.b	r0,r13			! the player
	mov	r13,r0
	add	#-1,r0
	mov	#3,r1
	cmp/hi	r1,r0
	bt	out
	mov.w	k_record,r1
	mul.l	r13,r1
	sts	macl,r9
	mov.l	k_records,r1
	add	r1,r9			! the deck's own status record
	mova	pool,r0
	mov.l	@r0,r1
	add	r1,r0
	mov	r0,r8
	bsr	address
	nop
	tst	r0,r0
	bt	out
	mov	r0,r12			! the deck's address
	mov.l	k_tick,r1
	mov.l	@r1,r11			! the deck clock

	mov.l	@(D_CEP,r8),r0
	cmp/pl	r0
	bt	1f
	mov.l	@(D_TRY,r8),r1
	mov	r11,r2
	sub	r1,r2
	mov.w	k_1000,r1
	cmp/hs	r1,r2
	bf	out
	mov.l	r11,@(D_TRY,r8)
	mov	r8,r4
	add	#D_CCEP,r4
	fw	D_CRE
	mov.l	r0,@(D_CEP,r8)
	cmp/pl	r0
	bf	out
1:	bsr	metrics
	nop

	mov	#4,r10
1:	mov	#0,r1
	mov.l	r1,@-r15		! TMO_POL
	mov.l	@(D_CEP,r8),r4
	mov	r8,r5
	add	#D_FROM,r5
	mov	r8,r6
	add	#D_RX,r6
	mov	#64,r7
	fw	D_RCV
	add	#4,r15
	cmp/pl	r0
	bf	1f
	bsr	incoming
	nop
	dt	r10
	bf	1b

1:	mov.l	@(D_OPTIN,r8),r1
	mov	r11,r2
	sub	r1,r2
	mov.w	k_1000,r1
	cmp/hs	r1,r2
	bf	1f
	mov.l	r11,@(D_OPTIN,r8)
	mov.w	k_optin,r4
	add	r8,r4
	mov	r12,r6
	mov	r13,r7
	bsr	optin_fill
	add	#0x30,r7
	bsr	each
	mov	#68,r5
	bsr	metadata
	nop
	mov.w	k_548,r5
	bsr	each
	nop

1:	mov.w	k_metrics,r14
	add	r8,r14
	mov	#27,r0
	mov.b	@(r0,r14),r0
	extu.b	r0,r3			! layer state
	mov.l	@(40,r14),r4		! speed
	mov	#112,r0
	mov.l	@(r0,r14),r5		! BPM
	mov.l	@(D_STATE,r8),r1
	cmp/eq	r1,r3
	bf	2f
	mov.l	@(D_SPEED,r8),r1
	cmp/eq	r1,r4
	bf	2f
	mov.l	@(D_BPM,r8),r1
	cmp/eq	r1,r5
	bf	2f
	mov	r3,r0
	add	#-3,r0
	mov	#1,r1
	cmp/hi	r1,r0
	bt	1f
	mov.l	@(D_PUSH,r8),r1
	mov	r11,r2
	sub	r1,r2
	mov	#50,r1
	cmp/hs	r1,r2
	bf	1f
2:	mov.l	r3,@(D_STATE,r8)
	mov.l	r4,@(D_SPEED,r8)
	mov.l	r5,@(D_BPM,r8)
	mov.l	r11,@(D_PUSH,r8)
	bsr	stamp
	mov	r14,r4
	bsr	each
	mov	#122,r5

1:	mov	#0x2C,r0
	mov.l	@(r0,r9),r1
	mov.l	@(D_TRACK,r8),r2
	cmp/eq	r1,r2
	bt	out
	mov.l	r1,@(D_TRACK,r8)
	bsr	metadata
	nop
	mov.w	k_548,r5
	bsr	each
	nop

out:
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

k_record:	.word 0x124
k_1000:		.word 1000
k_548:		.word 548
k_optin:	.word optin - data
k_metrics:	.word metrics_pkt - data
	.p2align 2
pool:		.long data - pool
k_device:	.long 0x0AB84CFA
k_records:	.long 0x0A371D3C
k_tick:		.long 0x0B0C5258

! Handles the datagram of r0 bytes in the receive buffer, sent from the
! endpoint at data + D_FROM. Clobbers r0-r7 and r14.
incoming:
	sts.l	pr,@-r15
	mov	r0,r1
	mov	r8,r14
	add	#D_RX,r14
	mov	#24,r2
	cmp/ge	r2,r1
	bf	9f
	mov.l	@(4,r14),r2
	mov	r2,r3
	shll8	r2
	mov.l	@(D_TCN,r8),r0
	cmp/eq	r0,r2
	bf	9f
	shlr16	r3
	shlr8	r3
	mov	r3,r0
	cmp/eq	#2,r0
	bf	1f
	mov	#28,r2
	cmp/ge	r2,r1
	bf	9f
	mov.w	@(26,r14),r0
	bra	4f
	extu.w	r0,r5
1:	cmp/eq	#30,r0
	bf	5f
	mov	#48,r2
	cmp/ge	r2,r1
	bf	9f
	mov.w	@(24,r14),r0
	extu.w	r0,r0
	mov	#-1,r2
	extu.w	r2,r2
	cmp/eq	r2,r0
	bf	9f
	mov	#46,r0
	mov.w	@(r0,r14),r0
	extu.w	r0,r5
4:	mov.l	@(D_FROM,r8),r4
	bsr	remember
	mov	#1,r6
	tst	r0,r0
	bt	9f
	mov	r0,r14
	bsr	metadata
	nop
	mov.w	i_548,r5
	bsr	snd
	mov	r14,r6
	mov.w	i_metrics,r4
	add	r8,r4
	bsr	stamp
	nop
	mov	#122,r5
	bsr	snd
	mov	r14,r6
	bra	9f
	nop
5:	cmp/eq	#20,r0
	bf	9f
	mov	#26,r2
	cmp/ge	r2,r1
	bf	9f
	mov	r8,r1
	add	#D_FROM,r1
	mov.l	@r1,r4
	mov.w	@(4,r1),r0
	extu.w	r0,r5
	bsr	remember
	mov	#0,r6
	mov	#25,r0
	mov.b	@(r0,r14),r0
	extu.b	r0,r0
	cmp/eq	r13,r0
	bf	2f
	mov	#24,r0
	mov.b	@(r0,r14),r0
	cmp/eq	#2,r0
	bf	1f
	mov.w	i_metrics,r4
	add	r8,r4
	bsr	stamp
	nop
	bra	3f
	mov	#122,r5
1:	cmp/eq	#4,r0
	bf	2f
	bsr	metadata
	nop
	mov.w	i_548,r5
	bra	3f
	nop
2:	mov.w	i_error,r4
	add	r8,r4
	mov	#24,r0
	mov.b	@(r0,r14),r1
	mov.b	r1,@(r0,r4)
	mov	#25,r0
	mov.b	@(r0,r14),r1
	mov.b	r1,@(r0,r4)
	mov	#14,r0			! empty
	mov.w	r0,@(26,r4)
	mov	#20,r0
	mov.w	r0,@(28,r4)
	bsr	stamp
	nop
	mov	#30,r5
3:	mov	r8,r6
	bsr	snd
	add	#D_FROM,r6
9:	lds.l	@r15+,pr
	rts
	nop

i_548:		.word 548
i_metrics:	.word metrics_pkt - data
i_error:	.word error - data

! Adds the endpoint r4 (IPv4 address), r5 (port) to the node list in place
! of the oldest entry, unless the address is there already; a known address
! takes the port r5 when r6 is not 0. Returns the entry in r0 when it was
! added or its port changed, else 0. Clobbers r0-r3.
remember:
	tst	r4,r4
	bt	8f
	tst	r5,r5
	bt	8f
	mov	r8,r1
	add	#D_NODES,r1
	mov	#4,r2
1:	mov.l	@r1,r3
	cmp/eq	r4,r3
	bt	2f
	add	#8,r1
	dt	r2
	bf	1b
	mov.l	@(D_NEXT,r8),r1
	mov	r1,r3
	add	#1,r3
	mov	#3,r2
	and	r2,r3
	mov.l	r3,@(D_NEXT,r8)
	shll2	r1
	shll	r1
	add	#D_NODES,r1
	add	r8,r1
	bra	3f
	mov.l	r4,@r1
2:	tst	r6,r6
	bt	8f
	mov.w	@(4,r1),r0
	extu.w	r0,r0
	cmp/eq	r5,r0
	bt	8f
3:	mov	r5,r0
	mov.w	r0,@(4,r1)
	rts
	mov	r1,r0
8:	rts
	mov	#0,r0

! Sends r5 bytes at r4 to every endpoint in the node list. Clobbers r0-r7,
! r10 and r14.
each:
	sts.l	pr,@-r15
	mov.l	r5,@-r15
	mov	r4,r14
	mov	r8,r10
	add	#D_NODES,r10
1:	mov.l	@r10,r0
	tst	r0,r0
	bt	2f
	mov	r14,r4
	mov.l	@r15,r5
	bsr	snd
	mov	r10,r6
2:	add	#8,r10
	mov	r8,r1
	add	#D_NODES+32,r1
	cmp/eq	r1,r10
	bf	1b
	add	#4,r15
	lds.l	@r15+,pr
	rts
	nop

! udp_snd_dat(listener socket, the T_IPV4EP at r6, r4, r5 bytes, TMO_FEVR).
! Clobbers r0-r7.
snd:
	sts.l	pr,@-r15
	mov	#-1,r1
	mov.l	r1,@-r15
	mov	r5,r7
	mov	r6,r5
	mov	r4,r6
	mov.l	@(D_CEP,r8),r4
	fw	D_SND
	add	#4,r15
	lds.l	@r15+,pr
	rts
	nop

! The header of the packet at r4 (see header), for this deck.
stamp:
	mov	r12,r6
	mov	r13,r7
	bra	header
	add	#0x30,r7

! Stores r2 at r1, a byte at a time. Clobbers r0.
put32b:
	mov	r2,r0
	mov.b	r0,@r1
	shlr8	r0
	mov.b	r0,@(1,r1)
	shlr8	r0
	mov.b	r0,@(2,r1)
	shlr8	r0
	rts
	mov.b	r0,@(3,r1)

! Fills the Metrics packet's layer fields from the status record r9 and the
! deck's words. Clobbers r0-r7.
metrics:
	sts.l	pr,@-r15
	mov.w	m_metrics,r4
	add	r8,r4
	mov	r9,r5
	add	#0x78,r5		! R+0x78, so the fields below are R+0x78+d
	mov	#25,r0
	mov.b	r13,@(r0,r4)
	mov.b	@r5,r0
	bsr	layer_state
	extu.b	r0,r0
	mov	r0,r1
	mov	#27,r0
	mov.b	r1,@(r0,r4)
	mov	#0x89-0x78,r0
	mov.b	@(r0,r5),r0
	and	#0x20,r0
	shlr2	r0
	shlr2	r0
	shlr	r0
	mov	r0,r1
	mov	#29,r0
	mov.b	r1,@(r0,r4)
	mov	#0xA6-0x78,r0
	mov.b	@(r0,r5),r0
	extu.b	r0,r0
	mov	#4,r1
	cmp/hi	r1,r0
	bf	1f
	mov	#0,r0
1:	mov	r0,r1
	mov	#31,r0
	mov.b	r1,@(r0,r4)
	mov.l	m_length,r1
	mov.l	@r1,r1
	mov.w	m_1000,r2
	mul.l	r2,r1
	sts	macl,r1
	mov.l	r1,@(32,r4)
	mov.l	m_position,r1
	mov.l	@r1,r1
	mov.l	r1,@(36,r4)
	mov.l	@(0x8C-0x78,r5),r6	! pitch
	mov	r6,r1
	shlr2	r1
	shlr2	r1
	shlr	r1
	mov.l	r1,@(40,r4)
	mov.l	@(0xA0-0x78,r5),r2
	cmp/pz	r2
	bt	1f
	mov	#0,r2
1:	mov	r4,r1
	bsr	put32b
	add	#57,r1
	mov.l	@(0x90-0x78,r5),r7
	cmp/pz	r7
	bf/s	1f
	extu.w	r7,r7
	mov	#0,r7
1:	dmulu.l	r6,r7
	sts	mach,r1
	shll8	r1
	shll2	r1
	shll2	r1
	sts	macl,r2
	shlr16	r2
	shlr2	r2
	shlr2	r2
	or	r2,r1
	mov	#112,r0
	mov.l	r1,@(r0,r4)
	mov	#0x2C,r0
	mov.l	@(r0,r9),r2
	mov	r4,r1
	bsr	put32b
	add	#118,r1
	lds.l	@r15+,pr
	rts
	nop

m_1000:		.word 1000
m_metrics:	.word metrics_pkt - data
	.p2align 2
m_length:	.long 0x0B5125E8
m_position:	.long 0x09947488

! Fills and stamps the MetaData packet; r4 = the packet. Clobbers r0-r3,
! r5-r7.
metadata:
	sts.l	pr,@-r15
	mov.w	d_meta,r4
	add	r8,r4
	mov	#25,r0
	mov.b	r13,@(r0,r4)
	mov	#0x2C,r0
	mov.l	@(r0,r9),r2
	mov.w	d_track,r1
	bsr	put32b
	add	r4,r1
	mov.l	p_artist,r6
	mov	r4,r5
	bsr	text
	add	#29,r5
	mov.l	p_title,r6
	mov.w	d_title,r5
	bsr	text
	add	r4,r5
	bsr	stamp
	nop
	lds.l	@r15+,pr
	rts
	nop

! Widens the NUL-terminated ASCII string at r6 to UTF-16LE in the 256-byte
! field at r5, cut at 127 characters and padded with 0. Clobbers r0-r2, r5,
! r6.
text:
	mov	#64,r2
	shll	r2
1:	mov	#0,r0
	tst	r6,r6
	bt	2f
	mov.b	@r6+,r0
	extu.b	r0,r0
	tst	r0,r0
	bf	2f
	mov	#0,r6
2:	mov	#1,r1
	cmp/eq	r1,r2
	bf	3f
	mov	#0,r0
3:	mov.b	r0,@r5
	mov	#0,r0
	mov.b	r0,@(1,r5)
	add	#2,r5
	dt	r2
	bf	1b
	rts
	nop

d_meta:		.word metadata_pkt - data
d_track:	.word 543
d_title:	.word 285
	.p2align 2
p_title:	.long 0x0994DE40
p_artist:	.long 0x0994E040

	.include "main_tcnet_shared.inc"

	.p2align 2
data:
	.long	0xFFE7FFFF		! fpscr mask
	.long	0x08233A22		! udp_vcre_cep
	.long	0x0823415E		! udp_rcv_dat
	.long	0x08233CF6		! udp_snd_dat
	.long	0x4E435400		! "TCN", a header's word @4 shifted left 8
	.long	0			! the listener socket, 0 before it exists
	.long	0			! deck clock at the last attempt to open it
	.long	0			! deck clock at the last Opt-IN round
	.long	0			! deck clock at the last Metrics push
	.long	-1			! its layer state
	.long	0			! its BPM
	.long	0			! its speed
	.long	0			! rekordbox id of the last MetaData push
	.long	0			! the node list's oldest entry
	.space	8			! the sender, a T_IPV4EP
	.long	0, 0			! T_UDP_CCEP: attribute, any address,
	.short	65023, 0		! the listener port,
	.long	0			! no callback
	.space	32			! the node list, four T_IPV4EP
	.space	64			! the receive buffer

	.p2align 2
optin:
	tcn_optin

	.p2align 2
error:
	tcn_header 13
	.space	30 - 24

	.p2align 2
metrics_pkt:
	tcn_header 200
	.byte	2
	.space	122 - 25

	.p2align 2
metadata_pkt:
	tcn_header 200
	.byte	4
	.space	548 - 25
	.p2align 2
