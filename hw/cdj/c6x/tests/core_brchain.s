; Branches taken inside other branches' delay slots (stage 1's divide at
; 0x00800280): five seeds in consecutive packets keep a branch landing on the
; one-packet loop every cycle, so the loop body runs once per cycle while B1
; counts down, and the code after it runs exactly once when the chain drains.
        .text
        .global _start
_start:
        mvk     .S2 10, b1
        zero    .L1 a3
        zero    .L1 a5
        b       .S1 loop
        b       .S1 loop
        b       .S1 loop
        b       .S1 loop
        b       .S1 loop
loop:
        [b1]    add .L1 1, a3, a3
||      [b1]    sub .L2 b1, 1, b1
||      [b1]    b .S1 loop
        add     .L1 1, a5, a5
done:
        idle
        b       .S1 done
        nop     5
