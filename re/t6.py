from pe import *
import struct
lo=0xeff00; hi=0x117300
seg=d[lo:hi]
print("bytes",len(seg))
print("jmp[imm32] :", seg.count(bytes([0xff,0x25])))
print("call[imm32]:", seg.count(bytes([0xff,0x15])))
