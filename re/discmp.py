from pe import r2o, o2r, d
import struct, re

lo, hi = 0xeff00, 0x117300
seg = d[lo:hi]

# Collect cmp <ax>, imm16 immediates -- these are the VB5 Select Case bounds.
vals_ax = []
for m in re.finditer(b'\x66\x3d(.{2})', seg, re.S):
    v = struct.unpack('<H', m.group(1))[0]
    vals_ax.append((lo + m.start(), v))
vals_dx = []
for m in re.finditer(b'\x66\x81\xfa(.{2})', seg, re.S):
    v = struct.unpack('<H', m.group(1))[0]
    vals_dx.append((lo + m.start(), v))

from collections import Counter
print('cmp ax histogram (top 25):')
for v, c in Counter(v for _, v in vals_ax).most_common(25):
    print('   0x%02x  x%d' % (v, c))
print()
print('cmp dx histogram (top 25):')
for v, c in Counter(v for _, v in vals_dx).most_common(25):
    print('   0x%02x  x%d' % (v, c))
