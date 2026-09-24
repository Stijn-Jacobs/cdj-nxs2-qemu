; Two writes to one register from one execute packet, at different pipeline
; stages (0x8006CE34: mpysp lands a5 at E4, ldw at E5): each must land at its
; own cycle, so the square is readable for exactly one cycle before the load.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810000, a8
        mvkh    .S1 0x00810000, a8
        mvkl    .S1 0x40400000, a1       ; 3.0f
        mvkh    .S1 0x40400000, a1
        zero    .L1 a5
        nop     5
        mpysp   .M1 a1, a1, a5
||      ldw     .D1T1 *a8, a5
        mv      .L1 a5, a10
        mv      .L1 a5, a11
        mv      .L1 a5, a12
        mv      .L1 a5, a13
        mv      .L1 a5, a14
        mv      .L1 a5, a15
        nop     5
done:
        idle
        b       .S1 done
        nop     5
