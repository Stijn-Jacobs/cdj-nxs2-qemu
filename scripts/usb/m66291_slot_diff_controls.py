#!/usr/bin/env python3
"""Negative controls for m66291_slot_diff.py: mutate a stock slot log and show
what the default (semantic) comparison rejects and what it lets through.

  m66291_slot_diff_controls.py STOCK.log [WORKDIR]

Each control writes WORKDIR/<name>.log and runs the diff against STOCK.log.
Exit status 0 when every control ends as expected, 1 otherwise.
"""
import os, subprocess, sys, tempfile

DIFF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "m66291_slot_diff.py")


def blocks(lines):
    """Split into (prefix, [transaction lines...], ...): a transaction is the
    SLOTW lines up to and including the doorbell write (off=000)."""
    out, cur = [], []
    for ln in lines:
        cur.append(ln)
        if " SLOTW " in ln and " off=000 " in ln:
            out.append(cur)
            cur = []
    return out, cur


def subst(lines, needle, repl, nth):
    hits = [i for i, ln in enumerate(lines) if needle in ln]
    lines = list(lines)
    i = hits[nth]
    lines[i] = lines[i].replace(needle, repl)
    return lines


def drop(lines, n, first=10):
    tx, tail = blocks(lines)
    # keep each block's ISO lines (the packet was delivered), drop its SLOTW/SLOTR
    for b in tx[first:first + n]:
        b[:] = [ln for ln in b if " ISO" in ln]
    return [ln for b in tx for ln in b] + tail


def dup(lines, at=20):
    tx, tail = blocks(lines)
    tx.insert(at + 1, [ln for ln in tx[at] if " ISO" not in ln])
    return [ln for b in tx for ln in b] + tail


def main():
    stock = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="slotdiff-controls-")
    os.makedirs(work, exist_ok=True)
    lines = open(stock).readlines()
    controls = [
        ("identical", lines, "PASS"),
        ("payload-corrupt", subst(lines, "off=040 val=", "off=040 val=dead", 5), "FAIL"),
        ("cfg2-rate-48k", subst(lines, "off=004 val=0000ac44", "off=004 val=0000bb80", 7), "FAIL"),
        ("cfg3-width-24", subst(lines, "off=008 val=00000010", "off=008 val=00000018", 7), "FAIL"),
        ("length-wrong", subst(lines, "off=02c val=000000c8", "off=02c val=000000c4", 7), "FAIL"),
        # tolerance is max(3, 2% of packets) extra skips over stock's own: these
        # two pin where it ends for this capture
        ("drop-1", drop(lines, 1), "PASS"),
        ("drop-tolerance+1", None, "FAIL"),
        ("duplicate", dup(lines), "FAIL"),
    ]
    n_iso = sum(" ISO " in ln for ln in lines)
    tol = max(3, n_iso * 2 // 100)
    controls[6] = ("drop-%d" % (tol + 1), drop(lines, tol + 1), "FAIL")
    ok = True
    for name, mutated, want in controls:
        path = os.path.join(work, name + ".log")
        open(path, "w").writelines(mutated)
        r = subprocess.run([sys.executable, DIFF, stock, path], capture_output=True, text=True)
        got = "PASS" if r.returncode == 0 else "FAIL" if r.returncode == 1 else "ERROR"
        last = (r.stdout.strip().splitlines() or [""])[-1]
        good = got == want
        ok &= good
        print("%-18s want %s got %s %s | %s" % (name, want, got, "ok" if good else "UNEXPECTED", last))
    print("controls: %s (tolerance %d extra skips, %d delivered packets)" % ("PASS" if ok else "FAIL", tol, n_iso))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
