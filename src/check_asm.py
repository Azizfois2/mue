"""Compare mue's assembler output against emu8086's own .list listings.

The .list files truncate long data lines, so every byte is compared at the
location the listing reports rather than by naive concatenation.
"""
import re, subprocess, os, glob

base = "C:/emu8086/MyBuild"
ok = fail = skip = 0
fails = []

for lst in sorted(glob.glob(base + "/*.list")):
    asm = lst[:-5] + ".~asm"
    if not os.path.exists(asm):
        asm = lst[:-5]
    if not os.path.exists(asm):
        skip += 1
        continue

    exp = []                                  # (loc, bytes) as listed
    for line in open(lst, errors="replace"):
        m = re.match(r"\[\s*\d+\]\s+([0-9A-Fa-f]{4}):\s+((?:[0-9A-Fa-f]{2} )+)", line)
        if m:
            exp.append((int(m.group(1), 16),
                        bytes(int(x, 16) for x in m.group(2).split())))
    if not exp:
        skip += 1
        continue

    out = "/tmp/out.bin"
    r = subprocess.run(["./mue.exe", "-a", asm, out], capture_output=True, text=True)
    if r.returncode != 0:
        fail += 1
        fails.append((os.path.basename(asm), "ASM ERR: " + r.stderr.strip()[:70]))
        continue

    got = open(out, "rb").read()
    # An `.model` source without ORG is an MZ EXE: skip the 0x200 header so the
    # comparison sees code/data at the same addresses the listing shows.
    if got[:2] == b"MZ":
        hdr = int.from_bytes(got[8:10], "little") * 16
        got = got[hdr:]
        # A `.stack N` segment sits ahead of .data in the image but is not
        # listed, so drop it (sp holds its size).
        # header offset 0x10 is SP, which emu8086 sets to the stack size
        raw = open(out, "rb").read()
        stk = int.from_bytes(raw[16:18], "little")
        if stk and got[:stk] == b"T" * stk:
            got = got[stk:]
    start = exp[0][0]                                # lowest listed address
    seg = got                                        # image begins at that address
    # listings truncate long data lines, so offsets must come from the
    # location field; holes are filled from our own image (unasserted)
    buf = bytearray(got)
    for loc, b in exp:
        off = loc - start
        if off + len(b) > len(buf):
            buf.extend(bytes(off + len(b) - len(buf)))
        buf[off:off + len(b)] = b
    want = bytes(buf)

    bad = None
    for loc, b in exp:
        off = loc - start
        if seg[off:off + len(b)] != b:
            for i in range(len(b)):
                if off + i >= len(seg) or seg[off + i] != b[i]:
                    bad = off + i
                    break
            break

    if bad is None:
        ok += 1
    else:
        fail += 1
        fails.append((os.path.basename(asm),
                      "byte %d: want %s got %s" % (
                          bad,
                          want[bad:bad + 6].hex() if bad < len(want) else "-",
                          seg[bad:bad + 6].hex())))

print("PASS %d  FAIL %d  SKIP %d" % (ok, fail, skip))
for n, m in fails[:16]:
    print("  %s: %s" % (n, m))
