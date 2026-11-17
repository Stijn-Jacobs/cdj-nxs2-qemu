#!/usr/bin/env python3
"""A USB host for the CDJ-2000NXS's M66291 chip model (CDJ_M66291_HOST=<port>).

  scripts/usb/m66291_host.py PORT [--service] [--secs N]
  scripts/usb/m66291_host.py PORT --audio FRAMES [--alt 1|2] [--rate HZ]
        [--pattern counter|sine] [--lossless] [--wait-audio SECS]

Attaches (Vbus), resets, enumerates (device/config/HID report descriptors,
SET_ADDRESS, SET_CONFIGURATION), sends a HID OUT report on EP4, and reads
HID IN (EP5) and MIDI IN (EP3) for a few seconds. --service opens a $CDJ
service session over HID OUT. Prints a transcript. Datagram protocol: see
hw/cdj/boards/cdj2000/usbf_m66291.c.

--audio FRAMES enumerates, then SET_INTERFACE (iface 1, alt 1 = 16 bit / 200 B
per frame, alt 2 = 24 bit / 300 B) and the UAC SET_CUR sampling frequency on
EP1, waits until the deck has started its USB audio (the chip sees the driver
enable SOF; --wait-audio, the deck must be in PC mode) and streams FRAMES
isochronous packets of a deterministic pattern, one per 1 ms SOF. Byte j of
the stream is the pattern (counter: (j * 131 + 17) & 255; sine: signed
little-endian samples of a 1 kHz tone, same left and right). --lossless keeps
a packet queued until the driver has armed EP1 instead of dropping it, which
makes the stream the deck sees identical from run to run. Prints the chip's
iso counters at the end; exit status 2 if audio never started.
"""
import argparse, socket, struct, sys, time


class Host:
    def __init__(self, port):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.dst = ("127.0.0.1", port)
        self.s.settimeout(10)

    def txn(self, data, want):
        self.s.sendto(data, self.dst)
        while True:
            r = self.s.recvfrom(4096)[0]
            if r[:1] == want:
                return r

    def vbus(self, on, wait=300):
        end = time.time() + wait
        while time.time() < end:
            r = self.txn(b"V" + bytes([on]), b"V")
            if r[1] != 2:
                return r[1]
            time.sleep(1)
        raise SystemExit("deck never configured the chip (INTENB0 still 0)")

    def reset(self):
        self.txn(b"R", b"R")
        time.sleep(0.3)

    def control(self, rt, req, val, idx, ln, out=b""):
        for _ in range(400):
            r = self.txn(b"S" + struct.pack("<BBHHH", rt, req, val, idx, ln) + out, b"S")
            if r[1] != 2:
                return r[1], r[2:]
            time.sleep(0.05)  # the deck is still finishing the last transfer
        return 2, b""

    def out(self, ep, data, tries=200):
        for _ in range(tries):
            r = self.txn(b"O" + bytes([ep]) + data, b"O")
            if r[2] == 0:
                return True
            time.sleep(0.05)
        return False

    def inp(self, ep):
        r = self.txn(b"I" + bytes([ep]), b"I")
        return r[3:] if r[2] == 0 else None

    def dump(self):
        return self.txn(b"Q", b"Q")[1:].decode()


def pattern_bytes(kind, alt, start, n):
    """Bytes start..start+n of the test stream."""
    if kind == "counter":
        return bytes((j * 131 + 17) & 255 for j in range(start, start + n))
    import math
    width = 2 if alt == 1 else 3
    frame = 2 * width
    out = bytearray()
    for j in range(start, start + n):
        i, k = divmod(j, frame)
        ch_byte = k % width
        v = int(0.5 * (1 << (8 * width - 1)) * math.sin(2 * math.pi * 1000 * (i // 1) / 48000))
        out.append(v.to_bytes(width, "little", signed=True)[ch_byte])
    return bytes(out)


def stream_audio(h, a, ifnum):
    ln = 200 if a.alt == 1 else 300
    st, _ = h.control(0x01, 11, a.alt, ifnum, 0)       # SET_INTERFACE
    print("SET_INTERFACE %d alt %d: status=%d" % (ifnum, a.alt, st))
    rate = struct.pack("<I", a.rate)[:3]
    st, _ = h.control(0x22, 1, 0x0100, 0x0001, 3, rate)  # SET_CUR sampling freq, EP1
    print("SET_CUR sample rate %d on EP1: status=%d" % (a.rate, st))
    h.txn(b"L" + bytes([1 if a.lossless else 0]), b"L")
    end = time.time() + a.wait_audio
    sof0 = None
    while time.time() < end:
        q = h.dump()
        sof = int(q.split("sof=")[1].split()[0])
        if sof0 is None:
            sof0 = sof
        if sof > 0:
            break
        time.sleep(0.5)
    else:
        print("audio never started (no SOF from the driver): %s" % h.dump())
        return False
    print("audio started: %s" % h.dump())
    sent = 0
    for i in range(a.audio):
        data = pattern_bytes(a.pattern, a.alt, i * ln, ln)
        while True:
            r = h.txn(b"O\x01" + data, b"O")
            if r[2] == 0:
                break
            time.sleep(0.002)       # queue full: the chip drains one per ms
        sent += 1
    for _ in range(600):            # let the queue drain
        if " iso_pending=0" in h.dump():
            break
        time.sleep(0.05)
    print("streamed %d packets of %d bytes (%s, alt %d); chip: %s" % (sent, ln, a.pattern, a.alt, h.dump()))
    return True


def hexs(b):
    return b.hex(" ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", type=int)
    ap.add_argument("--service", action="store_true")
    ap.add_argument("--secs", type=float, default=5)
    ap.add_argument("--audio", type=int, default=0, help="stream this many 1 ms isochronous frames")
    ap.add_argument("--alt", type=int, default=1, choices=(1, 2))
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--pattern", default="counter", choices=("counter", "sine"))
    ap.add_argument("--lossless", action="store_true")
    ap.add_argument("--wait-audio", type=float, default=120)
    a = ap.parse_args()
    h = Host(a.port)
    print("vbus on ->", h.vbus(1))
    time.sleep(1)
    h.reset()
    print("state:", h.dump())
    st, d = h.control(0x80, 6, 0x0100, 0, 64)
    print("GET_DESCRIPTOR device: status=%d %s" % (st, hexs(d)))
    if st or len(d) < 18:
        sys.exit("device descriptor failed")
    vid, pid = struct.unpack_from("<HH", d, 8)
    print("  VID %04x PID %04x" % (vid, pid))
    st, _ = h.control(0x00, 5, 1, 0, 0)
    print("SET_ADDRESS 1: status=%d" % st)
    st, d = h.control(0x80, 6, 0x0200, 0, 9)
    total = struct.unpack_from("<H", d, 2)[0]
    st, cfg = h.control(0x80, 6, 0x0200, 0, total)
    print("GET_DESCRIPTOR config: status=%d len=%d (wTotalLength %d)" % (st, len(cfg), total))
    print(" ", hexs(cfg))
    ifaces, off = [], 0
    while off + 2 <= len(cfg) and cfg[off]:
        if cfg[off + 1] == 4:
            ifaces.append((cfg[off + 2], cfg[off + 3], cfg[off + 5], cfg[off + 6]))
        if cfg[off + 1] == 0x21:
            hid_len = struct.unpack_from("<H", cfg, off + 7)[0]
        off += cfg[off]
    print("  interfaces (num, alt, class, subclass):", ifaces,
          "distinct:", len({i[0] for i in ifaces}))
    hid_if = [i[0] for i in ifaces if i[2] == 3][0]
    st, rep = h.control(0x81, 6, 0x2200, hid_if, hid_len)
    print("GET_DESCRIPTOR HID report (iface %d): status=%d len=%d %s" % (hid_if, st, len(rep), hexs(rep)))
    st, _ = h.control(0x00, 9, cfg[5], 0, 0)
    print("SET_CONFIGURATION %d: status=%d  state: %s" % (cfg[5], st, h.dump()))
    time.sleep(1)
    if a.audio:
        audio_if = [i[0] for i in ifaces if i[2] == 1 and i[1] == a.alt][0]
        ok = stream_audio(h, a, audio_if)
        print("final:", h.dump())
        sys.exit(0 if ok else 2)
    if a.service:
        msg = b"$CDJ\r"
        pkt = msg.ljust(26, b"\0")
        print("HID OUT ep4 $CDJ:", h.out(4, pkt))
    else:
        pkt = bytes([0x01, 0x02, 0x03, 0x04]).ljust(26, b"\0")
        print("HID OUT ep4:", h.out(4, pkt))
    end = time.time() + a.secs
    while time.time() < end:
        for ep, name in ((5, "HID IN"), (3, "MIDI IN")):
            d = h.inp(ep)
            if d is not None:
                print("%s ep%d: %s | %r" % (name, ep, hexs(d), d))
        time.sleep(0.05)
    print("final:", h.dump())


if __name__ == "__main__":
    main()
