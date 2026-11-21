! SPDX-License-Identifier: GPL-2.0-or-later
!
! Tempo WIDE on USB for CDJ-850 MAIN 1.13 (SH7263, SH-2A, big-endian).
!
! Hooked at 0x0C0D8DC2..0x0C0D8DCE in the TEMPO button handler, after r0 holds
! the source flag *0x0CE6D25A (1 = USB) and r6 the current tempo range
! (1=±6%, 2=±10%, 3=±16%, 4=WIDE). The handler then picks the next range:
!
!   r0 == 1 (USB):  1->2, 2->3, 3->1, 4->3       (WIDE never reached)
!   r0 != 1 (CD):   1->2, 2->3, 3->4, 4->1       (±6 -> ±10 -> ±16 -> WIDE)
!
! The trampoline saves r0-r7 onto the stack and passes the saved_regs pointer in
! r4 (r0 at @(0, r4)). Clearing saved r0 sends USB through the CD step table, so
! TEMPO cycles ±6 -> ±10 -> ±16 -> WIDE -> ±6 without skipping a range.

	.text
tempowide:
	mov	#0,r0
	rts
	mov.l	r0,@r4
