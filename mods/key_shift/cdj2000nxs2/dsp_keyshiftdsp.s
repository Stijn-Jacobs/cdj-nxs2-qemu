; SPDX-License-Identifier: GPL-2.0-or-later
;
; Key shift for the CDJ-2000NXS2's C6655 program (MAIN 1.87), the DSP half:
; the pitch moves by n semitones and the tempo stays at the speed MAIN sends.
;
; The program plays through one of two engines per block, never both
; (dispatcher 0x8000B7B0): a resampler that moves pitch and tempo together,
; or, with MASTER TEMPO, a phase vocoder that stretches the stream by the
; speed r and keeps its pitch. With a shift the vocoder always runs, and the
; stream is resampled by p = 2^(n/12) on its way in, between the per-channel
; rings the stream puller fills and the staging buffers the vocoder reads.
; The resampled stream carries pitch p and runs p times fast, so the vocoder
; stretches it by r / p: the tempo comes back to r and the pitch stays p.
; The vocoder still takes 512 r (engine A) or 1024 r (engine B) source frames
; a frame, as stock MASTER TEMPO does at speed r.
;
; n is the low half of id-block word 2, which the program publishes to
; 0x8A73CA; 0 there, all a stock MAIN ever sends, is no shift, and then every
; hook below leaves the program's values as they were.
;
;   ks_mt        state handler 0x80005EB0, the setter's MASTER TEMPO argument:
;                the MT word, or 1 with a shift, so the setter engages the
;                vocoder with MT off on a paused deck
;   ks_mt_play   the play-path setter 0x8000D158's test of its MT argument at
;                0x8000D424: the same with MT off on a playing deck
;   ks_hop_a/b   the hop computation of engine A (0x80046CF0, r in b4) and
;                engine B (0x8004F3B8, r in b5): r / p in place of r, and p
;                kept for the frame's pulls and the next append
;   ks_pull      the stream puller 0x8003E4E0: floor(carry + hop p) source
;                frames in place of hop
;   ks_append    the append of engines A and B (0x80046C2C, 0x8004F2EC), in
;                place of the EDMA copy 0x8007D0B0 of hop frames from the
;                rings to the staging tails: hop frames interpolated from the
;                rings at step p
;
; r / p is held to at most 2, the vocoder's own bound on r: past it the
; shift gives way (p = r / 2), so the tempo still follows the fader.
;
; The interpolation is the 4-point cubic Hermite, in a scratch copy per
; channel: the last 5 source frames of the frame before, then the frames
; pulled for this one. Positions are Q20 in that copy; a frame's first output
; sits at 2 + the fraction the frame before carried over.
;
; Every float operation is waited out in full (3 delay slots for MPYSP,
; ADDSP, SUBSP, INTSP and SPINT, 4 for a load, 5 for a branch).

        .set    PUBLISHED_SHIFT, 0x008A73CA
        .set    PULL_BACK, 0x8003E4E8
        .set    EDMA_COPY, 0x8007D0B0
        .set    HOP, 1076                       ; DP+1076, the hop in frames
        .set    SPEED, 892                      ; DP+892, the speed in Q20
        .set    SCRATCH_L, 0x80080000           ; DDR3 past the program's last section
        .set    SCRATCH_R, 0x80082100           ; 0x2100 bytes a channel
        .set    HISTORY, 5

        .text

; The program's edits branch here, at fixed offsets: ks_mt, ks_hop_a,
; ks_hop_b, ks_pull, ks_append, ks_mt_play.
        b       .S2     ks_mt
        nop     5
        b       .S2     ks_hop_a
        nop     5
        b       .S2     ks_hop_b
        nop     5
        b       .S2     ks_pull
        nop     5
        b       .S2     ks_append
        nop     5
        b       .S2     ks_mt_play
        nop     5
        .word   0, 0, 0, 0                      ; to the next fetch packet

; active, p (Q20), carry (Q20), switched on
ks_state:
        .word   0, 0, 0, 0
; 2^(n / 12) for n = -12..12
ks_ratio:
        .word   0x3F000000, 0x3F079C7D, 0x3F0FACD6, 0x3F1837F0, 0x3F214518, 0x3F2ADC08, 0x3F3504F3
        .word   0x3F3FC887, 0x3F4B2FF5, 0x3F5744FD, 0x3F6411F0, 0x3F71A1BF, 0x3F800000, 0x3F879C7D
        .word   0x3F8FACD6, 0x3F9837F0, 0x3FA14518, 0x3FAADC08, 0x3FB504F3, 0x3FBFC887, 0x3FCB2FF5
        .word   0x3FD744FD, 0x3FE411F0, 0x3FF1A1BF, 0x40000000
        .word   0, 0, 0                         ; to the next fetch packet

; b4 = MT word | (n != 0). From callp; b3 is dead until the setter's call sets it.
ks_mt:
        mvkl    .S2     PUBLISHED_SHIFT, b0
        mvkh    .S2     PUBLISHED_SHIFT, b0
        ldh     .D2T2   *b0, b0
        ldw     .D1T2   *a12, b4
        b       .S2     b3
        nop     4
  [b0]  or      .L2     1, b4, b4

; b4 = 1 when the setter must take the resampler: no MASTER TEMPO and no
; shift, or DP+502 is 1, where the caller passes 0 for MT and stock never runs
; the vocoder. From callp in place of cmpeq .L2 0,b13,b4; b13 is the MT
; argument and nothing else reads it. b0 holds DP+1404 for the setter and is
; kept; the setter writes a0 and a3 before reading them.
ks_mt_play:
        mvkl    .S2     PUBLISHED_SHIFT, b4
        mvkh    .S2     PUBLISHED_SHIFT, b4
        ldh     .D2T2   *b4, b4                         ; n
        ldh     .D2T1   *+b14(502), a3
        nop     4
        cmpeq   .L2     0, b4, b4
||      cmpeq   .L1     1, a3, a0
||      b       .S2     b3
        or      .L2X    b4, a0, b4
||      cmpeq   .L1X    0, b13, a3
        and     .L2X    b4, a3, b4
        nop     3

; Engine A: b4 = r. Engine B: b5 = r. From callp, before the hop 512 r or
; 1024 r is formed, where b3 is free. Uses a0, a1, a16-a23, b1, b2, b16, b17.
ks_hop_a:
        mv      .L2     b4, b16
||      mvk     .S2     0, b2
        b       .S1     hop
        nop     5
ks_hop_b:
        mv      .L2     b5, b16
||      mvk     .S2     1, b2
hop:
        mvkl    .S1     PUBLISHED_SHIFT, a16
        mvkh    .S1     PUBLISHED_SHIFT, a16
        ldh     .D1T1   *a16, a1                        ; n
        mvkl    .S1     ks_state, a17
        mvkh    .S1     ks_state, a17
        ldw     .D1T1   *+a17(0), a0                    ; active on the frame before
        nop     4
  [!a1] b       .S1     hop_off
        mvkl    .S1     ks_ratio, a18
        mvkh    .S1     ks_ratio, a18
        add     .L1     12, a1, a19
        sub     .L1     12, a1, a20
        nop     1
        ldw     .D1T1   *+a18[a20], a20                 ; 1 / p = p(-n)
        ldw     .D1T1   *+a18[a19], a21                 ; p
        mvkl    .S1     0x40000000, a22                 ; 2.0
        mvkh    .S1     0x40000000, a22
        mvkl    .S1     0x49800000, a23                 ; 2^20
        mvkh    .S1     0x49800000, a23
        mpysp   .M2X    b16, a20, b17                   ; r / p
        mpysp   .M1     a21, a23, a21
        nop     3
        cmpgtsp .S2X    b17, a22, b1                    ; past 2: the shift gives way
        spint   .L1     a21, a21                        ; p, Q20
        nop     3
  [b1]  ldw     .D2T1   *+b14(SPEED), a21
        nop     4
  [b1]  mv      .L2X    a22, b17
  [b1]  shru    .S1     a21, 1, a21                     ; p = r / 2
        stw     .D1T1   a21, *+a17(4)
  [a0]  b       .S1     hop_done
  [!a0] mvk     .S1     1, a19                          ; switched on: no carry and a clear history
  [!a0] stw     .D1T1   a19, *+a17(0)
  [!a0] stw     .D1T1   a19, *+a17(12)
  [!a0] mvk     .S1     0, a19
  [!a0] stw     .D1T1   a19, *+a17(8)
hop_done:
  [!b2] mv      .L2     b17, b4
  [b2]  mv      .L2     b17, b5
        b       .S2     b3
        nop     5
hop_off:
        mvk     .S1     0, a19
        stw     .D1T1   a19, *+a17(0)
        b       .S2     b3
        nop     5

; The puller's hop read, by b from 0x8003E4E0: b4 = the frames to pull, then
; back to 0x8003E4E8. a0, a4, b3 and b8 are the puller's; uses a1, a16-a19
; and b5, which the puller sets before it reads it.
ks_pull:
        ldw     .D2T2   *+b14(HOP), b4
        mvkl    .S1     ks_state, a16
        mvkh    .S1     ks_state, a16
        mvkl    .S2     PULL_BACK, b5
        mvkh    .S2     PULL_BACK, b5
        ldw     .D1T1   *+a16(0), a1                    ; active
        ldw     .D1T1   *+a16(4), a17                   ; p, Q20
        ldw     .D1T1   *+a16(8), a18                   ; carry, Q20
        nop     4
        mpy32   .M1X    a17, b4, a19
        nop     3
        add     .L1     a18, a19, a19
        b       .S2     b5
        nop     4
  [a1]  shru    .S2X    a19, 20, b4

; The append, by b in place of the EDMA copy: a8 and a10 the rings, b8 and
; b10 the staging tails, b6 the hop (at least 1), b3 the way back. With no
; shift, on to the copy with every argument as it was. a10-a15 and b10-b15
; are kept.
ks_append:
        mvkl    .S2     ks_state, b16
        mvkh    .S2     ks_state, b16
        ldw     .D2T2   *+b16(0), b0
        mvkl    .S2     EDMA_COPY, b17
        mvkh    .S2     EDMA_COPY, b17
        nop     2
  [!b0] b       .S2     b17
        nop     5

        ldw     .D2T1   *+b16(4), a16                   ; p, Q20
        ldw     .D2T1   *+b16(8), a17                   ; carry, Q20
        ldw     .D2T2   *+b16(12), b1                   ; switched on
        mvkl    .S1     SCRATCH_L, a20
        mvkh    .S1     SCRATCH_L, a20
        mvkl    .S2     SCRATCH_R, b20
        mvkh    .S2     SCRATCH_R, b20
        mpy32   .M1X    a16, b6, a18
        nop     3
        add     .L1     a17, a18, a18                   ; carry + hop p
        shru    .S1     a18, 20, a19                    ; N, the frames pulled
        extu    .S1     a18, 12, 12, a18                ; the carry of the next frame
        stw     .D2T1   a18, *+b16(8)
        zero    .L1     a21
        zero    .L2     b21
  [b1]  stw     .D1T1   a21, *+a20(0)
  [b1]  stw     .D2T2   b21, *+b20(0)
  [b1]  stw     .D1T1   a21, *+a20(4)
  [b1]  stw     .D2T2   b21, *+b20(4)
  [b1]  stw     .D1T1   a21, *+a20(8)
  [b1]  stw     .D2T2   b21, *+b20(8)
  [b1]  stw     .D1T1   a21, *+a20(12)
  [b1]  stw     .D2T2   b21, *+b20(12)
  [b1]  stw     .D1T1   a21, *+a20(16)
  [b1]  stw     .D2T2   b21, *+b20(16)
        stw     .D2T2   b21, *+b16(12)

        ; scratch[5..5 + N) = ring[0..N)
        mv      .L1     a8, a22
        mv      .L2X    a10, b22
        addk    .S1     4 * HISTORY, a20
        addk    .S2     4 * HISTORY, b20
        mv      .L2X    a19, b0
  [!b0] b       .S1     copied
        nop     5
copy:
        ldw     .D1T1   *a22++, a23
||      ldw     .D2T2   *b22++, b23
        sub     .L2     b0, 1, b0
        nop     3
        stw     .D1T1   a23, *a20++
||      stw     .D2T2   b23, *b20++
  [b0]  b       .S1     copy
        nop     5
copied:
        mvkl    .S1     SCRATCH_L, a20
        mvkh    .S1     SCRATCH_L, a20
        mvkl    .S2     SCRATCH_R, b20
        mvkh    .S2     SCRATCH_R, b20

        ; the hop outputs, from 2 + carry at step p
        mvk     .S1     2, a24
        shl     .S1     a24, 20, a24
        add     .L1     a24, a17, a24
        mvkl    .S1     0x35800000, a25                 ; 2^-20
        mvkh    .S1     0x35800000, a25
        mvkl    .S1     0x3F000000, a26                 ; 0.5
        mvkh    .S1     0x3F000000, a26
        mvkl    .S1     0x3FC00000, a27                 ; 1.5
        mvkh    .S1     0x3FC00000, a27
        mvkl    .S1     0x3F800000, a28                 ; 1.0
        mvkh    .S1     0x3F800000, a28
        mvkl    .S1     0x40200000, a29                 ; 2.5
        mvkh    .S1     0x40200000, a29
        mv      .L1X    b8, a30
        mv      .L2     b10, b30
        mv      .L2     b6, b0
sample:
        shru    .S1     a24, 20, a2
        extu    .S1     a24, 12, 12, a9                 ; the fraction, Q20
        sub     .L1     a2, 1, a2                       ; k - 1
        mv      .L2X    a2, b19
        addaw   .D1     a20, a2, a3
        addaw   .D2     b20, b19, b18
        ldw     .D1T1   *+a3(0), a4                     ; x[k - 1]
||      ldw     .D2T2   *+b18(0), b4
        ldw     .D1T1   *+a3(4), a5                     ; x[k]
||      ldw     .D2T2   *+b18(4), b5
        ldw     .D1T1   *+a3(8), a6                     ; x[k + 1]
||      ldw     .D2T2   *+b18(8), b6
        ldw     .D1T1   *+a3(12), a7                    ; x[k + 2]
||      ldw     .D2T2   *+b18(12), b7
        intsp   .L1     a9, a9
        nop     3
        mpysp   .M1     a25, a9, a9                     ; f
        nop     3
        mpysp   .M1     a9, a26, a21                    ; f / 2
        mpysp   .M1     a9, a9, a31                     ; f^2
        mpysp   .M1     a9, a27, a18                    ; 3f / 2
        nop     3
        subsp   .L1     a28, a21, a23                   ; 1 - f / 2
        subsp   .L1     a21, a26, a8                    ; f / 2 - 1 / 2
        subsp   .L1     a18, a29, a18                   ; 3f / 2 - 5 / 2
        nop     3
        mpysp   .M1     a9, a23, a23                    ; f - f^2 / 2
        mpysp   .M1     a31, a8, a8                     ; w2 = f^3 / 2 - f^2 / 2
        mpysp   .M1     a31, a18, a18                   ; f^2 (3f / 2 - 5 / 2)
        nop     3
        subsp   .L1     a23, a26, a23                   ; f - f^2 / 2 - 1 / 2
        addsp   .L1     a28, a18, a18                   ; w0 = 1 + f^2 (3f / 2 - 5 / 2)
        nop     3
        mpysp   .M1     a9, a23, a23                    ; wm1 = f^2 - f^3 / 2 - f / 2
        nop     3
        addsp   .L1     a23, a18, a31
        nop     3
        addsp   .L1     a31, a8, a31
        nop     3
        subsp   .L1     a28, a31, a21                   ; w1 = 1 - wm1 - w0 - w2
        nop     3
        mv      .L2X    a23, b26
        mv      .L2X    a18, b27
        mv      .L2X    a21, b28
        mv      .L2X    a8, b29
        mpysp   .M1     a4, a23, a4                     ; the four terms, both channels
||      mpysp   .M2     b4, b26, b4
        mpysp   .M1     a5, a18, a5
||      mpysp   .M2     b5, b27, b5
        mpysp   .M1     a6, a21, a6
||      mpysp   .M2     b6, b28, b6
        mpysp   .M1     a7, a8, a7
||      mpysp   .M2     b7, b29, b7
        nop     3
        addsp   .L1     a4, a5, a4
||      addsp   .L2     b4, b5, b4
        addsp   .L1     a6, a7, a6
||      addsp   .L2     b6, b7, b6
        nop     3
        addsp   .L1     a4, a6, a4
||      addsp   .L2     b4, b6, b4
        nop     3
        stw     .D1T1   a4, *a30++
||      stw     .D2T2   b4, *b30++
        add     .L1     a24, a16, a24
        sub     .L2     b0, 1, b0
  [b0]  b       .S1     sample
        nop     5

        ; the last 5 frames of the copy are the next frame's history
        addaw   .D1     a20, a19, a3
        mv      .L2X    a19, b19
        addaw   .D2     b20, b19, b18
        ldw     .D1T1   *+a3(0), a4
||      ldw     .D2T2   *+b18(0), b4
        ldw     .D1T1   *+a3(4), a5
||      ldw     .D2T2   *+b18(4), b5
        ldw     .D1T1   *+a3(8), a6
||      ldw     .D2T2   *+b18(8), b6
        ldw     .D1T1   *+a3(12), a7
||      ldw     .D2T2   *+b18(12), b7
        ldw     .D1T1   *+a3(16), a8
||      ldw     .D2T2   *+b18(16), b9
        nop     4
        stw     .D1T1   a4, *+a20(0)
||      stw     .D2T2   b4, *+b20(0)
        stw     .D1T1   a5, *+a20(4)
||      stw     .D2T2   b5, *+b20(4)
        stw     .D1T1   a6, *+a20(8)
||      stw     .D2T2   b6, *+b20(8)
        stw     .D1T1   a7, *+a20(12)
||      stw     .D2T2   b7, *+b20(12)
        stw     .D1T1   a8, *+a20(16)
||      stw     .D2T2   b9, *+b20(16)
        b       .S2     b3
        nop     5
