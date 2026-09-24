; SPLOOP exit (DSP frame copier, 0x0080209C): the instruction after the loop
; reuses A3 while the last iteration's load->add->store is still in the
; epilog. Post-loop code must reach E1 only after the SPKERNEL fetch delay plus
; the pipeline refill, so every word, including the last, is template + value.
        .text
        .global _start
_start:
        mvkl    .S1 0x00810600, a4       ; 8 u16 values
        mvkh    .S1 0x00810600, a4
        mvkl    .S2 0x00810700, b6       ; u32 output
        mvkh    .S2 0x00810700, b6
        mvkl    .S2 0x04020000, b5       ; template
        mvkh    .S2 0x04020000, b5
        mvk     .S2 8, b10
        mvc     .S2 b10, ilc
        nop     3
        sploop  1
        ldhu    .D1T1 *a4++, a3
        nop     4
        add     .L2X b5, a3, b4
        spkernel 3, 0
||      stw     .D2T2 b4, *b6++
        mvk     .L1 1, a3
        mv      .L2 b6, b11
done:
        idle
        b       .S1 done
        nop     5
