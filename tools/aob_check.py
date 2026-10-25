# Offline checks for the release address resolvers against the installed SOTTR.exe (read-only).
# Usage: python tools/aob_check.py ["<game folder>/SOTTR.exe"]
import struct, re, sys
EXE = sys.argv[1] if len(sys.argv) > 1 else r'D:/SteamLibrary/steamapps/common/Shadow of the Tomb Raider/SOTTR.exe'
data = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
opt = struct.unpack_from('<H', data, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode()
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, va, vsz, rptr, rsz))
def sec(name):
    for s in secs:
        if s[0] == name: return s
def image():
    # a flat RVA-indexed image (sections copied to their RVAs)
    top = max(va + max(vsz, rsz) for _, va, vsz, _, rsz in secs)
    img = bytearray(top)
    for _, va, vsz, rptr, rsz in secs:
        img[va:va + rsz] = data[rptr:rptr + rsz]
    return bytes(img)
IMG = image()
TEXT = sec('.text'); RDATA = sec('.rdata')

def find_all(pat, lo, hi):
    out = []; i = IMG.find(pat, lo, hi)
    while i >= 0:
        out.append(i); i = IMG.find(pat, i + 1, hi)
    return out

def event_global(name):
    """string -> code that LEAs it -> the next 'mov [rip+d32], eax' -> the id global's RVA."""
    s = find_all(name.encode() + b'\0', RDATA[1], RDATA[1] + RDATA[2])
    res = set()
    for srva in s:
        if IMG[srva - 1] != 0: continue   # whole string only
        t0, t1 = TEXT[1], TEXT[1] + TEXT[2]
        # LEA r64, [rip+disp32]: 48/4C 8D modrm(00 reg 101) disp32
        for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', IMG[t0:t1]):
            at = t0 + m.start()
            disp = struct.unpack_from('<i', IMG, at + 3)[0]
            if at + 7 + disp != srva: continue
            # the register call after it (E8 rel32), then the first store of a 32-bit register to a rip-relative global
            k = at + 7
            while k < at + 7 + 48 and IMG[k] != 0xE8: k += 1
            if IMG[k] != 0xE8: continue
            for j in range(k + 5, k + 5 + 32):
                if IMG[j] == 0x89 and (IMG[j + 1] & 0xC7) == 0x05:
                    d2 = struct.unpack_from('<i', IMG, j + 2)[0]
                    res.add(j + 6 + d2); break
    return sorted(res)

if __name__ == '__main__':
    src = open('src/cinematic_watch.cpp', encoding='utf-8').read()
    ok = bad = 0
    for name, rva in re.findall(r'\{ "(\w+)",\s+0x([0-9A-F]+), Act::', src):
        got = event_global(name)
        good = got == [int(rva, 16)]
        ok += good; bad += not good
        print('%-45s table 0x%s  resolved %s %s' % (name, rva, [hex(g) for g in got], 'OK' if good else '<-- MISMATCH'))
    print('ok', ok, 'mismatch', bad)
