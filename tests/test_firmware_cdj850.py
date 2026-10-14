# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850: pin the C850MAIN.UPD container profile values (size, hash, label)."""
import hashlib
import pathlib

C850 = pathlib.Path("/Users/julius/Documents/cdj850-emu/C850MAIN.UPD")


def test_c850_header():
    from c850_decode import find_main_label, read_upd
    data = read_upd(str(C850))
    assert len(data) == 2156300
    assert hashlib.sha256(data).hexdigest() == "12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7"
    assert find_main_label(data) == 0x60000
    assert data[0x60000:0x60000+12] == b"CDJ-850 MAIN"


def test_prepare_c850():
    # Real entry is launcher.firmware.prepare (brief's `run_steps` does not
    # exist); call signature mirrors test_prepare_names_the_model_on_a_hash_mismatch.
    from launcher import firmware, model
    m = model.load("cdj850")
    out = firmware.prepare(str(C850), model=m, say=lambda s: None)
    assert pathlib.Path(out, "extract", "main_flat.bin").is_file()


def test_c850_sh_coverage():
    from c850_sh_scan import sh2a_only_counts
    counts = sh2a_only_counts("/Users/julius/Documents/cdj850-emu/C850MAIN.UPD")
    assert counts, "scanner found no candidate SH-2A-only opcodes at all (scanner broken?)"


def test_c850_dsp_decode_synthetic():
    # Hand-built record sample: 3 byte-sends with known expected bytes
    # (synthetic vectors only — never firmware bytes), interleaved with
    # strobe/gate noise that the decoder must exclude.
    import struct
    from c850_dsp_decode import SITES, decode_image
    PEDRL, PEIORL = 0x202, 0x206
    CD0E_STROBE = (0x0C0FCD2C, 0x5080, 0x0C0FCD54, 0x5180)

    def word(hi_nib, clock):
        return ((hi_nib << 12) | 0x080 | (0x100 if clock else 0)) & 0xFFFF

    recs = []
    recs.append((0x0C0FD7CC, 6559, PEDRL, 2, 0x01C0))  # bring-up, ignored
    expected = bytearray()
    for hi, lo in ((0x3, 0xC), (0x0, 0x7), (0xA, 0x5)):
        w = [word(hi, False), word(hi, True), word(hi, False),
             word(lo, False), word(lo, True)]
        for pc, val in zip(SITES, w):
            recs.append((pc, 6811, PEDRL, 2, val))
        expected.append((hi << 4) | lo)
        # CD0E strobe pair + unrelated PEDRL/PEIORL noise between sends.
        recs.append((CD0E_STROBE[0], 6811, PEDRL, 2, CD0E_STROBE[1]))
        recs.append((CD0E_STROBE[2], 6811, PEDRL, 2, CD0E_STROBE[3]))
        recs.append((0x0C0FCA02, 6811, PEIORL, 2, 0xF1C0))
        recs.append((0x0C0FC918, 6811, PEDRL, 2, 0x50C0))
    assert decode_image(recs) == bytes(expected) == b"\x3c\x07\xa5"


def test_c850_unpack_map():
    # Packed-MAIN -> SDRAM decoder map (Task 8d): unpack the prepared
    # main_flat.bin and pin length + SHA (reads the local extract only,
    # never the read-only UPD; byte-matches live SDRAM code/rodata).
    import pathlib
    from c850_unpack import UNPACK_LEN, UNPACK_SHA256, decode_main
    import hashlib
    flat = pathlib.Path("extract/cdj850/main_flat.bin")
    declared, image = decode_main(str(flat))
    assert declared == 0x19E6E6
    assert len(image) == UNPACK_LEN == 0x39231E
    assert hashlib.sha256(image).hexdigest() == UNPACK_SHA256


def test_c850_shi_helpers_synthetic():
    # Synthetic vectors only (never firmware bytes): a well-formed SHI
    # header (count=3, addr=0, JMP vector table + NOP/RTS/REP payload)
    # must pass every stage; a garbage buffer must fail each one.
    from c850_shi_search import (census, exact_fit_offsets, find_all,
                                 framing_at, scan_headers, vector_ok,
                                 words24)
    good = (bytes([0, 0, 16, 0, 0, 0])          # count=16 addr=0
            + bytes([0x0C, 0x00, 0x10,           # JMP-short $010
                     0x00, 0x00, 0x00,           # NOP (slot 1)
                     0x0C, 0x00, 0x20,           # JMP-short $020
                     0x00, 0x00, 0x00,           # NOP
                     0x0C, 0x00, 0x30,           # JMP-short $030
                     0x00, 0x00, 0x00,           # NOP
                     0x0C, 0x00, 0x40,           # JMP-short $040
                     0x00, 0x00, 0x00,           # NOP
                     0x0C, 0x00, 0x50,           # JMP-short $050
                     0x00, 0x00, 0x00,           # NOP
                     0x0C, 0x00, 0x60,           # JMP-short $060
                     0x00, 0x00, 0x00,           # NOP
                     0x0C, 0x00, 0x70,           # JMP-short $070
                     0x00, 0x00, 0x0C,           # RTS
                     0x00, 0x00, 0x00,           # NOP
                     0x00, 0x00, 0x00]))         # NOP
    assert framing_at(good, 0, "BE") == (16, 0)
    assert (0, "BE", 16) in exact_fit_offsets(good)
    payload = words24(good[6:], "BE")
    assert vector_ok(payload)
    marks = census(payload)
    assert marks["nop"] == 8 and marks["jmp"] == 7 and marks["rts"] == 1
    assert scan_headers(good) == [(0, "BE", 16, 0)]
    assert find_all(b"ababa", b"aba") == [0, 2]
    bad = bytes([0x8B, 0xA0, 0x8C, 0x80, 0x90, 0x0D, 0x91, 0x88,
                 0xFF, 0xFF, 0xFF, 0xFF])
    assert framing_at(bad, 0, "BE")[0] != 2  # huge count, not remaining
    assert exact_fit_offsets(bad) == []
    assert not vector_ok(words24(bad, "BE"))
    assert scan_headers(bad) == []


def test_c850_shi_init_image_misfit():
    # Task 8e: the recorded 519-B image (local extract, SHA-pinned) owns
    # no SHI framing (UM App. A) and no DSP563xx code markers in either
    # byte order — mode 5 notwithstanding, nothing carves from it.
    import hashlib
    import pathlib
    from c850_shi_search import (IMAGE_LEN, IMAGE_SHA256, census,
                                 exact_fit_offsets, framing_at, words24)
    img = pathlib.Path("extract/cdj850/dsp_init_image.bin").read_bytes()
    assert len(img) == IMAGE_LEN == 519
    assert hashlib.sha256(img).hexdigest() == IMAGE_SHA256
    assert len(img) // 3 == 173
    for order in ("BE", "LE"):
        count, addr = framing_at(img, 0, order)
        assert (count, addr) != (171, addr) or addr >= 0x1000
        assert count != 171  # offset-0 count must be 171 (0xAB) to fit
        marks = census(words24(img, order))
        assert marks["nop"] == 0 and marks["rts"] == 0
        assert marks["rep"] == 0 and marks["bra_short"] == 0
    assert exact_fit_offsets(img) == []  # no offset fits count+address


def test_c850_shi_negative_search():
    # Rigorous redo: full-length bytes.find of the 519-B image over the
    # packed AND unpacked MAIN finds nothing (all offsets, not first-hit).
    import pathlib
    from c850_shi_search import IMAGE_SHA256, find_all
    from c850_unpack import UNPACK_SHA256, decode_main
    import hashlib
    img = pathlib.Path("extract/cdj850/dsp_init_image.bin").read_bytes()
    assert hashlib.sha256(img).hexdigest() == IMAGE_SHA256
    packed = pathlib.Path("extract/cdj850/main_flat.bin").read_bytes()
    _declared, unpacked = decode_main("extract/cdj850/main_flat.bin")
    assert hashlib.sha256(unpacked).hexdigest() == UNPACK_SHA256
    assert find_all(packed, img) == []
    assert find_all(unpacked, img) == []


def test_c850_shi_header_scan_pins():
    # Header-scan class pins on the verified unpacked image: structural
    # candidates exist (SH code/table aliasing) but none is a genuine
    # program — no survivor combines a distinct-handler vector table
    # with subroutine structure (RTS), a valid REP loop, and a nonzero
    # reset jump. Representatives of every survivor class disassemble
    # as valid SH-2A (see Task 8e report); the packed image's single
    # vector survivor is LZSS-stream coincidence (census-rejected).
    from c850_unpack import decode_main
    from c850_shi_search import (JMP_HI, VECTOR_SLOTS, scan_headers,
                                 vector_ok, words24)
    _declared, unpacked = decode_main("extract/cdj850/main_flat.bin")
    cands = scan_headers(bytes(unpacked))
    assert len(cands) == 4816
    vec = [c for c in cands
           if vector_ok(words24(unpacked[c[0] + 6:c[0] + 42], c[1]))]
    assert len(vec) == 253

    def genuine(off, order, cnt):
        payload = words24(unpacked[off + 6:off + 6 + 3 * cnt], order)
        slots = [payload[s] for s in VECTOR_SLOTS if s < len(payload)]
        targets = set(w & 0xFFF for w in slots if (w >> 16) in JMP_HI)
        rts = payload.count(0x00000C)
        rep = sum(1 for w in payload
                  if (w >> 16) == 0x06 and (w & 0xF0) == 0xA0)
        reset = ((payload[0] >> 16) in JMP_HI and (payload[0] & 0xFFF) != 0)
        return len(targets) >= 4 and rts >= 1 and rep >= 1 and reset

    assert not any(genuine(off, order, cnt) for off, order, cnt, _ in vec)
    packed = open("extract/cdj850/main_flat.bin", "rb").read()
    pcands = scan_headers(packed)
    assert len(pcands) == 88
    pvec = [c for c in pcands
            if vector_ok(words24(packed[c[0] + 6:c[0] + 42], c[1]))]
    assert len(pvec) == 1  # LZSS-stream coincidence (census-rejected)


def test_c850_spi_synthetic():
    # Synthetic vectors only (never firmware bytes): PE8 rising edges
    # sample one data bit per clock, MSB-first. Pattern 10100101 must
    # assemble to 0xA5 on PE15 and to 0x00 on PE12 (which stays 0).
    from c850_spi_decode import (bits_to_bytes_msb, nibble_bits, rise_bits)
    ped = [(0x0C0FCC86, 6811, 0x01C0)]  # anchor, PE8=1 (not a rise source)
    for bit in (1, 0, 1, 0, 0, 1, 0, 1):
        setup = (0x8080 if bit else 0x0080)
        strobe = setup | 0x0100
        ped.append((0x0C0FCC86, 6811, setup))    # PE8=0
        ped.append((0x0C0FCCB2, 6811, strobe))   # PE8=1 rising edge
    assert rise_bits(ped, 8, 15) == [1, 0, 1, 0, 0, 1, 0, 1]
    assert bits_to_bytes_msb(rise_bits(ped, 8, 15)) == b"\xa5"
    assert bits_to_bytes_msb(rise_bits(ped, 8, 12)) == b"\x00"
    # Nibble-parallel: nibbles 0x8 (1000) then 0xB (1011) in 15-12
    # order must give bits 1,0,0,0,1,0,1,1 = 0x8B.
    ped2 = [(0x0C0FCC86, 6811, 0x01C0)]
    for hi in (0x8, 0xB):
        base = (hi << 12) | 0x0080
        ped2.append((0x0C0FCC86, 6811, base))
        ped2.append((0x0C0FCCB2, 6811, base | 0x0100))
    assert nibble_bits(ped2, (15, 14, 13, 12)) == [1, 0, 0, 0, 1, 0, 1, 1]
    assert bits_to_bytes_msb(nibble_bits(ped2, (15, 14, 13, 12))) == b"\x8b"
    # A single garbage byte owns no SHI framing (too short for a header).
    from c850_shi_search import exact_fit_offsets
    assert exact_fit_offsets(b"\xa5") == []


def test_c850_spi_record_pins():
    # Pins on the existing Task 8c record (/tmp, untracked): total PEDRL,
    # PE8-rise counts, upload window, and the nibble-collapse proof.
    # Skips when the record file is absent (re-record env in 8c notes).
    import hashlib
    import pathlib
    import pytest
    rec = pathlib.Path("/tmp/8c-record20.bin")
    if not rec.is_file():
        pytest.skip("no DSP record file (see Task 8c notes for re-record env)")
    from c850_shi_search import IMAGE_SHA256, census, exact_fit_offsets, words24
    from c850_spi_decode import (DATA_BITS, NIBBLE_MSB_ORDERS,
                                 analyze_stream, bits_to_bytes_msb,
                                 nibble_bits, read_pedrl, rise_bits,
                                 upload_window)
    ped = read_pedrl(str(rec))
    assert len(ped) == 75465
    assert all(ped[i][1] <= ped[i + 1][1] for i in range(len(ped) - 1))
    n_full = sum(1 for i in range(1, len(ped))
                 if ((ped[i - 1][2] >> 8) & 1) == 0
                 and ((ped[i][2] >> 8) & 1) == 1)
    assert n_full == 25030
    up = upload_window(ped)
    assert len(up) == 5174
    n_up = sum(1 for i in range(1, len(up))
               if ((up[i - 1][2] >> 8) & 1) == 0
               and ((up[i][2] >> 8) & 1) == 1)
    assert n_up == 1600
    # Nibble-parallel byte-only 15-12 collapses to the pinned 519-B
    # control image (Task 8c/8e SHA): the "bitstream" adds nothing new.
    nib = bits_to_bytes_msb(
        nibble_bits(up, NIBBLE_MSB_ORDERS["15-12"], byte_only=True))
    assert len(nib) == 519
    assert hashlib.sha256(nib).hexdigest() == IMAGE_SHA256
    # Every serial/nibble upload stream misfits: no exact fit at any
    # offset in either order, census at noise level (<=4 singletons).
    checked = 0
    for data_bit in DATA_BITS:
        for byte_only in (False, True):
            bts = bits_to_bytes_msb(
                rise_bits(up, 8, data_bit, byte_only=byte_only))
            if not bts:
                continue
            assert exact_fit_offsets(bts) == []
            for order in ("BE", "LE"):
                marks = census(words24(bts, order))
                assert marks["total"] <= 4
            checked += 1
    for order_name, order in NIBBLE_MSB_ORDERS.items():
        for byte_only in (False, True):
            bts = bits_to_bytes_msb(nibble_bits(up, order, byte_only=byte_only))
            assert exact_fit_offsets(bts) == []
            for bo in ("BE", "LE"):
                marks = census(words24(bts, bo))
                assert marks["total"] <= 8
            checked += 1
    assert checked == 12
    # Full-record nibble stream is idle-dominated (reply-spin 0x11).
    full = bits_to_bytes_msb(nibble_bits(ped, NIBBLE_MSB_ORDERS["15-12"]))
    assert len(full) == 12515
    assert full.count(0x11) / len(full) > 0.93
