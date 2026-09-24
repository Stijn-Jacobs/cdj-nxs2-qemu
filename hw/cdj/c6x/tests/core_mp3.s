; Instructions the MP3 header search depends on:
; field extracts, a constant-minus-register subtract, and the ring store of
; 0x8006AF6C where the index is recomputed in the same parallel packet that
; stores through the previous one. core_test.c checks the results against
; independent C. Parameters are read from 0x00813000: ring write index, count.
        .text
        .global _start
_start:
        mvkl    .S1 0x12345677, a4
        mvkh    .S1 0x12345677, a4
        extu    .S1 a4, 21, 21, a5
        extu    .S1 a4, 30, 27, a6
        extu    .S1 a4, 25, 30, a7
        extu    .S1 a4, 28, 28, a8
        extu    .S1 a4, 21, 23, a9
        mvk     .S2 5, b5
        sub     .L2 1, b5, b5            ; 1 - b5, as at 0x80074500
        add     .L1X 0, b5, a15

        mvkl    .S1 0x00813000, a0
        mvkh    .S1 0x00813000, a0
        ldw     .D1T1 *+a0(0), a12       ; ring write index
        ldw     .D1T1 *+a0(4), a10       ; count
        mvkl    .S1 0x00810000, a11      ; struct: ring at +276
        mvkh    .S1 0x00810000, a11
        mvkl    .S1 0x00811000, a13      ; source bytes
        mvkh    .S1 0x00811000, a13
        mvk     .S2 0x55, b6             ; stale index: the first store must be masked
        nop     5

; transcribed from 0x8006AF6C..0x8006AFC8
        mvk     .S1 276, a3
        nop     3
        add     .L1 a3, a11, a3
        b       .S1 ring
        add     .L1 0, a13, a4
||      add     .L2X -1, a10, b0
        add     .L2X 0, a3, b5
||      ldbu    .D1T1 *a4++, a3
||      [b0]    b .S1 ring
        add     .L1 -1, a10, a1
||      add     .L2X -1, a12, b4
||      mvk     .S1 1, a2
||      [b0]    add .S2 -1, b0, b0
ring:
        add     .L2 1, b4, b4
||      extu    .S2 b6, 21, 21, b6
||      [a1]    ldbu .D1T1 *a4++, a3
||      [b0]    b .S1 ring
        [a2]    add .L1 -1, a2, a2
||      [a1]    add .S1 -1, a1, a1
||      [!a2]   stb .D2T1 a3, *+b6[b5]
||      xor     .L2 3, b4, b6
||      [b0]    add .S2 -1, b0, b0
        add     .L1X 0, b4, a12
        nop     2
done:
        idle
        b       .S1 done
        nop     5
