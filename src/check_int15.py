#!/usr/bin/env python3
"""INT 15h AH=86h: the wait must actually happen, and must be capped.

`check_run.py` carried a comment saying the delay "is checked by" something,
and nothing checked it.  That is the worst kind of coverage note: it reads
like a guard and is not one.  Remove the sleep, or remove the cap, and every
other test in the suite still passes.

Three properties, each measurable:

  1. A large tick count takes measurable wall time, and the guest continues
     afterwards (AH=86h must not halt or block forever).
  2. The wait is capped, so a request that would be 71 minutes returns in
     about a second.  Without the cap this test would take 71 minutes and the
     suite would hang - so the cap is proved by the suite finishing at all,
     and additionally by the elapsed time staying near the cap.
  3. MUE_NOSLEEP=1 removes the wait, which is how the example sweep stays
     fast.  If the flag stopped working, `make test` would become slow rather
     than red, so it is asserted here directly.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = ".exe" if os.name == "nt" else ""
# The clone caps the wait at 1000 ms.  Allow generous slack for process
# start-up and a loaded machine, but stay far below an uncapped wait.
CAP_MS = 1000.0
SLACK_MS = 4000.0


def find_mue():
    for c in (os.path.join(HERE, "mue" + SUFFIX), os.path.join(HERE, "mue")):
        if os.path.exists(c):
            return c
    return None


def winpath(p):
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == "/":
        return p[1].upper() + ":" + p[2:].replace("/", "\\")
    return p


# CX:DX is microseconds.  0xFFFF:0xFFFF is (~4295 s = 71 min) uncapped.
WAIT_HUGE = """org 100h
mov ah,86h
mov cx,0ffffh
mov dx,0ffffh
int 15h
mov si,1
ret
"""

# A short but non-trivial wait: 0x0004:0x93E0 = 300000 us = 300 ms.
WAIT_300MS = """org 100h
mov ah,86h
mov cx,0004h
mov dx,93e0h
int 15h
mov si,1
ret
"""

# Two waits back to back, to prove the call returns rather than swallowing
# the rest of the program.
WAIT_TWICE = """org 100h
mov ah,86h
mov cx,0
mov dx,1000
int 15h
mov ax,86h
mov cx,0
mov dx,1000
int 15h
mov si,2
ret
"""


def run_timed(mue, src, cwd, env=None):
    a = winpath(os.path.join(cwd, "t.asm"))
    with open(a, "w") as f:
        f.write(src)
    e = None
    if env:
        e = dict(os.environ)
        e.update(env)
    t0 = time.time()
    try:
        p = subprocess.run([mue, a], stdin=subprocess.DEVNULL, env=e,
                           capture_output=True, timeout=180, cwd=cwd)
    except subprocess.TimeoutExpired:
        # An uncapped wait makes the guest sleep for ~71 minutes.  Report that
        # as a very large elapsed time so the cap check fails with a message
        # instead of aborting the whole script.
        return 1e9, "TIMEOUT: the wait was not capped"
    ms = (time.time() - t0) * 1000.0
    return ms, (p.stdout + p.stderr).decode("utf-8", "replace")


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
        # ---- 1. a real wait takes real time and then returns ----------
        ms, out = run_timed(mue, WAIT_300MS, d)
        # 300 ms of sleep; process start-up is tens of ms.  Anything under
        # ~250 ms means the sleep did not happen.
        check("15h-86h-actually-waits", ms >= 250, "%.0f ms" % ms)
        check("15h-86h-returns-to-guest", "SI=0001" in out,
              out.strip()[-80:])

        # ---- 2. the huge request is capped ----------------------------
        ms, out = run_timed(mue, WAIT_HUGE, d)
        check("15h-86h-cap-prevents-71-minute-wait",
              ms <= CAP_MS + SLACK_MS, "%.0f ms" % ms)
        check("15h-86h-capped-wait-still-returns", "SI=0001" in out,
              out.strip()[-80:])
        # The cap must still spend *some* time - a clone that returned
        # immediately would also satisfy the bound above.
        check("15h-86h-cap-is-a-wait-not-a-stub", ms >= 500, "%.0f ms" % ms)

        # ---- 3. MUE_NOSLEEP removes the wait --------------------------
        ms, out = run_timed(mue, WAIT_300MS, d, env={"MUE_NOSLEEP": "1"})
        check("15h-86h-nosleep-is-fast", ms < 250, "%.0f ms" % ms)
        check("15h-86h-nosleep-still-returns", "SI=0001" in out,
              out.strip()[-80:])

        # ---- 4. consecutive waits both work ---------------------------
        ms, out = run_timed(mue, WAIT_TWICE, d)
        check("15h-86h-twice-in-a-row", "SI=0002" in out,
              out.strip()[-80:])

    print("INT15 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
