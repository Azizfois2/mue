from pe import r2o, o2r, d
import struct, re

# The opcode dispatcher compares the fetched opcode byte (held in a 16-bit
# local, loaded with movzx dx,BYTE[..]) against each opcode value in turn.
# VB5 emits:  66 81 FA <imm16>        cmp dx, imm16
#          or 66 3D    <imm16>        cmp ax, imm16
# We walk the emulator method and emit (site, imm, next jcc target) in order.

lo, hi = 0xeff00, 0x117300
pat = re.compile(rb'\x66\x81\xfa(.{2})', re.S)
rows = []
for m in pat.finditer(d[lo:hi]):
    site = lo + m.start()
    v = struct.unpack('<H', m.group(1))[0]
    # peek the next few bytes for a conditional jump
    tail = d[site + 5:site + 7]
    rows.append((site, v, tail.hex()))

print('cmp dx,imm16 sites in order:', len(rows))
print('%-10s %-6s %s' % ('site(VA)', 'imm', 'next-bytes'))
for site, v, tail in rows:
    print('%08x   %-6s %s' % (site + 0x400000, hex(v), tail))
