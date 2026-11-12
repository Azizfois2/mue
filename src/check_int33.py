#!/usr/bin/env python3
"""INT 33h (mouse): the four functions emu8086 admits to, checked per register.

emu8086's reference lists INT 33h/0000h-0003h and gives no register contract
for any of them, so the clone's obligation is narrow: accept the call, and
report "no mouse" without damaging registers it was not asked to touch.

This exists because the arm was `cpu.r[2] = 0`, and r[2] is **DX**, not AX -
the register enum is {AX,CX,DX,BX,SP,BP,SI,DI}. A numeric index silently
zeroed the wrong register, so AX kept the function number (a guest could
conclude a mouse was present) and DX was clobbered for no reason.

Checks:
  - 0000h reset answers AX=0 (no mouse installed), which is how DOS says no.
  - 0001h/0002h/0003h leave AX as the caller set it, like a driver with no
    mouse present.
  - DX is NOT clobbered by any of them (the old bug).
  - the call is accepted: the guest continues past it.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = ".exe" if os.name == "nt" else ""


def find_mue():
    for c in (os.path.join(HERE, "mue" + SUFFIX), os.path.join(HERE, "mue")):
        if os.path.exists(c):
            return c
    return None


def winpath(p):
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == "/":
        return p[1].upper() + ":" + p[2:].replace("/", "\\")
    return p


def reg(out, name):
    m = re.search(r"\b" + name + r"=([0-9A-F]{4})", out)
    return int(m.group(1), 16) if m else None


def run(mue, src, cwd):
    a = winpath(os.path.join(cwd, "t.asm"))
    with open(a, "w") as f:
        f.write(src)
    p = subprocess.run([mue, a], stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=60, cwd=cwd)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


# DX is seeded with a sentinel so a clobber is visible; BX with a second
# sentinel for the same reason.
GUEST = """org 100h
mov ax,%(fn)s
mov dx,1234h
mov bx,5678h
int 33h
mov si,ax
mov di,dx
mov bp,bx
ret
"""


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              ("" if cond else " " + str(extra)))
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as d:
        rc, out = run(mue, GUEST % {"fn": "0000h"}, d)
        check("int33-reset-reports-no-mouse", reg(out, "SI") == 0,
              reg(out, "SI"))
        check("int33-reset-no-complaint", "not supported" not in out,
              out.strip()[-60:])

        for fn in ("0001h", "0002h", "0003h"):
            rc, out = run(mue, GUEST % {"fn": fn}, d)
            want = int(fn.rstrip("h"), 16)
            check("int33-%s-leaves-ax" % fn[:4].lower(),
                  reg(out, "SI") == want, (reg(out, "SI"), want))
            check("int33-%s-no-complaint" % fn[:4].lower(),
                  "not supported" not in out, out.strip()[-60:])

        # The regression that motivated this file: DX must survive.  A guest
        # that keeps a pointer in DX would otherwise be silently corrupted.
        for fn in ("0000h", "0001h", "0002h", "0003h"):
            rc, out = run(mue, GUEST % {"fn": fn}, d)
            check("int33-%s-preserves-dx" % fn[:4].lower(),
                  reg(out, "DI") == 0x1234, reg(out, "DI"))
            check("int33-%s-preserves-bx" % fn[:4].lower(),
                  reg(out, "BP") == 0x5678, reg(out, "BP"))

    print("INT33 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
