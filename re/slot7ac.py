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


tables = {
    'frmMain': (0x4360bc, 0x6f),
    'Module1': (0, 0),
    'frmDat': (0x436278, 1),
    'Module2': (0, 0x0b),
    'mInstructions': (0, 0x1e),
    'frmInfo': (0x43627c, 0x2f),
    'StringTokenizer': (0, 2),
    'frmMacro': (0x436338, 6),
    'frmEmulation': (0x436350, 0x12a),
    'other': (0, 0x36),
    'ALU': (0x4367f8, 0x3b),
    'frmScreen': (0x4368e4, 0x48),
    'RAM': (0, 6),
    'frmFLAGS': (0x436a04, 0x11),
    'frmOptions': (0x436a48, 0x10),
    'ComDlg': (0, 0x16),
    'frm_mBox': (0x436a88, 6),
    'frmAbout': (0x436aa0, 0x0b),
    'mCodeSelector': (0, 0x10),
    'frmOrigCode': (0x436acc, 0x13),
    'frmBaseConvertor': (0x436b18, 0x20),
    'frmStack': (0x436b98, 9),
}

for name, (tbl, cnt) in tables.items():
    if not tbl:
        continue
    slot = tbl + 0x7ac
    try:
        v = dw(slot)
    except Exception as e:
        print('%-18s tbl %06x slot %06x  <unreadable>' % (name, tbl, slot))
        continue
    s = asc(v, 30).split('\x00')[0] if 0x400000 <= v < 0x63c000 else '-'
    print('%-18s tbl %06x cnt %-5d slot %06x -> %08x %r' % (name, tbl, cnt, slot, v, s))
