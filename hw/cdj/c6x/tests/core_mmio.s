; Memory-mapped loads and stores touch exactly their own bytes: an ldw at UPIS2
; (0x02580058) is one 4-byte bus read, never a read of the next register too
; (read-to-clear peripherals break otherwise). core_test.c logs the bus.
        .text
        .global _start
_start:
        mvkl    .S1 0x02580058, a3
        mvkh    .S1 0x02580058, a3
        ldw     .D1T1 *+a3(0), a4
        nop     4
        ldh     .D1T1 *+a3(0), a5
        nop     4
        lddw    .D1T1 *+a3[1], a7:a6
        nop     4
        stw     .D1T1 a4, *+a3(0)
done:
        idle
        b       .S1 done
        nop     5
