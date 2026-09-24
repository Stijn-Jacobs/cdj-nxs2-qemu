; Busy-wait detection: a loop that republishes an unchanged status word is a
; fixed point (c66x_step returns C66X_STOP_IDLE at its head); the same loop
; with a free-running counter is not.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810500, a4
        mvkh    .S1 0x00810500, a4
        mvkl    .S1 0x00810504, a5
        mvkh    .S1 0x00810504, a5
        zero    .L1 a6
poll:
        ldw     .D1T1 *a4, a0
        nop     4
        stw     .D1T1 a0, *a5
        ldw     .D1T1 *+a4(12), a1       ; a1 = 1 selects the counting variant
        nop     4
        [a1]    add .L1 1, a6, a6
        b       .S1 poll
        nop     5
