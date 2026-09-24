; Busy-wait detection across an interrupt: once the polling loop is a fixed
; point, an INT4 handler that stores only a word the loop never reads leaves it
; one, and c66x_step returns C66X_STOP_IDLE where the handler returns to. When
; the handler instead sets the polled word (variant word 0x0081050C = 1), the
; loop must run and consume it.
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
        mvkl    .S2 0x00812000, b15
        mvkh    .S2 0x00812000, b15
        mvkl    .S1 0x00810500, a4       ; polled word
        mvkh    .S1 0x00810500, a4
        mvkl    .S1 0x00810504, a5       ; status the loop republishes
        mvkh    .S1 0x00810504, a5
        zero    .L1 a6                   ; work done
        zero    .L1 a7
poll:
        ldw     .D1T1 *a4, a0
        nop     4
        stw     .D1T1 a0, *a5
        [a0]    add .L1 1, a6, a6
        [a0]    stw .D1T1 a7, *a4
        b       .S1 poll
        nop     5

        .org    0x480                    ; ISTP 0x00800400 + 4 * 0x20
        b       .S1 isr
        nop     5
        nop
        nop

        .org    0x600
isr:
        stw     .D2T2 b5, *b15--[1]
        stw     .D2T2 b1, *b15--[1]
        mvkl    .S2 0x00810508, b5
        mvkh    .S2 0x00810508, b5
        ldw     .D2T2 *b5, b1
        nop     4
        add     .L2 1, b1, b1
        stw     .D2T2 b1, *b5            ; interrupt count, never read by the loop
        ldw     .D2T2 *+b5(4), b1        ; variant word
        nop     4
        [b1]    stw .D2T2 b1, *-b5(8)    ; variant 1: set the polled word
        ldw     .D2T2 *++b15[1], b1
        nop     4
        ldw     .D2T2 *++b15[1], b5
        nop     4
        b       .S2 irp
        nop     5
