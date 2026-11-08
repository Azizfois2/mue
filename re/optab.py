from pe import r2o, o2r, d

# VB5 stores each literal as <u32 byte-len><utf16 chars>00 00.
# Parse the 13-byte fixed-width opcode-name array that sits right after the
# 'lst_opNames' variable, walking a strict record grammar.
start = 0x73070
recs = []
p = start
while p + 13 <= len(d):
    name = d[p:p + 13]
    if not all(32 <= c < 127 for c in name):
        break
    recs.append(name.decode('ascii').strip())
    # separators observed: 0e 00 (normal) and 00 00 0e 00 (after AAS block)
    if d[p + 13:p + 15] == b'\x0e\x00':
        p += 15
    elif d[p + 13:p + 17] == b'\x00\x00\x0e\x00':
        p += 17
    else:
        print('stop: unexpected separator at', hex(p + 13),
              d[p + 13:p + 21].hex(' '))
        break

print('records:', len(recs))
print('last:', recs[-5:] if recs else None)
