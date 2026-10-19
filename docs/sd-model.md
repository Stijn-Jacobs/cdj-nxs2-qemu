# SH7764 SDHI and SD card model

The optional host/card at `0xffe40000` uses QEMU 9.1's `sd-card` on an
SD bus. Raw image size determines SDSC (up to 2 GiB) or SDHC (above 2 GiB).
`CDJ_SD_IMAGE=/absolute/path/card.raw` opens the raw file through a mandatory
process-local temporary snapshot overlay; firmware writes cannot alter the
source. Power-of-two image sizes are required by QEMU. Invalid images fail
startup, rather than silently falling back to an empty slot.

With no `CDJ_SD_IMAGE`, the original empty-slot stub and its VMState layout
remain in use. The launcher and single-deck defaults are unchanged.

The port follows the stock driver contract: 16-bit TMIO registers, short and long
response packing, W0C status, inverted interrupt masks, a single 512-byte
FIFO, PIO and DMA, counted multi-block transfer with automatic CMD12, and
20 microseconds of virtual time before each RXRDY/TXRQ. Acknowledging
RXRDY/TXRQ does not drain the FIFO. Only consuming a block schedules the
next one. Eject cancels the pending timer, drops the FIFO/IRQ, changes the
live card-detect pin and makes responding commands time out. QEMU supplies
SD identification, CSD/CID/SCR, application commands, card states,
byte/block addressing, storage reads/writes and card error responses.

The data IRQ is INTEVT `0x9c0`, priority field INT2PRI12[23:16], mask bit
18 in INT2MSKR1. Read/write DMA use channels 5/4 respectively, with their
existing `0x7a0`/`0x780` completion events. The optional fixed-FIFO callback
in the shared DMAC is attached only when the SD model exists, preserving
other endpoints and their completion behavior. DMA reads and writes copy
only a staged block; stale or oversized transfers cannot invent data.

A configured NXS card supplies the physical closed-lid contact (panel
payload `17:04`). Card detect is active low. The card is initially absent,
then inserted after 20 seconds of virtual time so the stock driver's
absent-to-present mount gate can arm. `CDJ_SD_INSERT=N` changes the delay;
`0` leaves the card ejected until a runtime insert. No guest media-ready
state or filesystem result is injected.

Runtime control uses the existing QEMU monitor/QMP interface:

```text
qom-get /machine/sdhi card
qom-set /machine/sdhi card eject
qom-set /machine/sdhi card insert
```

QMP equivalent: `qom-set` arguments
`{"path":"/machine/sdhi","property":"card","value":"insert"}`.
Insert/eject also closes/opens the lid. Reinsertion keeps the same card and
its process-local write overlay; the stock driver reinitializes with CMD0.
`CDJ_SDHI_TRACE=1` logs actual SD commands and automatic stops.

The model is scoped to MAIN's SD memory-card driver, not SDIO, UHS,
electrical/CRC fault injection or arbitrary unbounded SD transfers. The
host/card has VMState fields, but cross-process persistence of its temporary
write overlay has not been qualified. This is separate from source-image
snapshot protection.

Build and host tests, all local:

```sh
PATH=/opt/homebrew/bin:$PATH QEMU_BUILD="$PWD/qemu-build" \
  SKIP_PATCH_REGEN=1 ./build.sh main
python3 scripts/test/test_cdj2000_sdhi.py qemu-build/qemu-system-sh4
```

The firmware-free qtest suite covers SDSC and SDHC initialization/R2,
register byte/halfword/word ordering, W0C, FIFO acknowledgement vs draining,
PIO and DMA reads/writes, multi-block reads/writes and automatic stop, out-of-range card
errors, stale FIFO reads, pending-transfer eject, reinsertion and unchanged
raw source bytes. An optional second argument supplies the baseline binary
for an exact empty-slot MMIO transcript comparison. `scripts/media/make_sd_fixture.py` builds a deterministic FAT32 fixture
image from a user-supplied USB image (the original is only read).

Local stock qualification (requires extracted NXS 1.44 images and the
existing qualified DSP modules; no files are written under `eval/`):

```sh
C66X_JIT="$HOME/c14gen/curated-cdj2000nxs/m.so:$HOME/c14gen/curated-cdj2000nxs/m.extra.so" \
C66X_JIT_AUTO=0 CDJ_SDHI_TRACE=1 \
python3 scripts/test/qualify_sd.py runs/sd/stock-proof \
  --qemu runs/sd/qemu-system-sh4.sd-final \
  --main extract/cdj2000nxs/main_unpacked.bin \
  --display extract/cdj2000nxs/display.upd \
  --source sd --sd runs/sd/fixture/diagnostic-sd.img \
  --play 20 --root-timeout 180 --load-timeout 90 --keys
```

This selects SD and omits USB completely. The default `--source usb` mode
uses the xcheck sequence without SD-specific settling or encoder changes.
