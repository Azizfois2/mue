#!/usr/bin/env python3
"""INT 21h AH=2Ah / AH=2Ch: date and time come from the host clock.

FINDINGS 41.  The original reads VB Now (0x40140C) in both arms and stores
five (2Ah) or five (2Ch) fields; the clone used to return a frozen
2024-01-01 12:00:00, so this checks the value is live and self-consistent.

The checks are deliberately date-agnostic: they never assume a particular day,
only that the fields agree with each other and with the host clock a moment
later.  Pinning a literal date would make the suite expire.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
import datetime
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = ".exe" if os.name == "nt" else ""
MUE = [os.path.join(HERE, "mue" + SUFFIX), os.path.join(HERE, "mue")]


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return c
    return None


def winpath(p):
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == "/":
        return p[1].upper() + ":" + p[2:].replace("/", "\\")
    return p


def run(mue, src):
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        b = winpath(os.path.join(d, "t.bin"))
        open(a, "w").write(src)
        p = subprocess.run([mue, "-a", a, b], capture_output=True, timeout=60)
        if p.returncode != 0 or not os.path.exists(b):
            return 1, (p.stdout + p.stderr).decode("utf-8", "replace")
        p = subprocess.run([mue, "-r", b], capture_output=True, timeout=60)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def reg(out, name):
    for line in out.splitlines():
        for tok in line.strip().split():
            if tok.upper().startswith(name.upper() + "="):
                try:
                    return int(tok.split("=", 1)[1], 16)
                except ValueError:
                    pass
    return None


DATE = """mov ah,2Ah
int 21h
mov si,cx
mov di,dx
mov bp,ax
ret
"""

TIME = """mov ah,2Ch
int 21h
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

    # --- AH=2Ah -------------------------------------------------------
    rc, out = run(mue, DATE)
    cx, dx, ax = reg(out, "SI"), reg(out, "DI"), reg(out, "BP")
    year = cx
    mon, day = (dx >> 8) & 0xFF, dx & 0xFF
    dow = ax & 0xFF

    check("date-cx-is-year", year is not None and 1980 <= year <= 2099, year)
    check("date-month-in-range", mon is not None and 1 <= mon <= 12, mon)
    check("date-day-in-range", day is not None and 1 <= day <= 31, day)
    check("date-day-of-week-in-range", dow is not None and 0 <= dow <= 6, dow)

    # The three fields must name a real calendar date ...
    real = True
    try:
        datetime.date(year, mon, day)
    except (ValueError, TypeError):
        real = False
    check("date-is-a-real-date", real, (year, mon, day))

    # ... and its day-of-week must agree with AL (0 = Sunday).
    if real:
        want_dow = (datetime.date(year, mon, day).weekday() + 1) % 7
        check("date-day-of-week-matches", dow == want_dow, (dow, want_dow))

    # It must be the host's date, not a frozen constant.  Compare against the
    # host on the same day the guest ran; allow a one-day slip for a run that
    # straddles midnight.
    host = datetime.date.today()
    near = real and abs((datetime.date(year, mon, day) - host).days) <= 1
    check("date-tracks-host-clock", near, (year, mon, day, host))

    # --- AH=2Ch -------------------------------------------------------
    rc, out = run(mue, TIME)
    cx, dx, ax = reg(out, "SI"), reg(out, "DI"), reg(out, "BP")
    hh, mm = (cx >> 8) & 0xFF, cx & 0xFF
    ss, hs = (dx >> 8) & 0xFF, dx & 0xFF

    check("time-hour-in-range", hh is not None and 0 <= hh <= 23, hh)
    check("time-minute-in-range", mm is not None and 0 <= mm <= 59, mm)
    check("time-second-in-range", ss is not None and 0 <= ss <= 59, ss)
    check("time-hundredths-in-range", hs is not None and 0 <= hs <= 99, hs)
    check("time-leaves-al-zero", ax is not None and (ax & 0xFF) == 0, ax)

    # The clone used to answer a fixed 12:00:00.  Demand the value is inside a
    # window around the host clock: a frozen constant fails this on any day
    # but one, and a wrong timezone offset fails it always.
    now = datetime.datetime.now()
    got_minutes = hh * 60 + mm
    want_minutes = now.hour * 60 + now.minute
    delta = abs(got_minutes - want_minutes)
    delta = min(delta, 1440 - delta)          # tolerate midnight wrap
    check("time-tracks-host-clock", delta <= 5, (hh, mm, now.hour, now.minute))

    print("INT21TIME %s (%d fail)" % ("PASS" if not fails else "FAIL",
                                      len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
