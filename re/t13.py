from pe import *
p=0x73070
recs=[]
while p+13<=0x74000:
    nm=d[p:p+13]
    if not all(32<=c<127 for c in nm): break
    recs.append(nm.decode("ascii").strip())
    if d[p+13:p+15]==bytes([0x0e,0x00]): p+=15
    elif d[p+13:p+19]==bytes([0x00,0x00,0x0e,0x00,0x20,0x41]) or d[p+13:p+15]==bytes([0x00,0x00]): p+=17
    else: print("sep",hex(p+13),d[p+13:p+15].hex()); break
print(len(recs))
