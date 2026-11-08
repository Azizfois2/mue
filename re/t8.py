import re,struct
from pe import *
lo=0xeff00; hi=0x117300
seg=d[lo:hi]
for pat,name in [(bytes([0xff,0x24]),(b"ff24",b"jmp [reg*4]")),
                 (bytes([0xff,0x64]),(b"ff64",b"jmp [reg+disp]")),
                 (bytes([0x24]),(b"",b""))]:
    pass
for m in re.finditer(bytes([0xff,0x24]), seg):
    print("ff24 at", hex(lo+m.start()), seg[m.start():m.start()+8].hex(" "))
