import re
import bisect
from scan import meth

VA = 0x400000
lines = [l.rstrip('\r\n') for l in open('disasm.txt', encoding='utf8', errors='replace')]
addr_re = re.compile(r'^\s*([0-9a-f]{6,7}):\t(.*)$')
rows = []
for i, l in enumerate(lines):
    m = addr_re.match(l)
    rows.append((int(m.group(1), 16) if m else None, m.group(2) if m else None, i))

mvas = [m + VA for m in meth]


def meth_of(va):
    i = bisect.bisect_right(mvas, va) - 1
    return mvas[i]


sites = []
for idx, (va, txt, li) in enumerate(rows):
    if va and txt and '0x7ac]' in txt and 'call' in txt:
        # look ahead 40 lines for flag writes / message pushes
        flags = []
        msgs = []
        for j in range(idx + 1, min(idx + 40, len(rows))):
            t = rows[j][1] or ''
            if '62141c' in t or '621218' in t:
                flags.append('%s @%x' % (t.strip(), rows[j][0]))
            m2 = re.search(r'push\s+(0x4[0-9a-f]{5})', t)
            if m2:
                msgs.append(m2.group(1))
        # look back for message pushes too
        back = []
        for j in range(max(0, idx - 40), idx):
            t = rows[j][1] or ''
            m2 = re.search(r'mov\s+edx,(0x4[0-9a-f]{5})', t)
            if m2:
                back.append(m2.group(1))
        sites.append((va, meth_of(va), flags, msgs[-2:], back[-2:]))

print('call [x+0x7ac] sites:', len(sites))
cur = None
for va, mm, flags, msgs, back in sites:
    if mm != cur:
        print('\n== method %x' % mm)
        cur = mm
    print('  %06x flags=%s msgs=%s back=%s' % (va, flags, msgs, back))
