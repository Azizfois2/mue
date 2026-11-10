import struct
import pe
from pe import d, r2o

VA = 0x400000


def rd(a, n):
    return d[r2o(a - VA):r2o(a - VA) + n]


def dw(a):
    return struct.unpack('<I', rd(a, 4))[0]


def wd(a):
    return struct.unpack('<H', rd(a, 2))[0]


def asc(a, n=32):
    b = rd(a, n)
    out = ''
    for c in b:
        if 32 <= c < 127:
            out += chr(c)
        else:
            break
    return out


base = 0x435084
stride = 0x30
total = 0
rows = []
for i in range(40):
    a = base + i * stride
    name_p = dw(a)
    nm = asc(name_p, 28) if 0x400000 <= name_p < 0x63c000 else '<%08x>' % name_p
    if not nm:
        break
    cnt = dw(a + 4)
    tbl = dw(a + 8)
    flags = dw(a + 0x10)
    entry = dw(a + 0x18)
    iid = dw(a + 0x20)
    glob = dw(a + 0x28)
    rows.append((i, a, nm, cnt, tbl, flags, entry, iid, glob))
    total += cnt

for i, a, nm, cnt, tbl, flags, entry, iid, glob in rows:
    print('%2d %06x %-22s cnt=%-5d tbl=%s flags=%08x entry=%08x iid=%08x glob=%s'
          % (i, a, nm, cnt,
             ('%06x' % tbl) if tbl else '     -', flags, entry, iid,
             ('%06x' % glob) if glob else '-'))
print('records', len(rows), 'sum of counts', total)
