# Model profiles

One file per supported player. The firmware and launch scripts read the
profile named by `CDJ_MODEL` (or `--model`), so they carry no model specifics
of their own. The default is `cdj2000nxs2`.

| profile | player | update | state |
|---|---|---|---|
| `cdj2000nxs2.conf` | CDJ-2000NXS2 | v1.87, `C2KNXS2.UPD` | plays |
| `xdj1000mk2.conf` | XDJ-1000MK2 | v1.45, `XDJ1KMK2.UPD` | experimental |
| `xdj700.conf` | XDJ-700 | v1.15, `XDJ700.UPD` | experimental |
| `xdj1000.conf` | XDJ-1000 | v1.13, `XDJ1000.UPD` | experimental |
| `cdj900nxs.conf` | CDJ-900NXS | v1.31, `C900NXS.UPD` | experimental |
| `cdj2000nxs.conf` | CDJ-2000NXS | v1.44, `C2KNXS.UPD` | experimental |
| `cdj900.conf` | CDJ-900 | v4.32, four `C900*.UPD` files | experimental |
| `cdj2000.conf` | CDJ-2000 | v4.33, four `C2K*.UPD` files | experimental |

The table lists the default first, then the others newest first by `MODEL_RELEASED`.

The CDJ-2000 and the CDJ-2000NXS are one platform (an older SH-4A MAIN with
its peripherals at `0xFFxxxxxx`, a C672x-class DSP), so both boot the
`cdj2000nxs` machine. The NXS2 is a different platform (SH7724 MAIN, C6655
DSP, SH7269 GUI processor).

## Variables

| variable | meaning |
|---|---|
| `MODEL_TITLE` | the player's name, for messages |
| `MODEL_RELEASED` | `<year>.<n>`: the release year and, where two players share it, the order within it (CDJ-900 after the CDJ-2000). Setup and the launcher list the default player first, then the rest by this, newest first |
| `MODEL_MAIN_MACHINE` | QEMU machine for the MAIN board (`-M`) |
| `MODEL_GUI_MACHINE` | QEMU machine for the GUI board; empty while the model boots MAIN alone |
| `MODEL_EXTRACT` | where the images are installed, relative to the repository |
| `MODEL_FW_VERSION` | the one update version every firmware address is for |
| `MODEL_UPD` | the update file names; several names mean one file per section, in section order |
| `MODEL_UPD_SHA256` | their SHA-256s, in the same order |
| `MODEL_MAIN_SECTION`, `MODEL_GUI_SECTION` | which section holds MAIN and the GUI image |
| `MODEL_MAIN_LZSS` | offset of MAIN's LZSS stream in the S-record image (after its 4-byte length) |
| `MODEL_FW_STEPS` | the `prepare_firmware.sh` steps to run, in order |
| `MODEL_EXPECTED` | each output image and its SHA-256 |
| `MODEL_LAUNCH` | `rig` (default): the NXS2's two-board real-DSP rig, started by `rig.py`. `deck`: one MAIN emulator that draws the screen itself or runs its display board inside it, started by `deck.py`; the DSP warm-up, Pro DJ Link, mods, controller and virtual deck app are skipped for it |
| `MODEL_DISPLAY_UPD` | the update file the display board loads (a `deck` model only); the install keeps a copy as `display.upd` beside the main images |
| `MODEL_DISPLAY_FLASH` | the flash image of a display processor that runs inside the MAIN emulator, named in `MODEL_EXPECTED` and cut from the GUI section by the `srec_flat` step; the deck passes it as `CDJ_M16C_GUI` |
| `MODEL_DSP_IDLE` | a `deck` model with a generated DSP module: the C6747's busy-wait loop as `<head>:<stack_lo>:<stack_hi>`. The machine skips it (`CDJ_C6747_IDLE`), and `build_dsp_module.sh` generates and replays the module against the same head |
| `MODEL_DSP_ISR_FAST` | `1` keeps the DSP's interrupt handlers on the core's fast store paths while the skip is armed (`C66X_IDLE_ISR_FAST`) |
| `MODEL_DSP_GEN_ARGS` | generator options that model's DSP program needs, added to the common set in `build_dsp_module.sh` |
| `MODEL_RIG_ENV` | a `rig` model: `NAME=value` words the rig exports unless the caller already set them (the knobs whose NXS2 addresses do not hold in that model's MAIN) |
| `MODEL_IDLE_S`, `MODEL_LOAD_STEPS` | how a `deck` model reaches its snapshot points (`SNAPSHOT=idle\|loaded`): seconds from power-on to the settled screen, then the `;`-separated steps that load a track (`sendkey <key> <ms>`, `key <panel payload>`, `wait <s>`; see `scripts/run/snapshot_deck.py`) |

## Adding a model

1. Write `models/<id>.conf`. Pin `MODEL_EXPECTED` from a first run of
   `scripts/firmware/prepare_firmware.sh --model <id> <update> <outdir>`.
2. Add its MAIN machine under `hw/cdj/boards/<board>/`, describing the board
   with a `CdjBoardDesc` (`hw/cdj/common/cdj_common.h`), and give it a
   Kconfig entry that selects `CDJ_COMMON`.
3. Boot it with `CDJ_MODEL=<id> scripts/run/boot_main.sh <tag>`; its log lists
   every region the firmware touches that the board does not model yet.
