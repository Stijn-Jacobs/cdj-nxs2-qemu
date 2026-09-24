; The RS header bit against hand-encoded compact moves (SPRU732J 3.9.2.2, G-1,
; G-2): RS moves only 3-bit register fields to A16-A23/B16-B23; the 5-bit
; srcms:src2 of LSDmvto and dstms:dst of LSDmvfr name any register directly.
; gas cannot emit these under an RS header, and binutils decodes them wrongly,
; so the words are written out by hand. Every decoy register holds a value
; that identifies it if the move reads the wrong one.
        .text
        .global _start
_start:
        mvk     .S2 0x06, b6
        mvk     .S2 0x22, b22
        mvk     .S2 0x19, b19
        mvk     .S1 0x17, a17
        mvk     .S1 0x25, a25
        mvk     .S1 0x16, a16
        mvk     .S1 0x01, a1
        mvk     .S1 0x09, a9
        nop     5
        .align  5
packet:
        .short  0x4307          ; mv .L1 b6,b18       LSDmvto s=1 src2=6 dst=2
        .short  0xB98E          ; mv .S1X b19,a21     LSDmvto s=0 x=1 src2=19 dst=5
        .short  0x24D6          ; mv .D1 a17,a9       LSDmvfr s=0 src2=1 dst=9
        .short  0x9047          ; mv .L2X a16,b4      LSDmvfr s=1 x=1 src2=0 dst=4
        nop
        nop
        nop
        nop
        nop
        .word   0xE0680000      ; header: L1 L2 compact, RS
        nop     5
done:
        idle
        b       .S1 done
        nop     5
