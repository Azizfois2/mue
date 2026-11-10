import struct
import pe
from pe import d, r2o

VA = 0x400000


def rd(addr, n):
    return d[r2o(addr - VA):r2o(addr - VA) + n]


def dw(addr):
    return struct.unpack('<I', rd(addr, 4))[0]


def w(addr):
    return struct.unpack('<H', rd(addr, 2))[0]


def cstr(addr):
    out = b''
    while True:
        c = rd(addr + len(out), 1)
        if c == b'\x00' or not c:
            break
        out += c
    return out.decode('latin1')


hdr = 0x41ac1c
print('wRuntimeBuild', hex(w(hdr + 4)))
print('lpSubMain', hex(dw(hdr + 0x2c)))
pd = dw(hdr + 0x30)
print('lpProjectData', hex(pd))
print('wFormCount', w(hdr + 0x44), 'wExternalCount', w(hdr + 0x46),
      'thunks', dw(hdr + 0x48))
print('lpGuiTable', hex(dw(hdr + 0x4c)), 'lpExternalTable', hex(dw(hdr + 0x50)))
print('project name', cstr(dw(hdr + 0x64)))

print('\n--- lpProjectData dump ---')
for i in range(0, 0x60, 4):
    print('%08x' % (pd + i), '%08x' % dw(pd + i))

ot = dw(pd + 8)
print('\nlpObjectTable', hex(ot))
