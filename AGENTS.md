# Project Instructions

## CDJ-850 Reference & Reverse Engineering

When working on CDJ-850 hardware, firmware, or emulator behavior, consult the service manual digest alongside PyGhidra (`pyghidra-mcp`) and SCIF1 captures:

- **Service manual agent reference:** `docs/cdj850-service-manual.md` (`/Users/julius/Documents/cdj-nxs2/docs/cdj850-service-manual.md`), a structured digest of the Pioneer CDJ-850 service manual (`~/Documents/cdj850-emu/f70e4d.pdf`, RRV4128).
- Check `docs/cdj850-service-manual.md` before guessing or re-deriving IC/board mappings, MAIN CPU (`IC103`, SH7263) and Panel CPU (`IC1007`) pin/net maps, user settings in flash, boot/power-on sequence, error codes, service modes, firmware update/recovery, USB switching, signal levels, waveforms/timing, power rails, or PDF page references.
