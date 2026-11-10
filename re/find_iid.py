import struct
import pe
from pe import d, r2o

VA = 0x400000
GUID = bytes.fromhex('2d1274cbf6860384'.replace('0384', '0344'))  # placeholder


def rd(a, n):
    return d[r2o(a - VA):r2o(a - VA) + n]


def dw(a):
    return struct.unpack('<I', rd(a, 4))[0]


def asc(a, n=32):
    b = rd(a, n)
    return ''.join(chr(c) if 32 <= c < 127 else '.' for c in b)


g = bytes([0x2d, 0x12, 0x74, 0xcb, 0xf6, 0x86, 0x03, 0x44,
           0x89, 0xa1, 0x5a, 0xb6, 0x8f, 0x73, 0x3c, 0x03])
i = 0
hits = []
while True:
    i = d.find(g, i)
    if i < 0:
        break
    hits.append(i)
    i += 1
print('IID occurrences in file:', len(hits))
for h in hits:
    print('  file off', hex(h), 'VA', hex(VA + pe.o2r(h)))
