from pe import *
p=0x73070
recs=[]
for k in range(300):
    nm=d[p:p+13]
    if not all(32<=c<127 for c in nm): print("bad at",hex(p),repr(nm)); break
    recs.append(nm.decode("ascii").strip())
    p+=13
    s2=d[p:p+2]
    if s2==bytes([0x0e,0x00]): p+=2
    elif s2==bytes([0x00,0x00]): p+=4
    else: print("sep",hex(p),s2.hex()); break
print(len(recs),recs[:3],recs[-3:])
