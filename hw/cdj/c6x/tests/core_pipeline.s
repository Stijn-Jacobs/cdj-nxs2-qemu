; Core semantics the DSP image depends on, each checked against what TI
; SPRU732J states. Assembled with tic6x-elf-as, loaded at 0x00800000 and run
; by core_test.c until IDLE; core_test.c prefills the data at 0x00810000.
        .text
        .global _start
_start:
; 1. A load's result is readable 5 cycles later (Table 3-3: written in i+4).
        mvkl    .S1 0x00810100, a4
        mvkh    .S1 0x00810100, a4
        mvk     .S1 7, a0
        ldw     .D1T1 *a4, a0            ; cycle i
        mv      .L1 a0, a5               ; i+1: still 7
        nop     3                        ; i+2..i+4
        mv      .L1 a0, a6               ; i+5: 0x11111111

; 2. B has five delay slots; BNOP n fills n of them with NOP cycles.
        zero    .L1 a7
        b       .S1 over
        add     .L1 1, a7, a7            ; slot 1
        add     .L1 1, a7, a7            ; slot 2
        add     .L1 1, a7, a7            ; slot 3
        add     .L1 1, a7, a7            ; slot 4
        add     .L1 1, a7, a7            ; slot 5
        add     .L1 1, a7, a7            ; not executed
over:
        zero    .L1 a8
        bnop    .S1 over2, 3
        add     .L1 1, a8, a8            ; slot 4
        add     .L1 1, a8, a8            ; slot 5
        add     .L1 1, a8, a8            ; not executed
over2:

; 3. SPLOOP copy loop, SPRU732 Example 7-4: 8 words 0x00810000 -> 0x00810200.
        mvkl    .S1 0x00810000, a1
        mvkh    .S1 0x00810000, a1
        mvkl    .S2 0x00810200, b0
        mvkh    .S2 0x00810200, b0
        mvk     .S2 8, b1
        mvc     .S2 b1, ilc
        nop     3
        sploop  1
        ldw     .D1T1 *a1++, a2
        nop     4
        mv      .L2X a2, b2
        spkernel 6, 0
||      stw     .D2T2 b2, *b0++
        mv      .L1 a1, a9               ; a1 advanced by 8 words

; 4. SPLOOPW strcpy after SPRU732 Example 7-13: "abc" 0x00810300 -> 0x00810400.
;    The manual's "NOP 2" between MV and STB is dropped: with ii = 1 it lets the
;    next two iterations overwrite b0 before the store, so as printed the
;    example copies only the last character.
        mvkl    .S1 0x00810300, a4
        mvkh    .S1 0x00810300, a4
        mvkl    .S2 0x00810400, b4
        mvkh    .S2 0x00810400, b4
        [a0]    sploopw 1
||      mvk     .S2 1, b0
||      mvk     .S1 1, a0
        [a0]    ldb .D1T1 *a4++, a0
        nop     4
        [b0]    mv .L2X a0, b0
        spkernel 0, 0
||      [b0]    stb .D2T2 b0, *b4++
        stb     .D2T2 b0, *b4
        mv      .L2 b4, b10

        idle
        nop
        nop
        nop
        nop
        nop
        nop
        nop
