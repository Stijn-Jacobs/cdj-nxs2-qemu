; Memory ordering between stores and loads (SPRU732J 4.2.3, 4.2.5): both touch
; memory at E3, so a store one packet before a load is seen by it, a load one
; packet before a store is not, and in one execute packet the load reads the old
; value while the store writes the new one. Also: a pre-increment base pending
; from the previous packet, a store of a register a load has not yet landed, and
; double-word byte order.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810000, a8
        mvkh    .S1 0x00810000, a8
        mvkl    .S1 0x11111111, a1
        mvkh    .S1 0x11111111, a1
        mvkl    .S2 0x00810000, b8
        mvkh    .S2 0x00810000, b8
        mvkl    .S2 0x22222222, b1
        mvkh    .S2 0x22222222, b1
        nop     5

        stw     .D1T1 a1, *a8            ; store then load, next packet
        ldw     .D1T1 *a8, a2
        nop     5

        ldw     .D1T1 *+a8(4), a3        ; load then store, next packet
        stw     .D1T1 a1, *+a8(4)
        nop     5

        stw     .D2T2 b1, *+b8(8)        ; same packet, store first
||      ldw     .D1T1 *+a8(8), a4
        nop     5

        ldw     .D1T1 *+a8(12), a5       ; same packet, load first
||      stw     .D2T2 b1, *+b8(12)
        nop     5

        mv      .L1 a8, a9
        stw     .D1T1 a1, *++a9(16)      ; base written at E1
        ldw     .D1T1 *a9, a6            ; next packet uses the new base
        nop     5

        ldw     .D1T1 *+a8(20), a7       ; pending load result (lands E5)
        stw     .D1T1 a7, *+a8(24)       ; stores the register as read at E1
        nop     5

        mvkl    .S1 0x55667788, a10
        mvkh    .S1 0x55667788, a10
        mvkl    .S1 0x11223344, a11
        mvkh    .S1 0x11223344, a11
        nop     5
        stdw    .D1T1 a11:a10, *+a8(32)
        lddw    .D1T1 *+a8(32), a13:a12
        nop     5
done:
        idle
        b       .S1 done
        nop     5
