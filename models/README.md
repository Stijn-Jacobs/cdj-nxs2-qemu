# Model profiles

One file per supported player. The firmware and launch scripts read the
profile named by `CDJ_MODEL` (or `--model`), so they carry no model specifics
of their own. The default is `cdj2000nxs2`.

| profile | player | update | state |
|---|---|---|---|
| `cdj2000nxs2.conf` | CDJ-2000NXS2 | v1.87, `C2KNXS2.UPD` | plays |
| `cdj2000nxs.conf` | CDJ-2000NXS | v1.40, `C2KNXS.UPD` | bring-up, MAIN only |
| `cdj2000.conf` | CDJ-2000 | v4.33, four `C2K*.UPD` files | bring-up, MAIN only |

The CDJ-2000 and the CDJ-2000NXS are one platform (an older SH-4A MAIN with
its peripherals at `0xFFxxxxxx`, a C672x-class DSP), so both boot the
`cdj2000nxs` machine. The NXS2 is a different platform (SH7724 MAIN, C6655
DSP, SH7269 GUI processor).

## Variables

| variable | meaning |
|---|---|
| `MODEL_TITLE` | the player's name, for messages |
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

## Adding a model

1. Write `models/<id>.conf`. Pin `MODEL_EXPECTED` from a first run of
   `scripts/firmware/prepare_firmware.sh --model <id> <update> <outdir>`.
2. Add its MAIN machine under `hw/cdj/boards/<board>/`, describing the board
   with a `CdjBoardDesc` (`hw/cdj/common/cdj_common.h`), and give it a
   Kconfig entry that selects `CDJ_COMMON`.
3. Boot it with `CDJ_MODEL=<id> scripts/run/boot_main.sh <tag>`; its log lists
   every region the firmware touches that the board does not model yet.
