"""Which INT 21h functions do the shipped examples actually call?

Heuristic: for every `int 21h`, look back a few lines for `mov ah,XXh` or
`mov ax,XXXXh` and record AH. Approximate by nature, but good enough to rank
what to implement next.
"""
import collections
import glob
import os
import re

IMPL = {0x00, 0x01, 0x02, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x19, 0x1A,
        0x25, 0x2A, 0x2C, 0x2F, 0x30, 0x35, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
        0x41, 0x42, 0x43, 0x4C, 0x4D, 0x4E, 0x4F, 0x56}

cnt = collections.Counter()
where = collections.defaultdict(set)

for f in glob.glob('C:/emu8086/examples/**/*.asm', recursive=True):
    try:
        lines = open(f, encoding='latin1').read().splitlines()
    except OSError:
        continue
    for i, line in enumerate(lines):
        code = line.split(';')[0].strip().lower()
        if not re.match(r'^int\s+21h$', code):
            continue
        ah = None
        for j in range(i - 1, max(-1, i - 8), -1):
            c = lines[j].split(';')[0].strip().lower()
            m = re.match(r'^mov\s+ah\s*,\s*([0-9a-f]+)h$', c)
            if m:
                ah = int(m.group(1), 16)
                break
            m = re.match(r'^mov\s+ax\s*,\s*([0-9a-f]+)h$', c)
            if m:
                ah = int(m.group(1), 16) >> 8
                break
            if re.match(r'^(int|mov\s+ah)', c):
                break
        if ah is not None:
            cnt[ah] += 1
            where[ah].add(os.path.basename(f))

for ah, n in cnt.most_common():
    tag = 'ok' if ah in IMPL else 'MISSING'
    print('AH=%02X  uses=%-3d %-8s %s' % (ah, n, tag, ', '.join(sorted(where[ah])[:3])))
print('total sites', sum(cnt.values()))
