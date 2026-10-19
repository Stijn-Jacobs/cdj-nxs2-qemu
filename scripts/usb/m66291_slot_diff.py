#!/usr/bin/env python3
"""Compare the USB audio slot-write logs of two emulator runs (stock vs candidate).

  m66291_slot_diff.py STOCK.log CANDIDATE.log     PASS/FAIL, first divergence
  m66291_slot_diff.py LOG --pattern counter|sine --alt 1|2
                                                  check one log against the
                                                  host client's stream
  options: --strict          identical transactions, slot indices included
           --skip-tolerance N  extra skipped packets the candidate may have
           --show N          print the first N transactions of the (first) log

Log format (written by the emulator with CDJ_M66291_LOG=<path>; one event per
line, "t=<virtual ms> sof=<SOF count> " then):
  ISO frame=<n> len=<bytes> sum=<CRC-32>   a host packet landed in EP1's FIFO
  ISODROP frame=<n> len=<bytes>            EP1 was not armed: packet dropped
  SLOTW s=<0|1> off=<hex> val=<hex>        MAIN's 32-bit HPI write to DSP slot s
  SLOTR s=<0|1> off=<hex> val=<hex>        MAIN's HPI read (doorbell busy check)
Slot s is the DSP memory at 0x11837bc0 + s * 0x830. Only lines after the deck
started its USB audio (first SOF enabled) are logged. Slot layout, as the
stock driver writes it: +0x30.. payload, +0x2c length, +0x04 cfg2 (sample rate
low 16 bits), +0x08 cfg3 (0x10 = 16 bit, 0x18 = 24 bit), then +0x00 := 1
(doorbell), after which the driver flips to the other slot.

A transaction is the SLOTW lines up to and including the doorbell write; its
payload is checked against the ISO lines (the packets the chip delivered). The
stock path does not forward every delivered packet (when the DSP has not yet
consumed the slot, and in a few other timing dependent cases it skips or
reuses a slot), so the default comparison is semantic: both runs must have been
fed the same delivered stream (run the host client with --lossless), every
transaction of each must carry a delivered packet, in order, with the same
header (length, cfg2, cfg3, doorbell value) in both runs, and the candidate may
skip at most --skip-tolerance (default 2% of the packets, minimum 3) more
packets than stock. --strict instead requires identical transactions, slot
indices included. Times, SOF numbers, SLOTR values and ISODROP counts are
reported but never compared.
Exit status 0 PASS, 1 FAIL, 2 usage or empty log.
"""
import argparse, os, re, struct, sys, zlib

LINE = re.compile(r"^t=\d+ sof=\d+ (\w+)(?: (.*))?$")


def parse(path):
    tx, cur, stats = [], {}, {"ISO": 0, "ISODROP": 0, "SLOTR": 0}
    delivered = []
    for ln in open(path):
        m = LINE.match(ln.strip())
        if not m:
            continue
        kind, rest = m.group(1), m.group(2) or ""
        if kind in stats:
            stats[kind] += 1
        if kind == "ISO":
            delivered.append(int(dict(x.split("=") for x in rest.split())["sum"], 16))
        if kind != "SLOTW":
            continue
        f = dict(x.split("=") for x in rest.split())
        s, off, val = int(f["s"]), int(f["off"], 16), int(f["val"], 16)
        cur.setdefault("slot", s)
        cur[off] = val
        cur.setdefault("order", []).append(off)
        if off == 0:                       # doorbell
            tx.append(cur)
            cur = {}
    if cur:
        tx.append(dict(cur, incomplete=True))
    stats["delivered"] = delivered
    return tx, stats


def payload(t):
    n = t.get(0x2c, 0)
    out = bytearray()
    for o in range(0x30, 0x30 + ((n + 3) & ~3), 4):
        out += struct.pack("<I", t.get(o, 0))
    return bytes(out[:n])


def summary(t):
    return {"slot": t.get("slot"), "len": t.get(0x2c), "cfg2": t.get(4), "cfg3": t.get(8),
            "doorbell": t.get(0), "payload": payload(t)}


def fmt(v):
    return ("%s..(%d B)" % (v[:8].hex(), len(v))) if isinstance(v, bytes) else (hex(v) if isinstance(v, int) else v)


def match(tx, delivered):
    """Index in `delivered` of each transaction's payload (None if it is not a
    later delivered packet), and the delivered indices no transaction used."""
    idx, k = [], 0
    for t in tx:
        c = zlib.crc32(summary(t)["payload"])
        j = next((j for j in range(k, len(delivered)) if delivered[j] == c), None)
        idx.append(j)
        if j is not None:
            k = j + 1
    used = set(j for j in idx if j is not None)
    return idx, [j for j in range(len(delivered)) if j not in used and j < (max(used) + 1 if used else 0)]


def compare(a, b, strict, tol):
    (ta, sa), (tb, sb) = a, b
    if strict:
        for i in range(max(len(ta), len(tb))):
            if i >= len(ta) or i >= len(tb):
                return "transaction count differs: %d vs %d (first extra is #%d)" % (len(ta), len(tb), i)
            x, y = summary(ta[i]), summary(tb[i])
            for k in x:
                if x[k] != y[k]:
                    if k == "payload":
                        j = next((j for j in range(min(len(x[k]), len(y[k]))) if x[k][j] != y[k][j]), 0)
                        return "transaction #%d payload differs at byte %d: %s vs %s" % (i, j, x[k][j:j + 4].hex(), y[k][j:j + 4].hex())
                    return "transaction #%d %s differs: %s vs %s" % (i, k, fmt(x[k]), fmt(y[k]))
        return None
    da, db = sa["delivered"], sb["delivered"]
    for i in range(max(len(da), len(db))):
        if i >= len(da) or i >= len(db) or da[i] != db[i]:
            return "the runs were fed different streams: delivered packet #%d differs (use the same host client arguments, with --lossless)" % i
    ia, skip_a = match(ta, da)
    ib, skip_b = match(tb, db)
    for name, idx in (("stock", ia), ("candidate", ib)):
        if None in idx:
            return "%s transaction #%d carries no delivered packet in order (corrupt or reordered payload)" % (name, idx.index(None))
    hdr = lambda tx: {(summary(t)["len"], summary(t)["cfg2"], summary(t)["cfg3"], summary(t)["doorbell"]) for t in tx}
    if hdr(ta) != hdr(tb):
        h = lambda x: sorted(hdr(x))
        return "slot headers differ (len,cfg2,cfg3,doorbell): stock %s vs candidate %s" % (
            [tuple(hex(v) for v in x) for x in h(ta)], [tuple(hex(v) for v in x) for x in h(tb)])
    print("skipped packets: stock %d, candidate %d" % (len(skip_a), len(skip_b)))
    if len(skip_b) > len(skip_a) + tol:
        first = next(j for j in skip_b if j not in set(skip_a))
        return "candidate skipped %d packets, stock %d (tolerance %d); first extra skip is delivered packet #%d" % (
            len(skip_b), len(skip_a), tol, first)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--pattern", choices=("counter", "sine"))
    ap.add_argument("--alt", type=int, choices=(1, 2), default=1)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--skip-tolerance", type=int)
    ap.add_argument("--show", type=int, default=0)
    a = ap.parse_args()
    if len(a.logs) > 2 or (len(a.logs) == 2) == bool(a.pattern):
        sys.exit("give two logs, or one log with --pattern")
    runs = [parse(p) for p in a.logs]
    for p, (tx, st) in zip(a.logs, runs):
        print("%s: %d transactions, %d ISO, %d ISODROP, %d SLOTR" % (p, len(tx), st["ISO"], st["ISODROP"], st["SLOTR"]))
        if not tx:
            print("FAIL: no slot writes (audio never started?)")
            sys.exit(2)
    for t in runs[0][0][:a.show]:
        print("  ", {k: fmt(v) for k, v in summary(t).items()})
    if a.pattern:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from m66291_host import pattern_bytes
        tx = runs[0][0]
        k, slot, why, skipped, repeats = 0, None, None, [], []
        for i, t in enumerate(tx):
            s = summary(t)
            n = s["len"]
            # the stock path forwards most but not all delivered packets: allow gaps
            j = next((j for j in range(k, k + 16) if pattern_bytes(a.pattern, a.alt, j * n, n) == s["payload"]), None)
            if j is None:
                why = "transaction #%d payload is not stream packet %d..%d" % (i, k, k + 15)
                break
            skipped += list(range(k, j))
            if slot is not None and s["slot"] != slot ^ 1:
                repeats.append(i)
            if s["doorbell"] != 1:
                why = "transaction #%d doorbell value %s" % (i, fmt(s["doorbell"]))
                break
            slot, k = s["slot"], j + 1
        if skipped:
            print("stream packets with no slot write: %s" % skipped)
        if repeats:
            print("transactions that reused the previous slot: %s" % repeats)
        print("FAIL: " + why if why else "PASS: %d transactions are %s stream packets in order (%d skipped, %d slot reuses), doorbell 1"
              % (len(tx), a.pattern, len(skipped), len(repeats)))
        sys.exit(1 if why else 0)
    tol = a.skip_tolerance if a.skip_tolerance is not None else max(3, len(runs[0][1]["delivered"]) // 50)
    why = compare(runs[0], runs[1], a.strict, tol)
    print("FAIL: " + why if why else "PASS: %d vs %d transactions, same stream and headers" % (len(runs[0][0]), len(runs[1][0])))
    sys.exit(1 if why else 0)


if __name__ == "__main__":
    main()
