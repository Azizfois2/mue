from pe import r2o, o2r, d

# The array is 14-char fields, but the AAS entry is followed by a 4-byte
# <00 00 0E 00> terminator pair before the next group starts.
start = 0x73070
recs = []
p = start
while p + 16 <= 0x78000:
    nm = d[p:p + 14]
    if not all(32 <= c < 127 for c in nm):
        break
    recs.append(nm.decode('ascii').strip())
    if d[p + 14:p + 16] == b'\x0e\x00':
        p += 16
    elif d[p + 14:p + 18] == b'\x00\x00\x00\x00':
        p += 18
    elif d[p + 14:p + 18] == b'\x00\x00\x0e\x00':
        p += 18
    else:
        print('unexpected sep at', hex(p + 14), d[p + 14:p + 22].hex(' '))
        break

print('records:', len(recs))
for k in range(0, len(recs), 6):
    print('%3d: %s' % (k, ' | '.join(recs[k:k + 6])))
