# Builds release byte patterns from tools/sig_input.txt (Ghidra DumpForSigs.java) and checks each matches exactly once

# in SOTTR.exe's .text (offline, read-only). Relative branch targets and RIP-relative displacements are wildcarded

# (from Ghidra's references), so a pattern survives code moving between builds; struct offsets and immediates stay.

# Usage: python tools/make_sigs.py ["<game folder>/SOTTR.exe"]  -> prints the C++ table for src/game_addresses.cpp
# (tools/sig_input.txt is Ghidra's dump of the game's code; it is not distributed -- make it with DumpForSigs.java.)

import re, struct, sys

sys.path.insert(0, 'tools')

from aob_check import IMG, TEXT



BASE = 0x140000000

targets = []   # (kind, target, [ (addr, bytes, refs) ...] per window)

cur = None

for line in open('tools/sig_input.txt', encoding='utf-8'):

    p = line.rstrip('\n').split(' ')

    if p[0] == 'T':

        cur = {'kind': p[1], 'target': int(p[2], 16), 'windows': [], 'refsite': []}

        targets.append(cur)

        if p[1] == 'code': cur['windows'].append([])

    elif p[0] == 'R':

        cur['refsite'].append(int(p[1], 16)); cur['windows'].append([])

    elif p[0] == 'I':

        refs = [] if p[3] == '-' else [int(x, 16) for x in p[3].split(',') if re.fullmatch(r'[0-9a-fA-F]+', x)]

        cur['windows'][-1].append((int(p[1], 16), bytes.fromhex(p[2]), refs))



def masked(ins):

    addr, b, refs = ins

    end = addr + len(b)

    m = [True] * len(b)   # True = fixed byte

    for r in refs:

        for size, fmt in ((4, '<i'), (1, '<b')):

            d = r - end

            if size == 1 and not -128 <= d <= 127: continue

            enc = struct.pack(fmt, d)

            k = b.rfind(enc)

            if k > 0:

                for j in range(k, k + size): m[j] = False

                break

    return [(b[j] if m[j] else None) for j in range(len(b))]



def regex(pat):

    return re.compile(b''.join(re.escape(bytes([x])) if x is not None else b'.' for x in pat), re.S)



def count(pat):

    rx = regex(pat)

    n = 0

    for _ in rx.finditer(IMG, TEXT[1], TEXT[1] + TEXT[2]):

        n += 1

        if n > 1: break

    return n



def build(window, first, need_until=None):

    """Grow a pattern from instruction index `first` until unique (and covering need_until)."""

    pat = []

    for i in range(first, len(window)):

        if i > first:

            gap = window[i][0] - (window[i - 1][0] + len(window[i - 1][1]))

            if gap < 0 or gap > 32: return None

            pat += [None] * gap   # padding between functions: wildcards

        pat += masked(window[i])

        covered = need_until is None or window[i][0] + len(window[i][1]) >= need_until

        if covered and len(pat) >= 12 and count(pat) == 1:

            return pat

        if len(pat) > 160: return None

    return None



def fmt(pat):

    return ' '.join('??' if x is None else '%02X' % x for x in pat)



rows = []

for t in targets:

    T = t['target']

    if t['kind'] == 'code':

        w = t['windows'][0]

        idx = next(i for i, ins in enumerate(w) if ins[0] == T)

        best = None

        # start at the target, or up to 6 instructions before it (a call site's return address)

        # a function entry: padding or a return right before it -- patterns then start at the target itself
        is_entry = idx == 0 or w[idx - 1][0] + len(w[idx - 1][1]) < T or w[idx - 1][1][-1:] == bytes([0xC3])
        for back in range(0, 1 if is_entry else 7):

            if idx - back < 0: break

            pat = build(w, idx - back, T + 1)

            if pat:

                start = w[idx - back][0]

                cand = (len(pat), pat, T - start)

                if back == 0: best = cand; break

                if best is None or cand[0] < best[0]: best = cand

        if not best: print('NO PATTERN for', hex(T)); continue

        _, pat, off = best

        # verify the unique match lands on the target

        m = regex(pat).search(IMG, TEXT[1], TEXT[1] + TEXT[2])

        assert m.start() + off == T - BASE, hex(T)

        rows.append(('code', T, fmt(pat), off, 0, 0))

    else:

        done = False

        for site, w in zip(t['refsite'], t['windows']):

            idx = next((i for i, ins in enumerate(w) if ins[0] == site), None)

            if idx is None: continue

            ins = w[idx]

            end = ins[0] + len(ins[1])

            k = ins[1].rfind(struct.pack('<i', T - end))

            if k < 0: continue

            for back in range(0, 4):

                if idx - back < 0: break

                pat = build(w, idx - back, end)

                if not pat: continue

                start = w[idx - back][0]

                m = regex(pat).search(IMG, TEXT[1], TEXT[1] + TEXT[2])

                instOff = site - start

                at = m.start() + instOff

                disp = struct.unpack_from('<i', IMG, at + k)[0]

                assert at + len(ins[1]) + disp == T - BASE

                rows.append(('data', T, fmt(pat), instOff, k, len(ins[1])))

                done = True

                break

            if done: break

        if not done: print('NO PATTERN for data', hex(T))



for kind, T, pat, off, dispAt, ilen in rows:

    if kind == 'code':

        print('    // 0x%X\n    { "%s", %d },' % (T, pat, off))

    else:

        print('    // data 0x%X: instruction at +%d, disp32 at +%d, length %d\n    { "%s", %d, %d, %d },' % (T, off, dispAt, ilen, pat, off, dispAt, ilen))

