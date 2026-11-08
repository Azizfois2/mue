import re
from pe import *
lo=0xeff00; hi=0x117300
seg=d[lo:hi]
# VB5 Select Case on a small int compiles to a jump table: ff 24 85 <addr of table>
for m in re.finditer(bytes([0xff,0x24,0x85])+b"(.{4})", seg, re.S):
    import struct
    t=struct.unpack("<I",m.group(1))[0]
    print("jmp [eax*4+tbl]", hex(lo+m.start()), "table VA", hex(t))
for m in re.finditer(bytes([0xff,0x24,0x85]), seg):
    print("hit at", hex(lo+m.start()))
