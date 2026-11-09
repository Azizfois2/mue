"""Assemble every program emu8086 ships in C:/emu8086/examples.

This is a breadth check, not a correctness oracle: it answers "does mue accept
this source at all", which catches whole features going missing (an operand
form, a directive, a macro facility).  Byte-exact correctness is covered by
check_asm.py and check_roundtrip.py.

Failures are expected to be a small non-zero set; the run fails only if the
number of assembling programs drops below --min (default: the best known
count), so a regression is loud while known gaps stay documented.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

BASE = "C:/emu8086/examples"
MUE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mue.exe")

ap = argparse.ArgumentParser()
ap.add_argument("--min", type=int, default=85,
                help="fail if fewer than this many programs assemble (default 85)")
ap.add_argument("-v", "--verbose", action="store_true",
                help="list each failing program and its error")
args = ap.parse_args()

out = os.path.join(tempfile.gettempdir(), "mue_examples.bin")

ok = 0
fails = []

for src in sorted(glob.glob(BASE + "/*.asm")):
    try:
        r = subprocess.run([MUE, "-a", src, out],
                           capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        fails.append((os.path.basename(src), "timeout"))
        continue
    if r.returncode == 0:
        ok += 1
        continue

    msg = (r.stderr or r.stdout or "").strip().splitlines()
    msg = msg[-1] if msg else "exit %d" % r.returncode
    # mue prefixes errors with the source path and name; keep just the reason
    msg = msg.replace(BASE + "/", "").replace(BASE + "\\", "")
    name = os.path.basename(src)
    if msg.startswith(name + ":"):
        msg = msg[len(name) + 1:].strip()
    fails.append((name, msg))

total = ok + len(fails)
print("EXAMPLES  assemble=%d  fail=%d  total=%d" % (ok, len(fails), total))

if fails and args.verbose:
    for name, why in fails:
        print("  %-40s %s" % (name, why))

if ok < args.min:
    print("REGRESSION: only %d assembled, expected at least %d" % (ok, args.min))
    sys.exit(1)

sys.exit(0)
