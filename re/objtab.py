import struct
import pe
from pe import d, r2o

VA = 0x400000


def rd(a, n):
    return d[r2o(a - VA):r2o(a - VA) + n]


def dw(a):
    return struct.unpack('<I', rd(a, 4))[0]


def asc(a, n=40):
    b = rd(a, n)
    return ''.join(chr(c) if 32 <= c < 127 else '.' for c in b)


# find dwords in the object-table region pointing into the ASCII name pool
lo, hi = 0x435018, 0x436200
name_lo, name_hi = 0x437000, 0x43a400
prev = None
for a in range(lo, hi, 4):
    v = dw(a)
    if name_lo <= v < name_hi:
        s = asc(v, 28).split('\x00')[0]
        print('%08x -> %08x  %-24s | next: %s' % (
            a, v, s,
            ' '.join('%08x' % dw(a + 4 + 4 * k) for k in range(6))))
