#!/usr/bin/env python3
"""Regression for the gaps listed in re/GAPS.md.

Each check asserts a service the clone used to report as unsupported now runs
and leaves the documented answer.  Where the contract is a value (2Eh/54h,
23h, 33h, 57h) the check compares it; where the contract is "no effect and no
diagnostic" (INT 10h AH=11h, 0Ch/0Dh in text mode) it asserts stderr is quiet.

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


def run(mue, src, workdir=None, stdin=b""):
    """Assemble and run with workdir as cwd; return (rc, out, err)."""
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        b = winpath(os.path.join(d, "t.bin"))
        open(a, "w").write(src)
        p = subprocess.run([mue, "-a", a, b], capture_output=True, timeout=60)
        if p.returncode != 0 or not os.path.exists(b):
            return 1, "", (p.stdout + p.stderr).decode("utf-8", "replace")
        p = subprocess.run([mue, "-r", b], input=stdin, capture_output=True,
                           timeout=60, cwd=workdir)
        return (p.returncode,
                p.stdout.decode("utf-8", "replace"),
                p.stderr.decode("utf-8", "replace"))


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
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              ("" if cond else " " + str(extra)))
        if not cond:
            fails.append(name)

    # --- INT 21h AH=2Eh set / AH=54h get verify flag round-trips ----------
    rc, out, err = run(mue, """org 100h
mov ah,2Eh
mov al,1
int 21h
mov ah,54h
int 21h
mov si,ax
ret
""")
    al = reg(out, "SI") & 0xFF if reg(out, "SI") is not None else None
    check("verify-flag-round-trips", al == 1, al)
    check("verify-flag-no-report", "not supported" not in err, err.strip()[:60])

    # Default is 0, and setting 1 then 0 must clear it.
    rc, out, err = run(mue, """org 100h
mov ah,54h
int 21h
mov si,ax
mov ah,2Eh
xor al,al
int 21h
mov ah,54h
int 21h
mov di,ax
ret
""")
    first, second = reg(out, "SI"), reg(out, "DI")
    check("verify-flag-default-zero",
          first is not None and (first & 0xFF) == 0, first)
    check("verify-flag-clears",
          second is not None and (second & 0xFF) == 0, second)

    # --- INT 21h AH=23h get break flag ---------------------------------
    rc, out, err = run(mue, """org 100h
mov ax,23h
int 21h
mov si,ax
ret
""")
    check("break-flag-zero", reg(out, "SI") is not None and
          (reg(out, "SI") & 0xFF) == 0, reg(out, "SI"))
    check("break-flag-no-report", "not supported" not in err, err.strip()[:60])

    # --- INT 21h AH=33h get/set Ctrl-Break ------------------------------
    rc, out, err = run(mue, """org 100h
mov ax,3301h
mov dl,1
int 21h
mov ax,3300h
int 21h
mov si,dx
ret
""")
    check("ctrl-break-round-trips", reg(out, "SI") is not None and
          (reg(out, "SI") & 0xFF) == 1, reg(out, "SI"))

    # --- INT 21h AH=17h rename -----------------------------------------
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "a.txt"), "w").write("hello")
        rc, out, err = run(mue, """org 100h
mov ah,17h
mov dx,offset o
mov di,offset n2
int 21h
pushf
pop bp
mov si,1
ret
o db "a.txt",0
n2 db "b.txt",0
""", workdir=d)
        cf = (reg(out, "BP") & 1) if reg(out, "BP") is not None else None
        check("rename-clears-cf", cf == 0, cf)
        check("rename-moved-file",
              os.path.exists(os.path.join(d, "b.txt")) and
              not os.path.exists(os.path.join(d, "a.txt")),
              sorted(os.listdir(d)))
        check("rename-preserved-bytes",
              os.path.exists(os.path.join(d, "b.txt")) and
              open(os.path.join(d, "b.txt")).read() == "hello", "")

    # Renaming a file that is not there must fail with CF set.
    with tempfile.TemporaryDirectory() as d:
        rc, out, err = run(mue, """org 100h
mov ah,17h
mov dx,offset o
mov di,offset n2
int 21h
pushf
pop bp
ret
o db "nope.txt",0
n2 db "x.txt",0
""", workdir=d)
        cf = (reg(out, "BP") & 1) if reg(out, "BP") is not None else None
        check("rename-missing-sets-cf", cf == 1, cf)

    # --- INT 21h AH=57h get file date/time -----------------------------
    with tempfile.TemporaryDirectory() as d:
        rc, out, err = run(mue, """org 100h
mov ah,3Ch
xor cx,cx
mov dx,offset f
int 21h
mov bx,ax
mov ah,57h
mov al,0
int 21h
mov si,cx
mov di,dx
mov ah,3Eh
int 21h
ret
f db "t.txt",0
""", workdir=d)
        cx, dx = reg(out, "SI"), reg(out, "DI")
        mtime = os.path.getmtime(os.path.join(d, "t.txt"))
        tm = datetime.datetime.fromtimestamp(mtime)
        want_d = (((tm.year - 1980) & 0x7F) << 9) | ((tm.month & 0x0F) << 5) | \
                 (tm.day & 0x1F)
        got_d = dx
        check("filedate-packed-date", got_d == want_d,
              "%s want %#x got %s" % (tm, want_d,
                                      hex(got_d) if got_d is not None else None))
        # Decode back: year/month/day must be a real date near today.
        if got_d is not None:
            y = 1980 + ((got_d >> 9) & 0x7F)
            m = (got_d >> 5) & 0x0F
            day = got_d & 0x1F
            ok = True
            try:
                datetime.date(y, m, day)
            except ValueError:
                ok = False
            check("filedate-decodes-to-real-date", ok, (y, m, day))
        check("filedate-no-report", "not supported" not in err, err.strip()[:60])

    # Bad handle must fail, not crash.
    rc, out, err = run(mue, """org 100h
mov ah,57h
mov al,0
mov bx,0FFh
int 21h
pushf
pop bp
ret
""")
    cf = (reg(out, "BP") & 1) if reg(out, "BP") is not None else None
    check("filedate-bad-handle-sets-cf", cf == 1, cf)

    # --- INT 21h AH=0Ch clears the key buffer and re-dispatches on AH=AL --
    # Not a file flush: 0x5275DA zeroes ds:0x621270, and 0x527749 re-enters
    # the INT 21h dispatcher unless AL is one of 06/07/08/0A.
    rc, out, err = run(mue, """org 100h
mov ax,0C30h
int 21h
mov si,ax
ret
""")
    si = reg(out, "SI")
    check("flush-0Ch-reinvokes-AH-AL", si is not None and (si & 0xFF) == 5, si)
    check("flush-0Ch-no-report", "not supported" not in err, err.strip()[:60])

    # AL=09h must actually print, proving the re-dispatch ran.
    rc, out, err = run(mue, """org 100h
mov ax,0C09h
mov dx,offset m
int 21h
ret
m db "INVOKED$"
""")
    check("flush-0Ch-runs-09h", "INVOKED" in out, out[:60])

    # The direct-console services (06/07/08/0A) are deliberately NOT
    # re-entered, because reading a key after clearing the buffer is wrong.
    rc, out, err = run(mue, """org 100h
mov ax,0C06h
int 21h
mov si,ax
ret
""", stdin=b"Q")
    si = reg(out, "SI")
    check("flush-0Ch-skips-06h", si is not None and (si & 0xFF00) == 0x0C00, si)

    # --- INT 21h AH=0Dh is a pure no-op (`jmp 0x52a52a`) -----------------
    rc, out, err = run(mue, """org 100h
mov ax,0D09h
mov dx,offset m
int 21h
ret
m db "MUST-NOT-PRINT$"
""")
    check("disk-reset-0Dh-does-nothing", "MUST-NOT-PRINT" not in out, out[:60])
    check("disk-reset-0Dh-no-report", "not supported" not in err,
          err.strip()[:60])

    # --- INT 10h AH=11h is a silent no-op in the original ----------------
    rc, out, err = run(mue, """org 100h
mov ax,1130h
int 10h
mov bp,1
ret
""")
    check("int10-11-silent", "not supported" not in err, err.strip()[:60])
    check("int10-11-keeps-running", reg(out, "BP") == 1, reg(out, "BP"))

    # --- INT 10h AH=0Ch/0Dh in text mode: quiet --------------------------
    rc, out, err = run(mue, """org 100h
mov ax,0C05h
mov cx,10
mov dx,10
xor bh,bh
int 10h
mov bp,1
ret
""")
    check("int10-0C-text-silent", "not supported" not in err, err.strip()[:60])

    # --- INT 10h AH=0Dh returns the pixel in AL -------------------------
    rc, out, err = run(mue, """org 100h
mov ax,13h
int 10h
mov ax,0C09h
mov cx,5
mov dx,5
int 10h
mov ax,0D05h
int 10h
mov si,ax
ret
""")
    si = reg(out, "SI")
    check("int10-0D-reads-pixel-back", si is not None and (si & 0xFF) == 9, si)

    # Reading an unwritten pixel must be 0, not the old AL.
    rc, out, err = run(mue, """org 100h
mov ax,13h
int 10h
mov ax,0D07h
mov cx,100
mov dx,100
int 10h
mov si,ax
ret
""")
    si = reg(out, "SI")
    check("int10-0D-blank-pixel-zero", si is not None and (si & 0xFF) == 0, si)

    print("GAPS %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
