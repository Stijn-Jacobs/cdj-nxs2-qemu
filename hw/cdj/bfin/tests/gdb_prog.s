/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test program for gdb_test.c: assembled by `make tests/gdb_prog.h`. */
.text
.global start
start:
	P0.L = 0x2000;          /* 32-bit */
	P0.H = 0;
.global l_a
l_a:
	R0 = 5 (X);             /* 16-bit */
	R1 = 7 (X);
.global l_b
l_b:
	R2 = R0 + R1;           /* 32-bit */
.global l_store
l_store:
	[P0] = R2;
.global l_after
l_after:
	R3 = [P0];
.global l_bundle
l_bundle:
	R6 = R0 + R1 (NS) || [P0] = R2 || NOP;   /* 64-bit bundle, one store */
.global l_end
l_end:
	NOP;
.global l_spin
l_spin:
	JUMP.S l_spin;
