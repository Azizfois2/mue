#!/usr/bin/env python3
"""Assembler encodings pinned without emu8086 fixtures.

check_asm.py compares against emu8086's own .list files, so it skips where
emu8086 is not installed and only covers the instructions those listings
happen to contain.  The segment-register push/pop bug (FINDINGS 16) slipped
through both gaps - no fixture used the idiom - so those forms are pinned
here, together with a couple of controls that must not change.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(HERE, "..", "mue.exe"), os.path.join(HERE, "..", "mue")]

# source -> expected bytes.  8086 opcode map: 06/07 ES, 0E/0F CS, 16/17 SS,
# 1E/1F DS.  All eight are one byte.
CASES = [
    ("push es",            "06"),
    ("pop es",             "07"),
    ("push cs",            "0e"),
    ("pop cs",             "0f"),
    ("push ss",            "16"),
    ("pop ss",             "17"),
    ("push ds",            "1e"),
    ("pop ds",             "1f"),
    # the standard prologue, as written by examples\int10_13.asm
    ("push cs\npop es",    "0e 07"),
    # controls: these were already correct and must stay that way
    ("mov ax,cs",          "8c c8"),
    ("push ax\npop bx",    "50 5b"),
]


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return c
    return None


def assemble(mue, src):
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, "t.asm")
        b = os.path.join(d, "t.bin")
        with open(a, "w") as f:
            f.write(src + "\n")
        p = subprocess.run([mue, "-a", a, b], capture_output=True, text=True,
                           timeout=60)
        if p.returncode != 0 or not os.path.exists(b):
            return None, (p.stdout + p.stderr).strip()
        with open(b, "rb") as f:
            return f.read(), ""


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    fails = []

    for src, want_hex in CASES:
        want = bytes(int(x, 16) for x in want_hex.split())
        got, err = assemble(mue, src)
        label = src.replace("\n", "; ")
        if got is None:
            print("FAIL %-22s assemble failed: %s" % (label, err[:70]))
            fails.append(label)
        elif got != want:
            print("FAIL %-22s got %s want %s" %
                  (label, got.hex(" "), want.hex(" ")))
            fails.append(label)
        else:
            print("PASS %-22s %s" % (label, got.hex(" ")))

    print("ENCODING %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
