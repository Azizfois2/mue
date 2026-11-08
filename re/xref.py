from pe import r2o, o2r, d
from scan import meth
import struct, re

VA_BASE = 0x400000
N = len(d)


# Locate a UTF-16 literal by its ASCII text; return RVA of first char.
def find_lit(text):
    raw = text.encode('utf-16le')
    out = []
    i = 0
    while True:
        i = d.find(raw, i)
        if i < 0:
            break
        out.append(i)
        i += 1
    return out


# Report every string-literal site, plus all code that loads its VA.
def who_uses(text):
    sites = find_lit(text)
    res = []
    for rva in sites:
        va = rva + VA_BASE
        pat = struct.pack('<I', va)
        refs = []
        i = 0
        while True:
            i = d.find(pat, i)
            if i < 0:
                break
            refs.append(i)
            i += 1
        res.append((rva, refs))
    return res


if __name__ == '__main__':
    for s in ['unknown opcode skipped:', 'not 8086 instruction - not supported yet.',
              'no such opcode in 8086 instruction set.', 'LOCK-OPCODE:F0',
              'error on single step: ', 'do_instruction_C0: ',
              '------------- REGISTERS -------------', 'emulator.state',
              'wrong C6 address on doStep()', 'reset emulator: restore']:
        for rva, refs in who_uses(s):
            print('%-45s rva=%06x refs=%s' % (s[:45], rva, [hex(r) for r in refs][:8]))
