#!/usr/bin/env python3
"""INT 1Ah AH=00: the tick count must come from the host clock.

`FINDINGS.md` 5.5 confirmed that the original reads the host clock at
0x524EC6 and splits it into CX:DX (ticks) plus a midnight flag in AL.  The
clone answered with the constants CX=1234h DX=5678h - the right *shape* and
frozen data, so anything that timed itself or measured elapsed time got a
constant.  Nothing tested it, and `src/dos.c` even carried a comment saying
no headless oracle could distinguish the two, which was not true: the host
clock is right there.

Checks, tolerant of the second rolling over during the run:
  - CX:DX is a plausible tick count for the host's own time of day
    (within a few seconds' worth of ticks).
  - AL comes back 0 (not midnight).
  - two calls made in quick succession agree to within a small delta, so the
    value is live rather than random.
  - a large tick count is *not* the old constant 0x12345678.

Also pins that AH=02 is refused: the original dispatches only AH=00 (the
`[ebp-0x26c]` chain opens with `xor ecx,ecx`) and emu8086's reference lists
only INT 1Ah/00h, so the clone's invented AH=02 arm was removed.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = ".exe" if os.name == "nt" else ""

# 18.2 ticks/second.  Allow a generous window: the guest runs a moment after
# Python reads its own clock, and the run may straddle a second boundary.
TOL_TICKS = 40


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


def host_ticks():
    t = time.localtime()
    secs = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec
    return (secs * 182) // 10


GET_TICKS = """org 100h
mov ah,0
int 1ah
mov si,cx
mov di,dx
mov bp,ax
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
        before = host_ticks()
        rc, out = run(mue, GET_TICKS, d)
        after = host_ticks()

        hi, lo = reg(out, "SI"), reg(out, "DI")
        got = ((hi or 0) << 16) | (lo or 0)

        # A day is 18.2*86400 = 1,572,480 ticks, so CX is normally 0 and the
        # whole count lives in DX.  Compare against the host's window.
        lo_ok = (before - TOL_TICKS) <= got <= (after + TOL_TICKS)
        check("int1ah-ticks-track-host-clock", lo_ok,
              (hex(got), "host", hex(before), hex(after)))
        check("int1ah-not-the-old-constant", got != 0x12345678, hex(got))
        check("int1ah-midnight-flag-clear", (reg(out, "BP") or 0) & 0xFF == 0,
              reg(out, "BP"))

        # Two calls in a row must agree closely - a live counter, not noise.
        rc, out2 = run(mue, """org 100h
mov ah,0
int 1ah
mov si,cx
mov di,dx
mov ah,0
int 1ah
mov bp,cx
mov bx,dx
ret
""", d)
        a2 = ((reg(out2, "SI") or 0) << 16) | (reg(out2, "DI") or 0)
        b2 = ((reg(out2, "BP") or 0) << 16) | (reg(out2, "BX") or 0)
        check("int1ah-two-consecutive-calls-agree", abs(a2 - b2) <= TOL_TICKS,
              (hex(a2), hex(b2)))

        # AH=02 is not dispatched by the original, so it must report and
        # continue rather than inventing date values.
        rc, out3 = run(mue, """org 100h
mov ah,2
int 1ah
mov si,1
ret
""", d)
        check("int1ah-ah02-not-supported", "not supported" in out3,
              out3.strip()[-70:])
        check("int1ah-ah02-continues", reg(out3, "SI") == 1, reg(out3, "SI"))

    print("INT1A %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
