# CDJ-850 Ghidra annotations

`cdj850-annotations.json` records the hand-authored function names and listing
comments from the CDJ-850 v1.13 analysis project. The SHA-256 hashes identify
the local inputs. The export contains no firmware bytes, Ghidra project files,
or generated disassembly.

1. Prepare your own CDJ-850 v1.13 firmware extracts. See the
   [firmware layout and DSP bootstrap notes](../../docs/cdj850-modding.md).
2. Import the files you want to annotate into a local Ghidra project as raw
   binaries. Use the default compiler spec and set **Base Address** in the
   import options.

   | File | Language | Base address |
   | --- | --- | --- |
   | `boot.bin` | `SuperH:BE:32:SH-2A` | `00000000` |
   | `main_unpacked.bin` | `SuperH:BE:32:SH-2A` | `0c000000` |
   | `dsp_seg*.bin` | `DSP56300:LE:24:default` | `P:000000` |

   DSP imports require the DSP56300 processor extension. Use the payload of
   each SHI stage, omitting the six-byte stage header and storing each
   24-bit word least-significant byte first. Stage 4 has no hand-authored
   entries in this snapshot.
3. Close the project in Ghidra. Use a Python environment with PyGhidra
   installed and point `GHIDRA_INSTALL_DIR` to your Ghidra installation.
   The commands below were tested with Ghidra 12.2.

   ```sh
   export GHIDRA_INSTALL_DIR=path/to/ghidra
   dist="$GHIDRA_INSTALL_DIR/Ghidra/Features/PyGhidra/pypkg/dist"
   python -m pip install --find-links "$dist" "$dist"/pyghidra-*.whl
   python scripts/ghidra/apply_cdj850_annotations.py path/to/cdj850.gpr /main_unpacked.bin
   ```

Run the script once for each program you want to annotate. The second argument
is the program's path inside the Ghidra project. The script checks the imported
file's SHA-256, language, compiler spec and load address before making changes.
It creates missing functions where needed and sets the bundled function names
and comments at their recorded addresses. A rerun applies the same names and
comments. Existing names and comments at those entries can be overwritten.
