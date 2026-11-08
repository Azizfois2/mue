import struct

PATH = r'C:\emu8086\emu8086.exe'
d = open(PATH, 'rb').read()

e = struct.unpack('<I', d[0x3c:0x40])[0]
opt = e + 24
nsec = struct.unpack('<H', d[e+6:e+8])[0]
so = opt + struct.unpack('<H', d[e+20:e+22])[0]
S = []
for i in range(nsec):
    s = so + i * 40
    nm = d[s:s+8].rstrip(b'\x00').decode()
    vs, va, rs, ra = struct.unpack('<IIII', d[s+8:s+24])
    S.append((nm, va, vs, ra, rs))


def r2o(r):
    for nm, va, vs, ra, rs in S:
        if va <= r < va + vs:
            return ra + (r - va)
    raise ValueError(hex(r))


def o2r(o):
    for nm, va, vs, ra, rs in S:
        if ra <= o < ra + rs:
            return va + (o - ra)
    raise ValueError(hex(o))
