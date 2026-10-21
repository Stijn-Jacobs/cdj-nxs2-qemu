/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test program for trace_test.c: assembled by `make tests/trace_prog.h`. */
.text
.global start
start:
	SP.L = 0x8000;
	SP.H = 0;
	R0 = 1 (X);
	R1 = 2 (X);
	R2 = 3 (X);
.global call1
call1:
	CALL.X f;
	R0 = 10 (X);
	R1 = 20 (X);
.global call2
call2:
	CALL.X g;
	R0 = 7 (X);
	P1 = 3 (X);
	LSETUP (lp_top, lp_bot) LC0 = P1;
lp_top:
	R0 += 1;
lp_bot:
	R1 += 1;
.global done
done:
	JUMP.S done;
.global f
f:
	RTS;
.global g
g:
	R0 += 5;
	RTS;
