# CDJ-850 service manual: agent reference

Digest of the Pioneer CDJ-850 service manual (order no. RRV4128, July 2010;
ManualsLib copy, 127 PDF pages, kept outside the repo as
`~/Documents/cdj850-emu/f70e4d.pdf`). Code comments cite it as
"service manual f70e4d.pdf". **Page numbers below are the manual's own
printed page numbers**; the PDF page is printed page + 4.

This file is for agents working on the emulator. It records what the manual
says, with a section reference for each fact, so you rarely need to open
the PDF. Where the emulator already models something, the file is named.
Things the manual does *not* settle are marked **open**.

Rendering pages yourself: `pdftoppm` is not installed on this Mac. Split the
page out with pypdf, then render it with
`sips -s format png -Z 3200 page.pdf --out page.png`. Crop schematics with
`sips -c H W --cropOffset Y X`.

---

## 1. Hardware at a glance (§6.2, §4.2, §12)

| Ref | Part | Role | Assy |
|---|---|---|---|
| IC103 | R5S72630P200FP (SH7263, SH-2A) | MAIN CPU, 33 MHz crystal, 200 MHz core, 66 MHz bus (CKIO pin 50) | MAIN |
| IC101 | DYW1796 (= programmed S29GL032N90TFI030, 32 Mbit NOR, 16-bit) | MAIN firmware **and user settings** | MAIN |
| IC102 | K4S561632J-UC75 (256 Mbit SDRAM, 16-bit) | MAIN work RAM | MAIN |
| IC301 | DSPC56371AF180 (DSP56371) | Audio DSP; program downloaded by MAIN at boot | MAIN |
| IC302 | PCM1742KE | Audio DAC (chip-selected by DACCS) | MAIN |
| IC303 | RNB4580F | Output op-amp | MAIN |
| IC201 | TC94A15FG | Servo DSP (CD) | MAIN |
| IC204 | BD7956FS | Servo/motor driver (spindle, sled, loader, focus/track) | MAIN |
| IC105 | TC74VHCU04FT + X103 16.9344 MHz | Buffers CLK_S_16M (servo) and CLK_A_16M (audio DSP) | MAIN |
| IC107 | TS3USB30RSW | USB D+/D- switch, USB-A (host) vs USB-B (PC), select = USB_ABSEL | MAIN |
| X102 | 48 MHz | USB clock (USB_X1/X2, pins 93/94) | MAIN |
| IC104 | — | MAIN reset buffer (MAIN_XRST → RES pin 59) | MAIN |
| IC1007 | PEG799A8 | PANEL CPU: keys, LEDs, both FL tubes, jog, standby/power control; 15.975 MHz crystal X1001 | DFLB |
| IC1004 | BD45302G | Panel reset IC (always powered from standby rail) | DFLB |
| V1001 | DEL1069 | DATA FL (main dot-matrix VFD) | DFLB |
| V1801 | DEL1058 | JOG FL (jog centre display) | JFLB |
| IC2003 | BD2051AFJ | USB-A VBUS switch / overcurrent | CMPX |
| PC1901 | photo interrupter | Jog encoder (JOG1/JOG2 quadrature) | JOGB |

Not in the firmware's reach, but useful to know: the unit has **no
EEPROM**. §8.2 says every user setting lives in IC101 flash:

| Setting | Values (factory default **bold**) | Stored as |
|---|---|---|
| A. CUE LEVEL | -36/-42/-48/-54/**-60**/-66/-72/-78 dB | UTILITY setting |
| MIDI CHANNEL | **1**..16 | UTILITY setting |
| LIBRARY CREATOR | **LIBRARY**/FOLDER | UTILITY setting |
| HISTORY NAME | **HISTORY** | UTILITY setting |
| TAGLIST NAME | **TAG LIST** | UTILITY setting |
| AUTO STANDBY | OFF/**ON** | UTILITY setting |
| VERSION No. | — | UTILITY setting |
| TIME MODE | TIME/**REMAIN** | key status |
| AUTO CUE | ON/**OFF** | key status |
| JOG MODE | CDJ/**VINYL** | key status |

So the emulator's flash image (`PERSIST=1`, `extract/flash-<tag>.bin`) *is*
the unit's settings memory, and a factory reset (§5 below) rewrites flash.

Board assemblies (§3.3): D MAIN (DWX3174), E CMPX (DWX3153, power/IO
board with the rear jacks), F USBA (DWX3156), G DFLB (DWX3152, panel CPU and
DATA FL), H KSW1, I KSW2, J JFLB (jog FL), K JOGB (jog encoder), L SLD1,
M PSWB (power switch), N SLD2 (tempo slider), A CNCT / B SLMB / C JINT (CD
mechanism side), O POWER SUPPLY, P ACIN.

---

## 2. MAIN CPU (IC103) pin and net map

Sources: §4.2 signal block diagram (printed p.18) and §10.2 MAIN ASSY 1/4
(printed p.69). Port names come from the SH7263 pin-function labels on the
schematic symbol. Cross-checked against `xc/port.c`, `xc/intc.c`,
`xc/scif.c`, `xc/dsp_shi.c`, `dsp56371.c`.

| Pin | Port / function | Net | Dir | Meaning |
|---|---|---|---|---|
| 48 | PE3/SCK1 | TSCK | in | Panel link clock (panel CPU is clock master) |
| 64 | PE4/RXD1 | TSI | in | Panel → MAIN data |
| 66 | PE5/TXD1 | TSO | out | MAIN → panel data |
| 70 | PE9/IRQ5 | PNL_XBUSY | in | Panel "busy/ready" handshake |
| 75 | PC14 | MAIN_XBUSY | out | MAIN "link ready" to panel |
| 59 | RES | MAIN_XRST (via IC104) | in | Reset, driven by panel CPU pin 40 |
| 49 | PE0/RXD0 | DBG_RxD | in | **Debug UART** (see §2.1) |
| 63 | PE1/TXD0 | DBG_TxD | out | **Debug UART** |
| 50 | CKIO | CLK_66M | out | Bus clock to flash/SDRAM |
| 55/56 | XTAL/EXTAL | — | — | 33.00 MHz crystal X101 |
| 85 | PB8 | SH_STATUS | out | Heartbeat test point, toggles about every 1 s when firmware runs (§5.2 [10-1], waveform 4) |
| 87 | PB10 | USB_ABSEL | out | IC107 select: H = USB-A (stick), L = USB-B (PC) |
| 88 | PB11 | USB_VBUSON | out | Enable USB-A VBUS (IC2003) |
| 71 | PE10/IRQ6 | USB_CDETFLG | in | H = normal, L = USB-A overcurrent |
| 102 | VBUS | VBUS | in | USB-B host present (PC mode) |
| 100/101 | DM/DP | USB D-/D+ | io | USB full speed, through IC107 |
| 72 | PE11/IRQ7 | DSPDREQ | in | Audio DSP request (waveform 5: ~40 µs low pulse) |
| 158 | (PF group) | DSPRST | out | Audio DSP reset (§5.1) |
| 169/167/166/165 | PF0/SSCK0, PF1/SSI0, PF2/SSO0, PF3/SCS0 | AUDIO_SCK, AUDIO_SI, AUDIO_SO, DSPCS | | SSU0 = DSP56371 SHI (SPI) link (`xc/dsp_shi.c`) |
| 148 | PF16 | PLAY | out | To DSP PF10 (`dsp56300.c`) |
| 161 | — | DACCS | out | PCM1742 control chip select |
| 185 | PB4/IRQ4 | ZERO | in | DAC zero-detect (`xc/intc.c`) |
| 160 | — | MUTE | out | Audio mute |
| 149/150 | — | CONT1/CONT2 | in | CONTROL mini-jack (fader start), via Q101/Q102 |
| 135 | PF25 | LPS1 | in | Loader switch 1 (L = switch ON) |
| 134 | PF26 | LPS2 | in | Loader switch 2 |
| 136 | — | INSIDE (INSW) | in | Pickup at innermost track = L |
| 113 | PA3/AN3 | ADC | in | Servo analog (`xc/adc.c`) |
| 208 | PD4/TIOC1A | TCOUNT | in | Servo tacho/count |
| 181 | PB2/IRQ2 | SPDLFG | in | Spindle FG from IC204 |
| 126 | PF27/SSISCK3 | SRVSCLK | | Servo audio-format serial clock |
| 125 | PF28/SSIWS3 | SRVLRCK | | |
| 124 | PF29/SSIDATA3 | SRVSDATA | in | CD audio from servo DSP |
| 62 | PE8/SCK2 | SRVBCLK | | Servo bus clock |
| 68 | PE6 | SRVCCE | out | Servo chip enable |
| 69 | PE7 | SRVRST | out | Servo DSP reset |
| 73, 81, 82, 83 | PE12..PE15 | SRVBUS0..3 | io | Servo command nibble |
| — | — | DRVMUTE1/2, PWMLO, PWMSL1IN, PWMSL2IN | out | Driver enables / PWM (IC204) |
| 61 | ASEMD | XASEMD | | JTAG/ASE mode |
| 90 | MD | — | | Boot mode strap (R159 10k) |
| 172–178 | TMS/TDI/TRST/TDO/TCK, ASEBRKAK | JTAG | | JTAG header on MAIN |

Pin numbers from the §4.2 block diagram are reliable. A few were read off a
dense schematic, which is harder: before you build a model on one, check it
against the firmware's PFC writes.

### 2.1 Debug UART (SCIF0)

§10.2 has an unpopulated 4-pin header labelled **UART**: DBG_TxD (pin 63,
TXD0), DBG_RxD (pin 49, RXD0), V+3R3_B, GNDD. This is the ASCII console
that `xc/scif.c` captures under `CDJ850_TRACE`. On real hardware it is the
first thing to solder to for boot logs (3.3 V levels).

### 2.2 Panel CPU (IC1007) pins (§4.2 p.19, §5.2, §10.15)

| Pin | Net | Notes |
|---|---|---|
| 12 | PNL_XRST | Reset from IC1004 pin 4 |
| 13/15 | XOUT/XIN | 15.975 MHz |
| 31 / 32 / 33 | TSI (in from MAIN TSO) / TSO (out to MAIN TSI) / TSCK out | Panel link; panel is clock master |
| 34 | PNL_XBUSY | out |
| 42 | MAIN_XBUSY | in |
| 40 | MAIN_XRST | out: releases MAIN reset |
| 43 | PNL_XMUTE | out: audio mute |
| 44 | VDET | in: power-good, 3.3 V normal, 0 V on failure |
| 45 | PWR_ON | out: switches MAIN power on |
| 90 | PWR_SW | in: power switch, L = pressed/ON |
| 1 / 2 | ADCT (1.65 V ref) / ADIN | Tempo slider, 3.3 V (+ side) → 0 V (- side) |
| 88 | TCH/REL | VINYL SPEED ADJUST (touch/release) pot, 3.3 V → 0 V turning CCW |
| 25 / 26 | JOG2 / JOG1 | Jog quadrature |
| 27 | JOG_SW | Jog touch (sheet switch), L = touched |
| 3 / 4 / 5 | ENC_SW / ENC_2 / ENC_1 | Rotary selector |
| 35/37/39/38 | D_DSO / D_SCLK / D_LAT / D_BK | DATA FL serial |
| 24/23/21/28 | J_DSO / J_SCLK / J_LAT / J_BK | JOG FL serial |
| 83, 87, 85, 84, 86 | KEY2, KEY0, KEY1, KEY3, KEY4 | **Resistor-ladder (analog) key inputs**, several keys per line |
| 60, 61, 56, 54, 51, 57, 71–74, 65, 63 | PLAY_SW, CUE_SW, LOOPIN_SW, LOOPOUT_SW, RELOOP_SW, USB_STOP, 8/4/2/1KEY_SW, SHIFT_SW (BEAT SELECT), REV_SW | Digital keys, L = pressed |

Ladder groups (from the block diagram): KEY0 = SLD1 (DISC EJECT, JOG MODE
VINYL, TEMPO, MASTER TEMPO); KEY1 = CUE/LOOP CALL ◀ ▶, DELETE, MEMORY;
KEY2 = KSW2 (TRACK SEARCH, SEARCH); KEY3 = PC, USB, DISC, TIME MODE/AUTO
CUE; KEY4 = BROWSE, TAG LIST, INFO, MENU, BACK, TAG TRACK/REMOVE. The
ladder resistor values are on §10.8/§10.9 (printed p.80–83) if a panel-CPU
model ever needs them. Today the emulator only sees the panel through the
SCIF1 link (`xc/panel_peer.c`, `xc/panel_link.c`).

---

## 3. Boot and power-on sequence (§5.1, printed p.22)

1. AC in: standby rails V+12_EUP / V+3R3_EUP feed the panel CPU only.
   IC1004 releases PNL_XRST, panel enters STANDBY (FL dark).
2. POWER switch ON: panel shows "POWER ON" on the FL, drives PWR_ON
   (pin 45) to power MAIN, then releases MAIN_XRST (panel pin 40 → IC103
   pin 59).
3. MAIN: **downloads the program to the audio DSP**, then releases DSPRST
   (IC103 pin 158 → IC301 pin 47) and SRVRST (IC103 pin 69 → IC201 pin 90).
4. MAIN ↔ panel communication starts. **If it fails, "E-8709".**
5. CD drive: inside switch check, LD on, focus/tracking offset, spindle
   kick + focus sweep, focus check (S-curve ≤ 0.88 Vp-p → retry with more
   RF gain; three failures → "EJECT" shown), spindle accel 200 ms, CLV on,
   wait 300 ms, tracking balance, tracking close, GFS (RF lock) check,
   focus/tracking/RF gain adjust, TOC read, first-track search, unmute.

Power monitoring (§5.5): from 0.1 s after power-on the panel CPU watches
VDET (pin 44). If VDET goes low for 0.5 s it shuts the unit down and flashes
the STANDBY LED. The error latches for about 1 minute after AC is removed.
Removing R1123 on DFLB disables the monitor (bench diagnosis only).

---

## 4. Error codes (§6.3 [4], printed p.39; diagnosis in §5.2 [9])

The time display shows `E-XXXX`.

| Code | Sub | Type | Medium | Meaning / manual's diagnosis |
|---|---|---|---|---|
| E-6002 | — | DSP PROGRAM | — | MAIN cannot write the program to the audio DSP at startup. Check V+3R3_A/V+1R25, then whether IC301 runs. **For the emulator: the DSP SHI boot upload failed or timed out (DSPDREQ handshake).** |
| E-7024 | — | Updating error | — | Firmware update failed; see §6 recovery |
| E-7201 | 26 | TOC READ ERROR | CD | TOC unreadable |
| E-8301 | 22 | PLAYER ERROR | CD | Focus servo cannot close |
| E-8301 | 91 | PLAYER ERROR | CD | Pickup cannot return to inner track (INSIDE switch never seen) |
| E-8302 | 12 | Playback error | CD | Target address not found |
| E-8302 | 15 | Playback error | CD | Address unreadable |
| E-8303 | 99 | Buffer write error | CD | Writing music data to buffer failed |
| E-8304 | — | Decoding error | CD | MP3/AAC data or decode error |
| E-8305 | — | Format error | CD | Right extension, unsupported format |
| E-8306 | — | No music file | USB | Track in library/playlist is missing from the device |
| E-8307 | — | USB writing error | USB | Writing to USB device failed |
| E-8709 | — | COMMUNICATION ERROR | — | Panel CPU ↔ MAIN link failed. Check TSCK, TSI, PNL_XBUSY, TSO, MAIN_XBUSY, V+3R3, V+1R2. **For the emulator: the SCIF1 panel-link handshake is wrong.** |
| E-9101 | 90 | LOADING TIMEOUT | CD | Loader mechanism timeout (LPS1/LPS2 never reached) |

These strings and codes should exist in the MAIN image. Searching for them
is a quick way to find the handler that raises each one.

---

## 5. Service modes (§6, printed p.35–41)

All are entered by **holding keys while switching POWER on** (the panel CPU
samples them), except version display.

| # | Mode | Entry | Exit / navigation |
|---|---|---|---|
| 1 | Panel diagnosis ("SERVICE MODE", panel-CPU function) | **BACK + TEMPO** held at power-on | TAG TRACK pages forward, BACK pages back |
| 1a | Jog load measurement | In mode 1, turn VINYL SPEED ADJUST (TOUCH/RELEASE) fully clockwise | POWER OFF |
| 2 | Version / error history | Hold **RELOOP ≥ 10 s** (normal operation, no power cycle) | VINYL MODE switches version ↔ error history; CUE/LOOP CALL ◀ ▶ step through the error log |
| 3 | CD drive diagnosis | **TIME + CD (DISC) SELECT** at power-on | POWER OFF |
| 4 | Firmware update from USB | **USB SELECT + RELOOP** at power-on | POWER OFF after "COMPLETE!" |
| 5 | Firmware update from CD-ROM | **CD (DISC) SELECT + RELOOP** at power-on | POWER OFF |
| 6 | Factory reset | **BACK + USB SELECT** at power-on | resets the §8.2 items |

"TEMPO" is the TEMPO (range) key on SLD1; it lights the STANDBY-labelled
LED in the key test (see below).

**Emulator note.** `./start.sh --service` and `docs/using-the-deck.md`
describe the NXS2's combination (TEMPO RANGE + MEMORY). On the CDJ-850 the
keys are read by the panel CPU, which the emulator stands in for over
SCIF1. Mode entry therefore depends on what the panel reports at the first
link exchange. `$CDJ850_PANEL_RX` (`xc/panel_link.c`) is the test hook for
"keys held at power-on", but the real key-event frame format is still
**open**, so no combination is wired yet. The manual tells you which keys
to emulate. It does not tell you the frame bytes.

### 5.1 Panel diagnosis screen (mode 1, §6.3 [1])

The BROWSE FL (the DATA FL dot-matrix) shows three lines:

```
SERVICE MODE
  BUTTON   <name of key held>
  <JOG | ENCODER | VOLUME>  ■■■■…   (bar for the last control touched)
```

The key's LED lights while it is held. Key → LED → displayed name
(printed p.37):

| Key | LED lit | BUTTON text | Other |
|---|---|---|---|
| PLAY/PAUSE | PLAY/PAUSE | PLAY | |
| CUE | CUE | CUE | |
| IN/CUE (IN ADJUST) | IN/CUE | IN | |
| OUT (OUT ADJUST) | OUT | OUT | |
| RELOOP/EXIT | RELOOP/EXIT | RELOOP | |
| TRACK REV / FWD | — | TRACK ⏮ / TRACK ⏭ | FWD: JOG FL pattern 1 |
| SEARCH REV / FWD | — | REV ◀◀ / FWD ▶▶ | JOG FL pattern 2 / 5 |
| JOG MODE | VINYL | JOG MODE | JOG FL pattern 4 |
| TEMPO | STANDBY | RANGE | |
| MASTER TEMPO | MASTER TEMPO | MT | |
| TIME MODE/AUTO CUE | — | TIME | |
| DELETE | all LEDs | DELETE | all JOG FL segments on |
| MEMORY | — | MEMORY | |
| EJECT | EJECT | EJECT | |
| CUE/LOOP CALL ◀ / ▶ | — | ◀ CALL / CALL ▶ | |
| JOG TOUCH | JOG TOUCH | TOUCH SW | JOG FL pattern 3 |
| AUTO BEAT LOOP 1/2/4/8 | same | A LOOP 1/2/4/8 | |
| BEAT SELECT | — | B SELECT | |
| DIRECTION (REV) | REV | REV | |
| USB STOP | USB access | USB STOP | |
| PC / USB / DISC | same | LINK / USB / DISC | |
| BROWSE / TAG LIST / INFO / MENU | same | same | |
| BACK, TAG TRACK | — | — | page through the BROWSE FL test patterns (lattice, all off, all on); DATA FL goes dark except in all-on |
| Rotary push | — | ENC PUSH | |
| Rotary turn | — | ENCODER bar ±1 (max 8) | |
| Jog turn | — | JOG bar, 8 steps per revolution | |
| Tempo slider | — | bar on the playing-address (■) | moving to − grows it |
| VINYL SPEED ADJUST | — | VOLUME bar, 8 steps | full CW → jog load mode |

The display-integration work (branch `codex/display-integration`,
`docs/cdj850-display-handoff.md`) notes an "all-on service mode" display
legend (2295). That matches the all-on test page here.

### 5.2 Jog load measurement (mode 1a, §8.4, printed p.55)

Spin the jog hard clockwise (peak ≥ 7×). The FL shows the measurement
number, the peak speed, the time to decay from 3× to 1.5×, and the average
over 5 spins with an OK/NG verdict. Spec: **170 ± 20 ms**. The adjust
plate has positions −4..+3. Exit with POWER OFF.

### 5.3 CD drive diagnosis (mode 3, §6.3 [5], printed p.40–41)

Two sub-modes. The first key press selects player mode. MASTER TEMPO
toggles between the two (its LED is lit in test mode).

Player operation mode:

| Key | Action |
|---|---|
| TIME | Servo all off (stop) |
| PLAY/PAUSE | Set up and trace at 4× (no audio). FL shows the address |
| TRACK SEARCH FWD/REV | Track search, then pause |
| CUE | Error-rate count, about 10 s, e.g. `3.56E-4 O.K.` (< 3.00E-3 is OK, otherwise NG) |
| EJECT | Eject |
| MASTER TEMPO | Switch to test mode |

Inserting a disc does **not** auto-start setup in this mode.

Test operation mode (FL text in quotes):

| Key | Action |
|---|---|
| TIME | Servo all off "ALL OFF" |
| TEMPO | LD on/off "LD ON"/"LD OFF" |
| RELOOP | Focus on (LD on + auto focus) "FCS ON" |
| IN/CUE | Spindle kick + auto-adjust + tracking on, or off if on "TRK ON"/"TRK OFF" |
| OUT | Tracking off |
| SEARCH FWD / REV | Tracking off, sled ±2 mm "SLD FWD"/"SLD REV" |
| UTILITY (DISPLAY) | LD on, pickup up/down sweep, no focus close "PU UP/DN" |
| MASTER TEMPO | Back to player mode |

Bring-up order: Servo All Off → Focus On → Spindle Kick/Tracking On.
§5.3 (printed p.32) uses these for pickup checks: MASTER TEMPO then MENU
sends the S-curve command; MASTER TEMPO, TEMPO, RELOOP, LOOP IN×2 for
TE level. Reference levels: LD current 65 mA typ / 75 mA max (measured
across LD3S–LDCHK ÷ 5.5 Ω), S-curve 1.7 Vp-p, TE 1.0 Vp-p, RFO 1.1 Vp-p,
AGC RF 1.2 Vp-p ±10 %, focus coil 3.7 ± 0.55 Ω, tracking coil 4.3 ± 0.65 Ω.

This mode is the cleanest test bench for the servo model (`dsp56371.c`
SRVBUS handling, `xc/loader.c`). Each key maps to exactly one servo command,
so a trace of SRVBUS traffic per key would pin the command set down.

---

## 6. Firmware update and recovery (§8.3, printed p.53–54)

| Device | File | Notes |
|---|---|---|
| MAIN CPU (only updatable device) | `C850MAIN.UPD` | No version in the file name. The DSP program and everything else travel inside it. There is no separate panel-CPU or DSP image |

**From USB** (FAT32 only, HFS+ is not supported):

1. Copy `C850MAIN.UPD` to the stick's root.
2. Power on holding **SOURCE SELECT/USB + RELOOP**, and keep holding until
   the caution screen shows.
3. FL: `Connect USB storage device into top USB port.`
4. Insert the stick and the update starts: `NOW UPDATING` with a progress
   bar, plus the current and the new version.
5. `COMPLETE! Turn the power off`, then power cycle.

**From CD-R/RW:** same, with **SOURCE SELECT/DISC + RELOOP**. FL:
`Insert CD-ROM disc`. The rest is the same as USB.

**Recovery:**

- Losing power mid-update leaves the unit unable to run normally.
- If the unit shows **E-7024**, running the same USB procedure again
  enters a *recovery mode* that performs the update.
- **Recovery works from USB only. CD-ROM cannot be used.**

What this implies for the emulator (inferences, not manual text):

- The updater and the recovery path must live in a flash region that a
  normal update does not erase: a boot/loader block apart from the program
  area ("program area" is the manual's own wording for modes 4/5). E-7024
  is raised when that loader finds the program area invalid. So the boot
  block has its own USB-mass-storage + FAT32 reader, which is why recovery
  is USB-only (no CD servo stack in the loader).
- A good emulator acceptance test is to boot into mode 4 with a virtual
  FAT32 stick holding `C850MAIN.UPD` and check that the flash image is
  rewritten. Next, corrupt the program area, check for E-7024, and recover.
  Do this on a throwaway flash image only. Never commit `C850MAIN.UPD` or
  anything derived from it.
- The version and error-history screen (mode 2: RELOOP held 10 s) is
  the quickest way to read the running firmware version inside the
  emulator.

---

## 7. USB and PC (§5.4, printed p.33; §10.15)

- USB-A is the host port for sticks. The 5 V/500 mA VBUS is switched by
  IC2003 (USB_VBUSON H = on). USB_CDETFLG goes L on overcurrent.
- USB-B is the device port to a PC. On Windows it enumerates as a **USB
  Composite Device** with **USB Audio Device** + **HID-compliant device /
  USB Human Interface Device**, no driver needed. FL shows `CONNECTED`.
  Entered with the PC button. If DISC/USB is in use, a caution appears and
  you press PC again or push the rotary.
- Both ports share the SH7263's single full-speed USB controller through
  IC107: USB_ABSEL H = USB-A, L = USB-B. VBUS (pin 102) H = PC attached.

---

## 8. Signal levels quick table (§10.15, printed p.93)

| Signal | High | Low |
|---|---|---|
| PWR_SW | 3.3 V switch OFF | 0 V switch ON |
| PWR_ON | power on | power off |
| PWR_XON (CMPX) | power off | power on |
| AMUTE | 12 V muted | released |
| MAIN_XMUTE | released | muted |
| USB_VBUSON | VBUS on | off |
| USB_CDETFLG | normal | overcurrent |
| USB_ABSEL | USB-A | USB-B |
| VBUS | PC connected | not connected |
| INSW (INSIDE) | not at innermost | at innermost track |
| MAIN_XRST / DSPRST / SRVRST / PNL_XRST | reset released | in reset |
| DRVMUTE1,2 | after disc inserted | no disc |
| SPIN | 1.65 V centre, ~3 V during spin-up | |
| Digital keys, RENC_SW | not pressed | pressed |
| JOG_SW | not touched | touched |
| ADCT | 1.65 V constant | |
| ADIN | 3.3 V (+ side) … 0 V (− side) | |
| TCH/REL | 3.3 V (CW) … 0 V (CCW) | |
| LPS1/LPS2 | L when the loader switch is ON. Waveforms 11/12: disc in = LPS1 L→H, LPS2 H→L; eject reverses, with a short glitch | |

---

## 9. Waveforms and timing (§10.14, printed p.91–92)

Values read off the scope grids, roughly ±20 %. Treat them as order of
magnitude only.

| # | Point | What it shows |
|---|---|---|
| D1 | IC103 pin 50 CLK_66M | 66 MHz |
| D2 | IC301 pin 45 CLK_A_16M | 16.9344 MHz |
| D3 | IC201 pin 73 CLK_S_16M | 16.9344 MHz |
| D4 | IC103 pin 85 SH_STATUS | square wave ≈ 1 s high / 1 s low (firmware heartbeat) |
| D5 | IC103 pin 72 DSPDREQ | single ≈ 40 µs low pulse (20 µs/div) |
| D6 | IC103 pin 126 SRVSCLK | continuous clock, low MHz |
| D7 | PNL_XBUSY (R162) | one high pulse ≈ 0.6–0.7 ms per exchange |
| D8 | TSCK (R191) | short clock bursts every ≈ 0.8 ms |
| D9/D10 | TSI / TSO | byte bursts aligned with TSCK |
| D11/D12 | LPS1/LPS2 | see §8 |
| D13/D14 | RFO / AGCRF | RF eye ≈ 1.2 Vp-p |
| G1–G4 | DATA FL D_SCLK/D_BK/D_LAT/D_DSO | bursts and latch every ≈ 0.19 ms |
| G5/G6 | JOG1/JOG2 | quadrature while scratching, 10 ms/div |
| J1–J4 | JOG FL J_SCLK/J_BK/J_LAT/J_DSO | bursts every ≈ 0.8 ms, J_BK square |

Panel link takeaway: the panel CPU clocks short TSCK bursts on a cadence of
about 1 ms and frames each exchange with a PNL_XBUSY pulse.
`xc/panel_peer.c` already uses D7–D10.

---

## 10. Power rails worth knowing (§4.3, §5.2, §5.5)

Standby: V+12_EUP, V+3R3_EUP (panel CPU only). MAIN: V+3R3, V+1R2 (core),
V+3R3_B. Audio: V+3R3_A, V+1R25 (DSP), V+10_A / V-10_A, V+5_A. Servo/mech:
V+7R6_M, V+5R6_M, VREF1R65 (made by IC201), V+3R3_S. FL: V+6R6_FL/F±, V+38
(DATA FL), VFDP2R7_F1/F2, V+27 (JOG FL). The VDET monitor covers V+7R6_M,
V+12, V+5R6, V+1R2, V+1R25, V+3R3_S, V-10_A, V+3R3_SW, V+6R6_FL and V+38.

---

## 11. Where to look in the PDF

| Need | Printed page (PDF = +4) |
|---|---|
| Spec sheet | 7 |
| Panel facilities (front panel labels) | 10–12 |
| PCB locations / assy list | 14 |
| Wiring, signal block diagrams | 16–19 |
| Power block diagram | 20–21 |
| Power-on sequence | 22 |
| Troubleshooting by symptom | 23–31 |
| Pickup diagnosis | 32 |
| PC connection check | 33 |
| Power monitor, protectors | 34 |
| Service modes | 35–41 |
| User settings, firmware update, jog load | 52–55 |
| MAIN schematic (CPU / servo / DSP+DAC / power) | 68–75 |
| CMPX, USBA | 76–79 |
| DFLB (panel CPU, DATA FL) | 80–81 |
| KSW1/2, JFLB/JOGB, SLD/PSWB | 82–87 |
| Voltages, waveforms, signal levels | 90–93 |
| PCB layouts | 94–117 |
| PCB parts list | 118–122 |

**Open questions the manual cannot answer:** panel-link frame format and
key codes; the servo command set on SRVBUS; the flash layout of the
boot/loader block vs. the program area; where in flash the UTILITY settings
live. All of these need the firmware or a hardware capture.
