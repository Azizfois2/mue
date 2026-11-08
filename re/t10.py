import re,struct
from pe import *
lo=0xf0600; hi=0xf0900
seg=d[lo:hi]
for m in re.finditer(bytes([0x66,0x81,0xfa])+b"(.{2})", seg, re.S):
    print(hex(lo+m.start()), hex(struct.unpack("<H",m.group(1))[0]))
