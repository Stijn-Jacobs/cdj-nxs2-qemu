# GUI runtimes must own their sleep wake-up (Embassy wake model)

Since the Embassy wake model (`codex/embassy-wake-model`, 58a6dc8) the BF531
model follows the hardware: `SIC_IWR` selects which SIC sources wake the core
from `IDLE`, even when masked in `SIC_IMASK`; and the architectural `CYCLES`
counter and the core timer stop while the core is idle (63959e2).

Stock firmware never programs `SIC_IWR` (reset value all ones) and leaves the
PWM `TIMER0` interrupt latched behind `SIC_IMASK` forever, so on silicon and
on this model the stock GUI core never sleeps. The pre-Embassy model slept
anyway, which hid two things:

- The owned mods GUI runtime (`crates/cdj-gui-rt`, NORTi shim) idles with
  `IDLE; RTS` and arms its alarms on the core timer only. With the accurate
  model its executor spins (no sleep: 6x less guest time per wall second on
  the Linux gate host, 2.6x on the Mac; MAIN never reaches storage init on
  Linux, boot-root FAIL). Making it sleep with a `SIC_IWR` policy alone
  (f6191d80: `IWR = IMASK`; c7e4fb95: all but TIMER0) stalls it instead,
  because no SCLK source wakes it for its 1 ms / 3 ms timers: Linux and Mac
  both show MAIN healthy (TMU1 184k/122k, ATAPI 13.6k/11.7k, USBH 2.2k/2.1k)
  while the GUI exchanges only 228/204 link records in 150 s and never
  reaches ROOT.
- A GUI that bounds its sleep with an SCLK watchdog countdown and sets
  `SIC_IWR` explicitly boots to ROOT in 7.4 s (Linux) / 10.3 s (Mac), loads at
  35.5 s / 41.9 s and plays at realtime 1.00 / 1.10 with 8032 / 6297 link records.

Requirement: a GUI firmware run on this emulator needs an SCLK-based wake for
its deadlines (watchdog or GP timer) and an explicit `SIC_IWR`; relying on the
core timer across `IDLE` is not supported by the hardware or the model.

Measured with `scripts/test/qualify_sd.py --source usb` (boot, ROOT, load,
20 s play) and the mods `boot_root.py` gate, 2026-10-16/17.
