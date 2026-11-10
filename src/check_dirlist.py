#!/usr/bin/env python3
"""examples/dirlist.asm regression: INT 21h AH=4Eh/4Fh directory walk.

Runs the shipped example in a scratch directory with known contents and
compares the listing it prints.  This is also the end-to-end test for the
DTA layout (FINDINGS 19) and for `shift r,imm` execution (FINDINGS 23).

Exit 0 pass, 1 fail.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASM = os.path.join(ROOT, "examples", "dirlist.asm")
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(ROOT, "mue.exe"), os.path.join(ROOT, "mue")]


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return os.path.abspath(c)
    return None


def run(mue, cwd):
    p = subprocess.run([mue, ASM], capture_output=True, timeout=60, cwd=cwd)
    text = (p.stdout + p.stderr).decode("utf-8", "replace")
    body = []
    for line in text.splitlines():
        if line.strip().startswith("assembled"):
            continue
        if line.strip().startswith("-----"):
            break
        body.append(line.replace("\r", "").strip())
    return p.returncode, text, [b for b in body if b]


def reg(out, name):
    for line in out.splitlines():
        for tok in line.strip().split():
            if tok.upper().startswith(name.upper() + "="):
                try:
                    return int(tok.split("=", 1)[1], 16)
                except ValueError:
                    pass
    return None


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    if not os.path.exists(ASM):
        print("SKIP: examples/dirlist.asm missing")
        return 77
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              (" " + str(extra) if extra and not cond else ""))
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as d:
        for name, data in (("a.txt", b"hello"), ("b.txt", b"xyz"),
                           ("c.dat", b"0123456789"), ("keep.me", b"!")):
            with open(os.path.join(d, name), "wb") as f:
                f.write(data)
        os.mkdir(os.path.join(d, "sub"))

        rc, text, lines = run(mue, d)
        check("lists-all-files", sorted(lines) ==
              ["A.TXT  0005", "B.TXT  0003", "C.DAT  000A", "KEEP.ME  0001"],
              lines)
        check("skips-directories", not any("SUB" in l for l in lines), lines)
        check("names-are-upper-83", all(l.split()[0].isupper() for l in lines),
              lines)
        ax = reg(text, "AX")
        # The guest leaves with INT 21h AH=4Ch, so AX ends as 4C00h and the
        # program stopped rather than running off the end of the loop.
        check("terminates-cleanly", ax == 0x4C00, hex(ax) if ax is not None else None)

    with tempfile.TemporaryDirectory() as d:
        rc, text, lines = run(mue, d)          # empty directory
        check("empty-directory-lists-nothing", lines == [], lines)
        ax = reg(text, "AX")
        check("empty-directory-no-match-exits", ax == 0x4C00,
              hex(ax) if ax is not None else None)

    print("DIRLIST %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
