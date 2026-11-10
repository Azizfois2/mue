import struct
import pe
from pe import d, r2o

VA = 0x400000


def rd(a, n):
    return d[r2o(a - VA):r2o(a - VA) + n]


def dw(a):
    return struct.unpack('<I', rd(a, 4))[0]


def name_at(v):
    if not (0x400000 <= v < 0x63c000):
        return None
    s = asc(v, 60)
    if not s or not (s[0].isalpha() or s[0] == '_'):
        return None
    return s


def asc(a, n=60):
    b = rd(a, n)
    out = ''
    for c in b:
        if 32 <= c < 127:
            out += chr(c)
        else:
            break
    return out


# walk the packed name tables from the first known start
entries = []
a = 0x4360bc
while a < 0x437400:
    v = dw(a)
    if v == 0:
        entries.append((a, None))
    else:
        nm = name_at(v)
        if nm is None:
            break
        entries.append((a, nm))
    a += 4

print('total slots in packed region:', len(entries))
for base, label in ((0, 'raw index'), (7, 'after IUnknown+IDispatch'), (12, 'after 3 dwords')):
    idx = 491 - base
    if 0 <= idx < len(entries):
        addr, nm = entries[idx]
        print('%-24s -> idx %3d  entry@%06x  %r' % (label, idx, addr, nm))

print()
for k in range(480, 500):
    if k < len(entries):
        addr, nm = entries[k]
        print('  [%3d] %06x %r' % (k, addr, nm))
