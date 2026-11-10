import struct
import pe
from pe import d, r2o

e = struct.unpack('<I', d[0x3c:0x40])[0]
opt = e + 24
rsrc_rva, rsrc_sz = struct.unpack('<II', d[opt + 112:opt + 120])
base = r2o(rsrc_rva)

rows = []


def walk(off, depth, path):
    nnamed, nid = struct.unpack('<HH', d[off + 12:off + 16])
    for i in range(nnamed + nid):
        ent = off + 16 + i * 8
        name, off2 = struct.unpack('<II', d[ent:ent + 8])
        if name & 0x80000000:
            so = base + (name & 0x7fffffff)
            ln = struct.unpack('<H', d[so:so + 2])[0]
            nm = d[so + 2:so + 2 + ln * 2].decode('utf-16le')
        else:
            nm = str(name)
        if off2 & 0x80000000:
            walk(base + (off2 & 0x7fffffff), depth + 1, path + [nm])
        else:
            do = base + off2
            rva, sz = struct.unpack('<II', d[do:do + 8])
            rows.append((path + [nm], rva, sz))


walk(base, 0, [])
for path, rva, sz in rows:
    print(' / '.join(path), 'size', sz, 'rva', hex(rva))
