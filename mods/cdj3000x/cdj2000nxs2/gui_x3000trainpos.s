! SPDX-License-Identifier: GPL-2.0-or-later
!
! Centre waveform position during a detail waveform update, for the
! CDJ-2000NXS2 display firmware (SH7269, SH-2A, big-endian) with the CDJ-3000X
! identity. patch_gui.py places this routine in unused on-chip RAM and calls it
! from the centre-waveform renderer, right after the renderer has loaded the
! position word 0xFFF88070 (1/150 s) into r4.
!
! While Serato fills in the detail waveform, the main board sends each new
! stretch of it as a train of link frames. The display takes about 6.6 ms a
! frame to drain one, and every position word in the train was written before
! the train went out, so the word stands still for the length of the train
! (100 to 120 ms) and the waveform stops. The detail handler (message 0x37)
! clears the tick enable 0x0E5BC274 on the train's first chunk and the beat
! tick handler (0x38) sets it again at the train's end: that clear is the
! start of a train, and nothing else clears it while a gridded track plays.
!
! Outside a train the word goes through as it is. The speed is learned from
! it, in counts per ms (Q8), over at least 100 ms of forward motion and up to
! a second (a span that reaches one second moves on to the one that started
! halfway, so it stays at least half a second long). Standing still for 40 ms
! (a pause) or moving back forgets the speed, unless a train starts at that
! render; a jump of more than 12 counts (a seek or a cue) only restarts its
! span.
!
! A change is timed halfway between the render that saw it and the one
! before. When the tick enable goes from set to clear while the deck has a
! speed, the word last seen and the time it changed are the anchor, and the renderer gets
! the anchor plus the speed times the time since, never less than it got
! before. When the word comes back ahead of that, the output closes in on it
! at 1.25 times the speed, so the waveform does not jump. The extrapolation
! ends once the output has reached the word, 300 ms after the anchor, or
! 100 ms after the ticks are in. Then a word ahead is taken as it is, a word
! up to 12 counts behind gets one more render of the last output, and a word
! further behind (a pause, cue or loop inside the train) is taken as it is.
! A word that jumps more than 12 counts from the output during a train is
! taken at once, and a train that starts before the last one has closed goes
! on from the last output. A track with no beat grid never sets the tick
! enable, so it never opens a train.
!
! In: r4 = the saved r0-r7 (r4 at offset 16, the position the renderer is
! about to use). pr is saved; r0-r7 are free.

        .text
        .global x3000trainpos
x3000trainpos:
        sts.l   pr,@-r15
        mov.l   r8,@-r15
        mov.l   r9,@-r15
        mov.l   r10,@-r15
        mov.l   r11,@-r15
        mov     r4,r11                  ! r11 = the saved r0-r7
        add     #-8,r15
        mov.l   system_time,r1
        jsr     @r1
        mov     r15,r4
        mov.l   @(4,r15),r9             ! r9 = the time now, ms
        add     #8,r15
        mov.l   @(16,r11),r10           ! r10 = the position word
        mova    state,r0
        mov     r0,r8
        mov.l   tick_enable,r1
        mov.l   @r1,r7                  ! r7 = the tick enable now
        mov.l   @(32,r8),r6             ! r6 = as it was at the last render
        mov.l   r7,@(32,r8)
        mov.l   @(4,r8),r1
        mov     r10,r2
        sub     r1,r2                   ! r2 = how far the word moved
        mov     #0,r3                   ! r3 = 1 when that is more than 12 counts
        mov     r9,r5                   ! r5 = when it changed
        tst     r2,r2
        bt      1f
        mov.l   @(52,r8),r1
        mov     r9,r5
        sub     r1,r5
        shlr    r5
        add     r1,r5                   ! halfway between the last render and this one
        mov.l   r10,@(4,r8)
        mov.l   r5,@(8,r8)
        mov     r2,r0
        add     #12,r0
        mov     #24,r1
        cmp/hi  r1,r0
        movt    r3
1:      mov.l   @(36,r8),r0
        tst     r0,r0
        bf      window

        tst     r2,r2
        bf      moved
        tst     r7,r7                   ! a train starting now is why it stands still
        bf      8f
        tst     r6,r6
        bf      edge
8:      mov.l   @(8,r8),r1
        mov     r9,r0
        sub     r1,r0
        mov     #40,r1
        cmp/hi  r1,r0
        bf      edge
        bra     rebase
        mov.l   r2,@(12,r8)             ! still for 40 ms: no speed
moved:
        tst     r3,r3
        bf      rebase
        cmp/pz  r2
        bt      forward
        mov     #0,r0
        mov.l   r0,@(12,r8)             ! going back: no speed
rebase:
        mov.l   r10,@(16,r8)
        mov.l   r9,@(20,r8)
        mov     #0,r0
        bra     edge
        mov.l   r0,@(28,r8)
forward:
        mov.l   @(20,r8),r1
        mov     r5,r4
        sub     r1,r4                   ! r4 = ms since the span began
        mov     #100,r0
        cmp/hs  r0,r4
        bf      edge
        mov.l   @(16,r8),r0
        mov     r10,r1
        sub     r0,r1
        shll8   r1
        mov     r4,r0
        divu    r0,r1
        mov.l   r1,@(12,r8)             ! the speed, counts per ms in Q8
        mov.l   @(28,r8),r1
        tst     r1,r1
        bf      2f
        movi20  #500,r0
        cmp/hs  r0,r4
        bf      edge
        mov.l   r10,@(24,r8)            ! the second span starts here
        bra     edge
        mov.l   r5,@(28,r8)
2:      movi20  #1000,r0
        cmp/hs  r0,r4
        bf      edge
        mov.l   @(24,r8),r0
        mov.l   r0,@(16,r8)
        mov.l   r1,@(20,r8)
        mov     #0,r0
        mov.l   r0,@(28,r8)
edge:
        tst     r7,r7
        bf      pass
        tst     r6,r6
        bt      pass
        mov.l   @(12,r8),r0
        tst     r0,r0
        bt      pass
        mov.l   @(4,r8),r0
        mov.l   r0,@(40,r8)             ! a train: the anchor is the word last seen
        mov.l   @(8,r8),r0
        mov.l   r0,@(44,r8)             ! and the time it changed
        mov     #1,r0
        bra     extrapolate
        mov.l   r0,@(36,r8)
pass:
        bra     store
        mov     r10,r2

window:
        cmp/eq  #1,r0
        bf      7f
        tst     r7,r7
        bt      3f
        mov     #2,r0                   ! the ticks are in
        mov.l   r0,@(36,r8)
        bra     3f
        mov.l   r9,@(48,r8)
7:      tst     r7,r7
        bf      3f
        tst     r6,r6
        bt      3f
        mov.l   @r8,r0                  ! the next train before this one closed:
        mov.l   r0,@(40,r8)             ! go on from the last output
        mov.l   @(52,r8),r0
        mov.l   r0,@(44,r8)
        mov     #1,r0
        mov.l   r0,@(36,r8)
3:      mov.l   @(44,r8),r1
        mov     r9,r4
        sub     r1,r4
        movi20  #300,r0
        cmp/hi  r0,r4
        bt      resolve
        mov.l   @(36,r8),r0
        cmp/eq  #2,r0
        bf      4f
        mov.l   @(48,r8),r1
        mov     r9,r0
        sub     r1,r0
        mov     #100,r1
        cmp/hi  r1,r0
        bt      resolve
4:      tst     r3,r3
        bt      extrapolate
        mov.l   @r8,r1
        mov     r10,r0
        sub     r1,r0
        add     #12,r0
        mov     #24,r1
        cmp/hi  r1,r0
        bt      take                    ! a cue or seek inside the train
extrapolate:
        mov.l   @(44,r8),r1
        mov     r9,r4
        sub     r1,r4
        mov.l   @(12,r8),r0
        mulr    r0,r4
        shlr8   r4
        mov.l   @(40,r8),r1
        add     r1,r4                   ! r4 = the anchor plus the time since at the speed
        mov.l   @r8,r2                  ! r2 = the last output
        cmp/hi  r4,r10
        bt      catch_up
        cmp/hs  r2,r4
        bf      store
        bra     store
        mov     r4,r2
catch_up:
        cmp/hs  r10,r2
        bt      store
        mov.l   @(52,r8),r1
        mov     r9,r5
        sub     r1,r5                   ! ms since the last render
        mov.l   @(12,r8),r0
        mulr    r0,r5
        mov     #5,r0
        mulr    r0,r5
        shlr8   r5
        shlr2   r5                      ! 1.25 times the counts gone by
        tst     r5,r5
        bf      5f
        mov     #1,r5
5:      add     r2,r5
        cmp/hs  r4,r5
        bt      6f
        mov     r4,r5
6:      cmp/hs  r10,r5
        bt      take
        bra     store
        mov     r5,r2

resolve:
        mov.l   @r8,r2
        cmp/hs  r2,r10
        bt      take
        mov     r2,r0
        sub     r10,r0
        mov     #12,r1
        cmp/hi  r1,r0
        bt      take
        mov     #0,r0                   ! up to 12 behind: one more render of the last output
        bra     store
        mov.l   r0,@(36,r8)
take:
        mov     #0,r0
        mov.l   r0,@(36,r8)
        mov     r10,r2
store:
        mov.l   r2,@r8
        mov.l   r9,@(52,r8)
        mov.l   r2,@(16,r11)
        mov.l   @r15+,r11
        mov.l   @r15+,r10
        mov.l   @r15+,r9
        mov.l   @r15+,r8
        lds.l   @r15+,pr
        rts
        nop

        .p2align 2
! patch_gui.py fills both in from the firmware: the system time call the UI
! task times its frames with, and the beat ticks' enable word
system_time:
        .long   0x0E52E9FC
tick_enable:
        .long   0x0E5BC274
! the first word is the position last handed out
state:
        .long   0                       ! +0  the last output
        .long   0                       ! +4  the word as last seen
        .long   0                       ! +8  the time it changed
        .long   0                       ! +12 the speed, Q8 counts per ms (0: none)
        .long   0                       ! +16 the word where the speed's span began
        .long   0                       ! +20 and its time
        .long   0                       ! +24 the word where the next span began
        .long   0                       ! +28 and its time (0: none yet)
        .long   0                       ! +32 the tick enable at the last render
        .long   0                       ! +36 0 outside a train, 1 in one, 2 its ticks are in
        .long   0                       ! +40 the anchor's word
        .long   0                       ! +44 and its time
        .long   0                       ! +48 the time the ticks came in
        .long   0                       ! +52 the time of the last render
