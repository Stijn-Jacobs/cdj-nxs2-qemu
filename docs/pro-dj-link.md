# Pro DJ Link

Setup asks for one deck or two (`--decks 1|2`, or `CDJ_DECKS=2` in `cdj.conf`).
Two decks find each other over an emulated Pro DJ Link network, and MASTER and
SYNC light up between them, same as the real players.

<a id="rekordbox-ableton"></a>

## Connecting rekordbox and Ableton Live on this PC

The decks' usual Pro DJ Link network is QEMU's multicast segment, which no
program on your PC can join. On Windows the deck can instead sit on a virtual
network adapter of your PC, and then:

- **rekordbox** on the PC is a Pro DJ Link peer of the deck. In Export mode
  they see each other, the deck browses the rekordbox library (the deck's
  REKORDBOX / LINK source) and loads and plays tracks from it.
- **Ableton Live** (12 tested) shows `1 Link` at the deck's BPM, and follows
  the deck's tempo, with the [`ableton_link`](../mods/ableton_link/README.md) mod on. rekordbox in
  Performance mode also lists the deck as a Link peer and syncs to it.

**One-time setup.** The adapter is the TAP-Windows6 driver that ships with
[OpenVPN](https://openvpn.net/community-downloads/) (the "TAP Virtual Ethernet
Adapter" component is enough). Install OpenVPN, then from an **elevated**
PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\net\tap_setup.ps1
```

It creates an adapter named `CDJ-Link` with its `tapctl.exe` (expected in
`C:\Program Files\OpenVPN\bin`; pass `-TapCtl <path>` if OpenVPN lives
elsewhere) and gives it the address `192.168.50.1/24`. It also caps the
adapter's MTU at 1500, adds a fixed ARP entry for the deck (`192.168.50.10`),
and opens one firewall rule on that adapter for UDP 67 (DHCP), 20808 (Ableton
Link) and 50000-50002 (Pro DJ Link). It is safe to run again, and starting a
deck afterwards needs no admin rights.

**Start the deck on it** (one deck; the rig's own DHCP server gives it
`192.168.50.10`):

```sh
DJLINK=tap:CDJ-Link bash scripts/run/rig.sh show
# with the ableton_link mod:
DJLINK=tap:CDJ-Link CDJ_MAIN_ABLETONLINK=1 CDJ_MAIN_ABLETONLINKPONG=1 bash scripts/run/rig.sh show
```

**In rekordbox**, switch to **Export mode** (the deck does not show up in
Performance mode for browsing). Once the deck has booted it appears in the
player list. On the deck press `L` (LINK) or `R` (REKORDBOX) for the source,
browse the library and load a track as from a USB stick.

**In Ableton Live**, turn on *Preferences > Link/Tempo/MIDI > Show Link
Toggle*, then press `LINK` in the transport. With the mod on, the toggle shows
`1 Link` and Live's tempo follows the deck's.

**If it does not work:**

- **`E-8309` on the deck when you load from rekordbox**: the adapter's MTU is
  not capped at 1500. TAP-Windows reports 65500, so Windows sends rekordbox's
  NFS replies as single oversized frames, which the deck drops. Run
  `tap_setup.ps1` again, or
  `netsh interface ipv4 set subinterface "CDJ-Link" mtu=1500 store=persistent`.
- **rekordbox does not see the deck**: rekordbox is in Performance mode, or it
  is sending on another adapter. Switch to Export mode; if the deck still does
  not appear, check that `CDJ-Link` is the adapter rekordbox's network settings
  use.
- **The launcher says there is no adapter of that name, or that it has no
  address**: the setup has not run, or the adapter was renamed.
- **One deck only, and Windows only.** A TAP adapter carries a single deck
  (`NDECKS=1`), and `DJLINK=tap:` is refused on other systems.
