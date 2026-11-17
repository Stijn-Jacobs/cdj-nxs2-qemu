! SPDX-License-Identifier: GPL-2.0-or-later
!
! BPM filter for the XDJ-700 MAIN firmware (SH7724, SH-4, little-endian). The
! firmware carries the track filter of the CDJ-2000NXS2 (condition store, row
! predicate, list builders) but no screen or key that drives it. A long press
! of the INFO key, or of the INFO tab on the touch screen, while a list is open
! switches a filter on or off: with it on, a list shows only the tracks within
! 6 % of the deck's current BPM, so a list opened or refreshed afterwards is
! the matching rows only.
!
! Hook: the UI task's call to the key table's initialisation, right after it
! returns, on the patch engine's trampoline. The routine puts info_key in the
! table's INFO slot and keeps the stock handler to call for everything that is
! not a long press.
!
! info_key(ev), the handler: ev[0] = pressed, ev[2] = stage (1 pressed, 2 after
! 166 ticks, 3 after 333). In the BROWSE screen (mode 2) stage 2 switches the
! filter once per press and the release is swallowed, so the stock handler does
! not open the LINK INFO page; the press and a short release still go to the
! stock handler. In any other mode it gets every call.
!
! toggle: the filter of list slot 0 (the one the track lists use) on USB
! (media 3). Off: the switch is cleared. On: the BPM condition is set to
! [BPM - 6 %, BPM + 6 %] with the deck's BPM x 10 as the centre, then the
! switch is set; the server's list refresh is called as the DB server does after
! each of them, and UPDATE_LIST makes the browser read the list again. A deck
! with no track loaded has BPM 0 and is left as it is. The server functions
! are called from the key task, not by message; they only write the condition
! words and the list's re-evaluate flags.
!
! Mod state, written at run time inside this file: the stock INFO handler and
! whether this press has switched the filter.
! r8 and r9 are saved; r0-r7 are not.

	.text
	.global trackfilter
trackfilter:
	mov.l	p_key_table,r1
	mov	#108,r0
	mov.l	@(r0,r1),r2
	mova	info_key,r0
	mov	r0,r3
	cmp/eq	r3,r2
	bt	1f
	mova	stock_info,r0
	mov.l	r2,@r0
	mov	#108,r0
	mov.l	r3,@(r0,r1)
1:	rts
	nop

	.p2align 2
info_key:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov	r4,r8
	mov.l	p_browse_mode,r1
	jsr	@r1
	nop
	cmp/eq	#2,r0
	bf	stock
	mov.l	@(8,r8),r0
	cmp/eq	#1,r0
	bt	stock
	cmp/pl	r0
	bf	stock
	mov.l	@r8,r1
	tst	r1,r1
	bf	held
	mova	fired,r0
	mov	#0,r1
	mov.l	r1,@r0
	bra	out
	mov.l	r1,@(8,r8)
held:	mova	fired,r0
	mov.l	@r0,r1
	tst	r1,r1
	bf	done
	mov	#1,r1
	mov.l	r1,@r0
	bsr	toggle
	nop
done:	bra	out
	mov	#0,r0
stock:	mova	stock_info,r0
	mov.l	@r0,r1
	jsr	@r1
	mov	r8,r4
out:	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

toggle:
	sts.l	pr,@-r15
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	add	#-16,r15
	mov.l	p_filter_on,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	on
	mov.l	p_set_onoff,r1
	mov	#3,r4
	mov	#0,r5
	jsr	@r1
	mov	#0,r6
	bra	redraw
	nop
on:	mov.l	p_deck_bpm,r1
	mov.l	@r1,r1
	tst	r1,r1
	bt	end
	mov	r1,r3
	shll2	r3
	add	r1,r3
	shll	r3
	mov	#6,r2
	mul.l	r2,r1
	sts	macl,r1
	mov.l	p_div10,r2
	dmulu.l	r1,r2
	sts	mach,r1
	shlr2	r1
	shlr	r1
	mov	r15,r8
	mov.l	p_condition,r0
	mov.l	r0,@r8
	mov	r3,r0
	sub	r1,r0
	mov.l	r0,@(4,r8)
	add	r1,r3
	mov.l	r3,@(8,r8)
	mov	r8,r0
	add	#12,r0
	mov.l	r0,@-r15
	mov.l	r8,@-r15
	mov	#3,r9
	shll8	r9
	mov	r9,r4
	mov	#6,r5
	mov	#0,r6
	mov.l	p_set_condition,r1
	jsr	@r1
	mov	#12,r7
	add	#8,r15
	mov.l	p_list_ctx,r4
	mov.l	p_refresh_a,r1
	jsr	@r1
	mov	r9,r5
	mov	#3,r4
	mov	#0,r5
	mov.l	p_set_onoff,r1
	jsr	@r1
	mov	#1,r6
	mov.l	p_list_ctx,r4
	mov.l	p_refresh_b,r1
	jsr	@r1
	mov	r9,r5
redraw:	mov.l	p_update_list,r1
	jsr	@r1
	nop
end:	add	#16,r15
	mov.l	@r15+,r9
	mov.l	@r15+,r8
	lds.l	@r15+,pr
	rts
	nop

	.p2align 2
p_key_table:	.long 0x0BA39730	! 16 bytes a key, the handler at +12
p_browse_mode:	.long 0x08DB0DF4
p_update_list:	.long 0x08DAB83A
p_set_onoff:	.long 0x08B34CC0
p_set_condition: .long 0x08B34D5A
p_refresh_a:	.long 0x08CE4FD0
p_refresh_b:	.long 0x08CE5068
p_list_ctx:	.long 0x0B0E0E98
p_filter_on:	.long 0x09603674 + 0xE8	! the switch of list slot 0
p_deck_bpm:	.long 0x0C0E6B6C + 8	! the deck's BPM x 10
p_div10:	.long 0xCCCCCCCD
p_condition:	.long 0x00020601	! enabled, kind 6 (inside), two values
stock_info:	.long 0
fired:		.long 0
