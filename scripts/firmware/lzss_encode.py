#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encoder for the LZSS stream lzss_decode.py reads back (see that file for
the confirmed parameters): a 4096-byte ring pre-filled with the space
character, read position starting at N-F=4078, matches of 3-18 bytes stored
as (low 8 bits of the ring position, its high nibble | (length-3)), one flag
bit per item -- least-significant first, eight items per flag byte.

This is the classic Okumura/Yoshizaki LZSS bitstream (public-domain lzss.c,
1989). The decoder doesn't care how a match was chosen, only that offset and
length describe bytes it can actually reproduce, so this encoder is a plain
greedy hash-chain matcher rather than a reimplementation of that reference
tool's own search.

pack_tree() is the reference tool's own search, for when the exact shipped
stream matters (the CDJ-850 repack proves itself by reproducing it).

    usage: lzss_encode.py [--tree] <infile> <outfile>
"""
import sys

RING = 4096
FILL = 0x20
START = RING - 18      # matches GUI_RING_POS / lzss_decode.py's ring_pos
MIN_MATCH = 3
MAX_MATCH = 18
CHAIN_DEPTH = 48


def pack(data):
    """The decompressed bytes, encoded to the stream unpack() reads back."""
    cb = bytes([FILL]) * START + data
    chains = {}

    def key(pos):
        return (cb[pos] << 16) | (cb[pos + 1] << 8) | cb[pos + 2]

    def insert(pos):
        if pos + 3 <= len(cb):
            chains.setdefault(key(pos), []).append(pos)

    for pos in range(START):
        insert(pos)

    out = bytearray()
    code_buf = bytearray(1)    # element 0 is this group's flag byte
    mask = 1

    n = len(data)
    k = 0
    while k < n:
        a = START + k
        limit = min(MAX_MATCH, len(cb) - a)
        best_len, best_pos = 0, 0
        if limit >= MIN_MATCH:
            cands = chains.get(key(a))
            if cands is not None:
                lo = a - RING
                while cands and cands[0] < lo:
                    cands.pop(0)
                checked = 0
                for p in reversed(cands):
                    length = 0
                    while length < limit and cb[p + length] == cb[a + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_pos = length, p
                        if best_len >= limit:
                            break
                    checked += 1
                    if checked >= CHAIN_DEPTH:
                        break

        if best_len >= MIN_MATCH:
            step = best_len
            off = best_pos & (RING - 1)
            code_buf.append(off & 0xFF)
            code_buf.append(((off >> 4) & 0xF0) | (step - MIN_MATCH))
        else:
            step = 1
            code_buf[0] |= mask
            code_buf.append(cb[a])

        for t in range(step):
            insert(a + t)
        k += step
        mask = (mask << 1) & 0xFF
        if mask == 0:
            out += code_buf
            code_buf = bytearray(1)
            mask = 1

    if len(code_buf) > 1:
        out += code_buf
    return bytes(out)


def pack_tree(data):
    """Same bitstream as pack(), but match choice follows the binary-tree
    search of Okumura's 1989 lzss.c step for step (InsertNode/DeleteNode,
    ties kept by the newest node, look-ahead clipped at end of input). Pioneer
    built the CDJ-850's C850MAIN.UPD with that tool, so this reproduces the
    shipped stream byte for byte; pack() only promises a stream that decodes
    to the same bytes."""
    N, F, THRESHOLD = RING, MAX_MATCH, MIN_MATCH - 1
    NIL = N
    text = bytearray([FILL]) * (N + F - 1)
    lson = [NIL] * (N + 1)
    rson = [NIL] * (N + 257)
    dad = [NIL] * (N + 1)
    state = [0, 0]                       # match_position, match_length

    def insert(r):
        cmp = 1
        p = N + 1 + text[r]
        rson[r] = lson[r] = NIL
        state[1] = 0
        while True:
            if cmp >= 0:
                if rson[p] != NIL:
                    p = rson[p]
                else:
                    rson[p] = r
                    dad[r] = p
                    return
            else:
                if lson[p] != NIL:
                    p = lson[p]
                else:
                    lson[p] = r
                    dad[r] = p
                    return
            i = 1
            while i < F:
                cmp = text[r + i] - text[p + i]
                if cmp:
                    break
                i += 1
            if i > state[1]:
                state[0] = p
                state[1] = i
                if i >= F:
                    break
        dad[r] = dad[p]
        lson[r] = lson[p]
        rson[r] = rson[p]
        dad[lson[p]] = r
        dad[rson[p]] = r
        if rson[dad[p]] == p:
            rson[dad[p]] = r
        else:
            lson[dad[p]] = r
        dad[p] = NIL

    def delete(p):
        if dad[p] == NIL:
            return
        if rson[p] == NIL:
            q = lson[p]
        elif lson[p] == NIL:
            q = rson[p]
        else:
            q = lson[p]
            if rson[q] != NIL:
                while rson[q] != NIL:
                    q = rson[q]
                rson[dad[q]] = lson[q]
                dad[lson[q]] = dad[q]
                lson[q] = lson[p]
                dad[lson[p]] = q
            rson[q] = rson[p]
            dad[rson[p]] = q
        dad[q] = dad[p]
        if rson[dad[p]] == p:
            rson[dad[p]] = q
        else:
            lson[dad[p]] = q
        dad[p] = NIL

    out = bytearray()
    code_buf = bytearray(1)
    mask = 1
    s, r = 0, N - F
    src = 0
    n = len(data)
    length = 0
    while length < F and src < n:
        text[r + length] = data[src]
        src += 1
        length += 1
    if length == 0:
        return b''
    for i in range(1, F + 1):
        insert(r - i)
    insert(r)
    while length > 0:
        if state[1] > length:
            state[1] = length
        if state[1] <= THRESHOLD:
            state[1] = 1
            code_buf[0] |= mask
            code_buf.append(text[r])
        else:
            pos = state[0]
            code_buf.append(pos & 0xFF)
            code_buf.append(((pos >> 4) & 0xF0) | (state[1] - (THRESHOLD + 1)))
        mask = (mask << 1) & 0xFF
        if mask == 0:
            out += code_buf
            code_buf = bytearray(1)
            mask = 1
        last = state[1]
        i = 0
        while i < last and src < n:
            c = data[src]
            src += 1
            delete(s)
            text[s] = c
            if s < F - 1:
                text[s + N] = c
            s = (s + 1) & (N - 1)
            r = (r + 1) & (N - 1)
            insert(r)
            i += 1
        while i < last:
            i += 1
            delete(s)
            s = (s + 1) & (N - 1)
            r = (r + 1) & (N - 1)
            length -= 1
            if length:
                insert(r)
    if len(code_buf) > 1:
        out += code_buf
    return bytes(out)


def main():
    args = sys.argv[1:]
    tree = '--tree' in args
    args = [a for a in args if a != '--tree']
    if len(args) != 2:
        raise SystemExit(__doc__)
    sys.argv[1:] = args
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    out = pack_tree(data) if tree else pack(data)
    with open(sys.argv[2], 'wb') as f:
        f.write(out)
    print('lzss_encode: %d -> %d bytes' % (len(data), len(out)))


if __name__ == '__main__':
    main()
