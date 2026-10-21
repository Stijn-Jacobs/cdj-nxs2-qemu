# MAIN-to-GUI delayed transmission test

`CDJ_LINK_TX_DELAY_US=0..50000` is an explicit cold-boot fault injection.
It defaults to zero and preserves the original immediate send behavior. Values
must contain decimal digits only; empty, signed, trailing and out-of-range
values reject startup. It is independent of the existing `CDJ_LINK_RX_GAP_US`,
which delays GUI-to-MAIN answers instead.

A positive delay copies each MAIN transmit DMA packet into one fixed 2048-byte
owner, retaining the original packet-length clamp and order. A QEMU virtual-time
timer delivers that immutable packet after `US*1000` ns. Only then does the model
clear DMA START, publish DMA completion units and publish serial TX_DONE, in the
original delivery/completion order. Old unacknowledged status is not cleared at
new admission. A duplicate START or control clear while delayed transmission is
pending is refused with an error; it restores START and cannot replace the
captured packet, deadline or completion. No unbounded queue or heap allocation
is introduced. Snapshot saving/loading is explicitly refused with positive TX
delay; zero-delay version3 snapshots keep their existing serialized fields.

With `CDJ_BF531_LINK_LOG=1`, actual timer delivery emits:

```
display link: tx-delay delivered delay_us=5000 scheduled_ns=1000 due_ns=5001000 delivered_ns=5001000 applied=1
```

`applied` counts packets actually delivered by the delay timer, not scheduling
attempts. `delivered_ns>=due_ns` and `due_ns-scheduled_ns=US*1000`. Packet dumps
and capture also happen at actual delivery. This is injected virtual time, not
physical hardware timing or normal performance-baseline evidence.

The focused test includes the actual `display_link.c` source with deterministic
stand-ins for QEMU memory, timers and IRQs. It exercises real MMIO write, packet
copy, timer and completion functions. It does not run firmware or an emulator
workflow and cannot establish integrated protocol acceptance.

```
cc -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  hw/cdj/boards/cdj2000/tests/display_link_test.c -o /tmp/display_link_test
/tmp/display_link_test
```

Coverage: strict bounded configuration, unchanged zero-delay packets and
completion, no early delivery/IRQ, exact and late timer callbacks, immutable
copied bytes, packet order, preserved prior status, duplicate/reentrant refusal,
full-buffer clamp, deadline overflow refusal, duplicate callback suppression and
stress snapshot rejection. The stubs deliberately assert that neither DMA nor
serial completion appears before packet delivery.
