from pe import *
for rva in (0x19c92, 0x869e0, 0x48878, 0x73070, 0x1a018):
    o=r2o(rva)
    print(hex(rva), "-> raw", hex(o), d[o:o+16].hex(" "))
