; Cross-path stalls (SPRU732J 3.7.4) against the shape at 0x8006CE34/0x8006CE94:
; an E4 mpysp result read through 1X four packets later must not stall, so the
; same packet still sees the square written to A5 before the E5 load replaces
; it. An E1 result read through 2X the next cycle stalls once; a load's data
; read through 2X when it lands does not. core_test.c expects exactly 1 stall.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810000, a8
        mvkh    .S1 0x00810000, a8
        mvkl    .S1 0x40400000, a19      ; 3.0f
        mvkh    .S1 0x40400000, a19
        mvkl    .S2 0x40000000, b20      ; 2.0f
        mvkh    .S2 0x40000000, b20
        zero    .L1 a9
        zero    .L1 a5
        nop     5
        mpysp   .M2X b20, a19, b16       ; E4: 6.0f
||      mpysp   .M1 a19, a19, a5         ; E4: 9.0f
||      ldw     .D1T1 *a8, a5            ; E5: 1.0f
        mv      .L1 a8, a10
        mv      .L1 a8, a11
        mv      .L1 a8, a12
        add     .L1X a9, b16, a4         ; reads B16 as it lands: no stall
||      mv      .S1 a5, a13              ; still the square
        mv      .L1 a5, a14              ; the load
        nop     5

        mvk     .S1 7, a1
        mv      .L1 a1, a2               ; E1
        add     .L2X b1, a2, b2          ; next cycle through 2X: one stall
        ldw     .D1T1 *a8, a7
        nop     4
        add     .L2X b1, a7, b3          ; load data as it lands: no stall
        nop     5
done:
        idle
        b       .S1 done
        nop     5
