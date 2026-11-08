from pe import r2o, o2r, d
VA_BASE = 0x400000

# ---------------------------------------------------------------------------
# 1. VB5 native method table
#    prologue: 55 8B EC [83 EC xx | 81 EC xxxx]
#              68 <SEHrec>  64 A1 00 00 00 00  50  89 25 00 00 00 00
# ---------------------------------------------------------------------------
meth = []
i = 0
N = len(d)
while True:
    i = d.find(b'\x55\x8b\xec', i)
    if i < 0 or i > N - 40:
        break
    k = i + 3
    if d[k] == 0x83 and d[k+1] == 0xec:
        k += 3
    elif d[k] == 0x81 and d[k+1] == 0xec:
        k += 5
    n = d[k:k+20]
    ok = (n[0] == 0x68 and n[5] == 0x64 and n[6] == 0xa1 and
          n[7:11] == b'\x00' * 4 and n[11] == 0x50 and
          n[12:15] == b'\x64\x89\x25' and n[15:19] == b'\x00' * 4)
    if ok:
        meth.append(i)
    i += 1

print('VB5 native methods:', len(meth))
print('first VA', hex(meth[0] + VA_BASE), 'last VA', hex(meth[-1] + VA_BASE))

# sanity: the documented method for do_instruction_C0 region should be found
print('method containing rva 0x869e0:', hex(meth[0] + VA_BASE))
