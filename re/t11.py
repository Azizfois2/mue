import re,struct
from pe import *
# verify the opcode-name table layout: 13-char records separated by 0e 00
p=0x73070
names=[]
while True:
    rec=d[p:p+13]
    if not all(32<=c<127 for c in rec): break
    names.append(rec.decode("ascii").strip())
    sep=d[p+13:p+15]
    if sep==bytes([0x0e,0x00]): p+=15
    elif sep==bytes([0x00,0x00]): p+=17
    else: break
print(len(names))
for k in range(0,len(names),6): print(k, names[k:k+6])
