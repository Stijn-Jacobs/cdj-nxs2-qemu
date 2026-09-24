; SPLOOP interrupted mid-loop (SPRU732 7.13): core_test.c pulses INT4 while the
; 40-iteration copy loop runs. Every word must be copied exactly once, the ISR
; must run once, and it must see ILC holding the iterations still to run.
        .text
        .global _start
_start:
        mvkl    .S2 0x00800400, b0
        mvkh    .S2 0x00800400, b0
        mvc     .S2 b0, istp
        mvk     .S2 0x13, b1             ; NMIE + INT4
        mvc     .S2 b1, ier
        mvc     .S2 csr, b2
        or      .L2 1, b2, b2
        mvc     .S2 b2, csr              ; GIE
        zero    .L1 a15
        mvkl    .S1 0x00810000, a1
        mvkh    .S1 0x00810000, a1
        mvkl    .S2 0x00810200, b0
        mvkh    .S2 0x00810200, b0
        mvk     .S2 40, b1
        mvc     .S2 b1, ilc
        nop     3
        sploop  1
        ldw     .D1T1 *a1++, a2
        nop     4
        mv      .L2X a2, b2
        spkernel 6, 0
||      stw     .D2T2 b2, *b0++
        mv      .L1 a1, a9
        mv      .L2 b0, b10
done:
        idle
        b       .S1 done
        nop     5

        .org    0x480                    ; ISTP 0x00800400 + 4 * 0x20
        b       .S1 isr
        nop     5
        nop
        nop

        .org    0x600
isr:
        add     .L1 1, a15, a15
        mvc     .S2 ilc, b13
        b       .S2 irp
        nop     5
        nop
        nop
