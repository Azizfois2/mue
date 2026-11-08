from pe import r2o, o2r, d
import struct, re

lo, hi = 0xeff00, 0x117300
seg = d[lo:hi]

print('total bytes', len(seg))
print('cmp ax, imm16  (66 3d xx xx) :', seg.count(bytes([0x66, 0x3d])))
print('cmp dx, imm16  (66 81 fa)    :', seg.count(bytes([0x66, 0x81, 0xfa])))
print('cmp bx, imm16  (66 81 fb)    :', seg.count(bytes([0x66, 0x81, 0xfb])))
print('jmp [eax*4+tbl] (ff 24 85)   :', seg.count(bytes([0xff, 0x24, 0x85])))
print('jmp [edx*4+tbl] (ff 24 95)   :', seg.count(bytes([0xff, 0x24, 0x95])))
for code in range(4):
    for rm in (0x85, 0x95, 0xa5, 0xb5, 0x8d, 0x9d):
        pat = bytes([0xff, 0x24, rm])
        n = seg.count(pat)
        if n:
            print('ff 24 %02x ->' % rm, n)
