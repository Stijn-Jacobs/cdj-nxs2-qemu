! SPDX-License-Identifier: GPL-2.0-or-later
!
! Tempo WIDE shortcut for CDJ-850 MAIN 1.13 (SH7263, SH-2A, big-endian).
!
! Hooked at 0x0C0D8DC2..0x0C0D8DCE right after movu.b @(0,r5), r6 reads the
! current tempo range into r6 (1=±6%, 2=±10%, 3=±16%, 4=WIDE).
!
! The trampoline saves r0-r7 onto the stack and passes saved_regs pointer in r4
! (r6 is at @(24, r4)). When r6 == 2 (±10%), rewrite saved r6 to 3 (±16%) so
! the following USB mode step transitions 3 -> 4 (WIDE) on a single TEMPO tap.

	.text
tempowide:
	mov	#0,r0
	mov.l	r0,@r4
	mov.l	@(24,r4),r0
	cmp/eq	#2,r0
	bf	done
	mov	#3,r0
	mov.l	r0,@(24,r4)
done:
	rts
	nop
