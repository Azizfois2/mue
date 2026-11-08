from pe import r2o, o2r, d

# Fixed-width VB5 string array lst_opNames:
#   <14 chars ' NAME         '><0x0E 0x00>   stride 16
# with an occasional <00 00 0E 00> terminator pair after the AAS record.
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
    else:
        break

print('records:', len(recs))
for k in range(0, len(recs), 6):
    print('%3d: %s' % (k, ' | '.join(recs[k:k + 6])))
