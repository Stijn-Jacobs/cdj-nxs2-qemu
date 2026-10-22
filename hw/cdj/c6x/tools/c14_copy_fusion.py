# SPDX-License-Identifier: GPL-2.0-or-later
"""Recognize the complete mature two-cycle copy pipeline, without PC matching."""
import re


def canonical(code):
    names = {}
    code = re.sub(r'(?<![.\w])t\d+\b', lambda m: names.setdefault(m[0], 'T%d' % len(names)), code)
    return re.sub(r'\s+', '', code)


def fuse_copy(code):
    labels = list(re.finditer(r'^(?:L|X|P)(\d+): ;(?: /\* (.*?) \*/)?$', code, re.M))
    blocks = {}
    for i, label in enumerate(labels):
        if code[label.start()] != 'L':
            continue
        end = labels[i + 1].start() if i + 1 < len(labels) else len(code)
        block = code[label.end():end]
        prefix = ('\n    if (__builtin_expect(c->cycle >= end, 0)) { c->jit_exit[5]++; goto X%s; }'
                  '\n    if (__builtin_expect(c->code_gen != gen, 0)) { c->jit_exit[6]++; goto X%s; }') % (label[1], label[1])
        if not block.startswith(prefix):
            continue
        blocks[int(label[1])] = (label, prefix, block[len(prefix):])
    for number, (label, prefix, first) in blocks.items():
        if label[2] != 'S 0 mcnop 0 pre 0 r4 m1':
            continue
        src = re.search(r'jit_load64\(c, c->reg\[(\d+)\]\)', first)
        dst = re.search(r'c->reg\[(\d+)\] = m0;', first)
        next_label = re.search(r'goto L(\d+);\s*$', first)
        if not src or not dst or not next_label:
            continue
        src, dst, nxt = int(src[1]), int(dst[1]), int(next_label[1])
        if nxt not in blocks or blocks[nxt][0][2] != 'S 1 mcnop 0 pre 0 r6 m1':
            continue
        second = blocks[nxt][2]
        out = re.search(r'c->reg\[(\d+)\] = r0;\s*c->reg\[(\d+)\] = r1;', second)
        jumps = re.findall(r'goto L(\d+);', second)
        if not out or int(out[2]) != int(out[1]) + 1 or len(jumps) != 2 or int(jumps[0]) != number:
            continue
        out = int(out[1])
        if len({src, dst, out, out + 1}) != 4:
            continue
        expected_first = '''
 c->reg[%d] = m0;
 uint32_t T0 = c->reg[%d] + 0x00000008u;
 uint64_t T1 = 0;
 T1 = jit_load64(c, c->reg[%d]);
 uint32_t T2 = (uint32_t)(T1);
 uint32_t T3 = (uint32_t)((T1) >> 32);
 if (c->npst) JIT_API(c)->flush_stores(c);
 c->cycle++;
 { uint32_t _r4 = T2; uint32_t _r5 = T3; uint32_t _m0 = T0; r4 = _r4; r5 = _r5; m0 = _m0; }
 goto L%d;
''' % (dst, src, src, nxt)
        rotate = '{ uint32_t _r0 = r2; uint32_t _r1 = r3; uint32_t _r2 = r4; uint32_t _r3 = r5; uint32_t _m0 = T0; r0 = _r0; r1 = _r1; r2 = _r2; r3 = _r3; m0 = _m0; }'
        expected_second = '''
 c->reg[%d] = r0; c->reg[%d] = r1; c->reg[%d] = m0;
 if (__builtin_expect((irq_possible(c) && pending_interrupt(c)), 0)) { c->jit_exit[0]++; goto P%d; }
 c->store_now = 1;
 uint32_t T0 = c->reg[%d] + 0x00000008u;
 { uint32_t base = c->reg[%d], ea = base + 0x00000008u, v = c->reg[%d]; ea = base; jit_store64(c, ea, v, c->reg[%d]); }
 c->store_now = 0;
 if (c->npst) JIT_API(c)->flush_stores(c);
 if (__builtin_expect(c->cr[CR_ILC] != 0, 1)) {
 c->cr[CR_ILC]--; c->spl.last_iter = (int)((c->cycle - c->spl.t0 + 1) / 2);
 c->cycle++; %s goto L%d;
 } else {
 c->spl.terminated = 1; c->spl.drain_start = c->cycle + 1;
 { uint64_t en_ = c->spl.drain_start + 0; if (en_ > c->spl.kernel_cycle + 1) c->pm_resume_at = en_ + 6; }
 c->cycle++; %s goto L%s;
 }
''' % (out, out + 1, src, nxt, dst, dst, out, out + 1, rotate, number, rotate, jumps[1])
        if canonical(first) != canonical(expected_first) or canonical(second) != canonical(expected_second):
            continue
        injection = '''
    if (_copy_ok && c->cr[CR_ILC] >= 4 && end - c->cycle >= 8) {
        uint64_t _tail[2];
        unsigned _n = jit_copy_burst(c, end, gen, %d, %d, %d, m0,
                                     ((uint64_t)r1 << 32) | r0,
                                     ((uint64_t)r3 << 32) | r2, _tail);
        if (_n) {
            r0 = (uint32_t)_tail[0]; r1 = (uint32_t)(_tail[0] >> 32);
            r2 = (uint32_t)_tail[1]; r3 = (uint32_t)(_tail[1] >> 32);
            r4 = r2; r5 = r3; m0 += 8 * _n;
            goto L%d;
        }
        _copy_ok = 0;
    }
''' % (src, dst, out, number)
        point = label.end() + len(prefix)
        code = code[:point] + injection + code[point:]
        opening = code.index('{') + 1
        code = code[:opening] + '\n    unsigned _copy_ok = 1;' + code[opening:]
        return code, True
    return code, False
