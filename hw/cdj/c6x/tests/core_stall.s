; A cross-path stall freezes the whole pipeline: a load issued before it still
; becomes readable five execute packets later, not five clock cycles later.
; The long-block width builder at 0x8006D040 stalls every iteration and relies
; on its two load streams into B16 keeping their packet spacing.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810000, a8
        mvkh    .S1 0x00810000, a8
        zero    .L1 a5
        zero    .L2 b1
        nop     5
        ldw     .D1T1 *a8, a5            ; packet 0: data readable at packet 5
        mvk     .S1 7, a1                ; packet 1: E1 write
        add     .L2X b1, a1, b2          ; packet 2: 2X read of it, stalls once
        mv      .L1 a5, a10              ; packet 3
        mv      .L1 a5, a11              ; packet 4: still the old value
        mv      .L1 a5, a12              ; packet 5: the load
        nop     5
done:
        idle
        b       .S1 done
        nop     5
