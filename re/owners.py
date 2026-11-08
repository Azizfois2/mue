from pe import r2o, o2r, d
from scan import meth
import bisect, struct

VA_BASE = 0x400000


def owner(va):
    """Return (index, method_va) of the VB5 method containing this VA."""
    rva = va - VA_BASE
    lo = bisect.bisect_right(meth, rva) - 1
    if lo < 0:
        return None
    return lo, meth[lo] + VA_BASE


SITES = {
    'unknown opcode skipped:':           0x116e86,
    'not 8086 instruction - not supported yet.': 0x116e97,
    'no such opcode in 8086 instruction set.':  0x100d0f,
    'no such opcode #2':                 0x1060e6,
    'LOCK-OPCODE:F0':                    None,
    'error on single step: ':            0x117186,
    '------------- REGISTERS -------------': 0x11879a,
    'emulator.state':                    0xe47dd,
    'emulator.state #2':                 0xe5270,
    'emulator.state #3':                 0x12aef0,
    'wrong C6 address on doStep()':       0xf06f7,
}

if __name__ == '__main__':
    print('methods:', len(meth))
    for k, va in SITES.items():
        if va is None:
            continue
        rva = va
        o = owner(rva + VA_BASE)
        if o is None:
            print('%-45s NO OWNER' % k)
            continue
        idx, mva = o
        nxt = meth[idx + 1] + VA_BASE if idx + 1 < len(meth) else 0x6221000
        print('%-45s site=%06x  METHOD #%d va=%08x size=%d' % (k, rva, idx, mva, nxt - mva))
