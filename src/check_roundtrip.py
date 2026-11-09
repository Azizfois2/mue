"""Byte round-trip against emu8086's own shipped binaries.

This is the strongest available external oracle: for each source in
C:/emu8086/MyBuild, emu8086 itself wrote the machine code next to it
(*.bin_, *.com_, *.exe_).  We reassemble that same source with mue and require
the result to be byte-identical -- including the MZ EXE header.

Exit code 0 only if every pair matches.
"""
import glob
import os
import subprocess
import sys

BASE = "C:/emu8086/MyBuild"
MUE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mue.exe")
OUT = os.path.join(os.environ.get("TEMP", "/tmp"), "mue_roundtrip.bin")

ok = fail = skip = 0
fails = []

for src in sorted(glob.glob(BASE + "/*.~asm")):
    ref = src[: -len(".~asm")]          # e.g. adc.exe_ / hi-world.com_ / 0.bin_
    if not os.path.exists(ref):
        skip += 1
        continue

    try:
        r = subprocess.run([MUE, "-a", src, OUT], capture_output=True, text=True)
    except OSError as e:
        fail += 1
        fails.append((os.path.basename(ref), "cannot run mue: %s" % e))
        continue
    if r.returncode != 0:
        fail += 1
        fails.append((os.path.basename(ref), "ASM ERR: " + (r.stderr or "").strip()[:70]))
        continue

    got = open(OUT, "rb").read()
    want = open(ref, "rb").read()
    if got == want:
        ok += 1
    else:
        fail += 1
        if len(got) != len(want):
            why = "size %d != %d" % (len(got), len(want))
        else:
            at = next(i for i in range(len(got)) if got[i] != want[i])
            why = "first diff at 0x%x: got %02x want %02x" % (at, got[at], want[at])
        fails.append((os.path.basename(ref), why))

for name, why in fails:
    print("  %s: %s" % (name, why))

print("ROUNDTRIP  pass=%d  fail=%d  skip=%d" % (ok, fail, skip))
sys.exit(1 if fail else 0)
