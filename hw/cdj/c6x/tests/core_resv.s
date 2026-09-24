; The Layer III main-data copy, transcribed from 0x80068550..0x800685A4: a
; self-branching loop whose first pass is masked by a1 (mpysu of its low half
; clears it), moving ring bytes backwards through the (i XOR 3) index mapping.
; Parameters are read from 0x00813000: base a9, dst a5, src a8, count b0.
        .text
        .global _start
_start:
        mvkl    .S1 0x00813000, a0
        mvkh    .S1 0x00813000, a0
        mvkl    .S2 0x00812000, b15
        mvkh    .S2 0x00812000, b15
        mvkl    .S2 0x00810114, b5       ; ring
        mvkh    .S2 0x00810114, b5
        ldw     .D1T1 *+a0(0), a9
        ldw     .D1T1 *+a0(4), a5
        ldw     .D1T1 *+a0(8), a8
        ldw     .D1T2 *+a0(12), b0
        mvk     .S1 -1, a7
        nop     5
        stw     .D2T1 a9, *+b15(16)
        zero    .L1 a9
        nop     5

        ldw     .D2T1 *+b15(16), a9
||      mvc     .S2 csr, b4
||      sub     .L1 a0, a0, a1
||      add     .S1X 0, b5, a6
||      add     .D1 a5, 1, a5
        and     .L2 -2, b4, b5
||      mvkh    .S1 65536, a1
        mvc     .S2 b5, csr
loop:
        [b0]    b .S2 loop
||      [!a1]   ldbu .D1T1 *+a3[a6], a3
        add     .L1 1, a7, a7
        add     .L1 a9, a5, a4
||      sub     .S1 a8, a7, a3
        xor     .L1 3, a4, a4
||      add     .S1 a9, a3, a3
        [a1]    mpysu .M1 2, a1, a1
||      extu    .S1 a4, 21, 21, a16
||      xor     .L1 3, a3, a4
        add     .L1 -1, a5, a5
||      [!a1]   stb .D1T1 a3, *+a16[a6]
||      [b0]    add .L2 -1, b0, b0
||      extu    .S1 a4, 21, 21, a3
        mvc     .S2 b4, csr
        nop     5
done:
        idle
        b       .S1 done
        nop     5
