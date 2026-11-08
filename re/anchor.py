from pe import r2o, o2r, d
from scan import meth
import struct, re

VA_BASE = 0x400000

# ---------------------------------------------------------------------------
# Build a map of UTF-16 literal string pool (VB5 stores literals as
# <u32 bytelen><utf16 chars>00 00 in .text). We recover each literal and its RVA.
# ---------------------------------------------------------------------------
lits = {}          # rva of char data -> text
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', d):
    off = m.start()
    txt = m.group().decode('utf-16le', 'replace')
    lits[off] = txt

print('utf16 literals found:', len(lits))

# ---------------------------------------------------------------------------
# For each method, collect the immediate operands pushed (push imm32) that
# land inside the literal pool -> the method's string anchors.
# ---------------------------------------------------------------------------
anchors = []
for src, off in enumerate(meth):
    end = meth[src + 1] if src + 1 < len(meth) else r2o(0x221000)
    found = []
    for m in re.finditer(rb'\x68(.{4})', d[off:end], re.S):
        imm = struct.unpack('<I', m.group(1))[0]
        rva = imm - VA_BASE
        if rva in lits and len(lits[rva]) > 3:
            found.append(lits[rva])
    anchors.append(found)

named = sum(1 for a in anchors if a)
print('methods with string anchors:', named, '/', len(meth))

# dump everything
with open('methods_dump.txt', 'w', encoding='utf-8') as f:
    for k, off in enumerate(meth):
        f.write('%4d %08x  %s\n' % (k, off + VA_BASE, ' ; '.join(anchors[k])[:300]))
print('wrote methods_dump.txt')
