# HID mode — quick start

Run the emulated CDJ-2000NXS2 as a HID controller, the way a real CDJ plugged into
a laptop is. The firmware does everything: in **LINK › CONTROL MODE › USB MIDI** (the
PC / rekordbox source) the deck speaks the Pioneer HID protocol over its rear USB-B
port. Nothing is reimplemented — the emulator relays that port and a small bridge
turns the relay into a HID device.

> **To connect to rekordbox today, use Pro DJ Link, not HID.** On Windows and macOS
> there is no virtual-HID device yet (Windows needs a Virtual HID Framework
> minidriver, macOS a DriverKit one), so rekordbox there cannot open the deck over
> HID. Pro DJ Link over a TAP adapter in **Export mode** is the working path — see
> *Connecting rekordbox and Ableton Live on this PC* in the top-level `README.md`.
> HID mode is for watching the handshake anywhere and, on Linux, real HID software.

## Run it

1. Start a deck with its USB-B port relayed (`CDJ_USBF_HOST` plugs the relay in):

   ```sh
   CDJ_USBF_HOST=27020 bash scripts/run/rig.sh show
   ```

2. On the deck, set the source to PC: **LINK › CONTROL MODE › USB MIDI**.

3. Run the bridge on the same port:

   ```sh
   python3 scripts/net/usb_hid_port.py --monitor    # anywhere: one line per report
   python3 scripts/net/usb_hid_port.py              # Linux: makes a /dev/uhid device
   ```

`--monitor` prints every report and names the handshake packets (announce, host
challenge, deck reply, host reply, ack) — that is how you confirm the firmware
reached HID mode. On Linux, drop `--monitor` to create a real HID device
(`/dev/uhid`) that a Linux DJ app (e.g. Mixxx) can open; `--name` sets the product
string the host matches the model on (default `CDJ-2000NXS2`).

## Status

| | |
|---|---|
| firmware HID handshake | works (it is stock firmware behaviour) |
| `--monitor` (watch it) | works on Windows, macOS, Linux |
| real HID device | **Linux only** (`/dev/uhid`) |
| rekordbox over HID | **not yet** on Windows/macOS — needs a virtual-HID driver; use Pro DJ Link |
