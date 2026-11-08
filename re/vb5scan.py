import struct,re
d=open(r'C:\emu8086\emu8086.exe','rb').read()
e=struct.unpack('<I',d[0x3c:0x40])[0]; opt=e+24
nsec=struct.unpack('<H',d[e+6:e+8])[0]; so=opt+struct.unpack('<H',d[e+20:e+22])[0]
S=[]
for i in range(nsec):
    s=so+i*40; nm=d[s:s+8].rstrip(b'\x00').decode()
    vs,va,rs,ra=struct.unpack('<IIII',d[s+8:s+24]); S.append((nm,va,vs,ra,rs))
def o2r(o):
    for nm,va,vs,ra,rs in S:
        if ra<=o<ra+rs: return va+(o-ra)
def r2o(r):
    for nm,va,vs,ra,rs in S:
        if va<=r<va+vs: return ra+(r-va)

# native prologue for VB5 Win32 methods (direct-code generated):
# 55 8B EC 83 EC xx  call __vbaExceptHandler / __vbaFPException
pro_pat = b'\x55\x8b\xec'
entries=[]
for i in range(0x1000, r2o(0x221000)-80):
    if d[i]==0x55 and d[i+1]==0x8b and d[i+2]==0xec:
        # require a following call to the exception handler stub
        nxt = d[i+3:i+20]
        entries.append(i)
print('candidate entries', len(entries))

# A real VB5 method entry: 55 8B EC 83 EC xx E8 (call) ... where call targets a stub
real=[]
for i in entries:
    j=i+3
    if d[j]==0x83 and d[j+1]==0xec:
        j+=3
    elif d[j]==0x81 and d[j+1]==0xec:
        j+=5
    if d[j]==0xe8:
        tgt=o2r(j+5+struct.unpack('<i',d[j+1:j+5])[0])
        if tgt and 0x38700<=tgt<=0x3f000:
            real.append((i,tgt))
print('real method entries',len(real))
for a,t in real[:10]:
    print(hex(o2r(a)), hex(t))
