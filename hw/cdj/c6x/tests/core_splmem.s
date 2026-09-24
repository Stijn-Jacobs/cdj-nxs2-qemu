; Stores and loads to one word in the same cycle from overlapping SPLOOP
; iterations (ii 1, body 6 cycles). Iteration k loads *a8 at body offset 0,
; iteration k-1 stores its counter there at offset 1 in that same cycle; the
; load must see the word from before that store (SPRU732J 4.2.3). Offset 5
; copies the loaded value out, once the load has landed. core_test.c expects
; out = init, init, 0x100, 0x101, ... rather than init, 0x100, 0x101, ...
        .text
        .global _start
_start:
        mvkl    .S1 0x00810000, a8
        mvkh    .S1 0x00810000, a8
        mvkl    .S2 0x00810100, b0
        mvkh    .S2 0x00810100, b0
        mvk     .S1 0x100, a1
        mvk     .S2 8, b1
        mvc     .S2 b1, ilc
        nop     5
        sploop  1
        ldw     .D1T1 *a8, a2
        stw     .D1T1 a1, *a8
||      add     .L1 1, a1, a1
        nop
        nop
        nop
        spkernel 0, 0
||      stw     .D2T1 a2, *b0++
        nop     5
        nop     5
done:
        idle
        b       .S1 done
        nop     5
