#!/usr/bin/env python3
"""INT 16h keyboard reads: the key buffer and the scan-code contract.

FINDINGS 40.  The original buffers keys as scan/ASCII pairs at ds:0x621264
(two bytes an entry, scan at +0, ASCII at +1) with a 16-entry cap, and its own
documentation states "INT 16h / AH = 00h ... AH = BIOS scan code. AL = ASCII
character."  The clone used to return AH = 00 for every key and buffer 255.

Exit 0 pass, 1 fail, 77 skip (no mue binary).
"""
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


def run(mue, src, stdin=b""):
    """Assemble and run with the given stdin bytes; return (rc, output)."""
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        b = winpath(os.path.join(d, "t.bin"))
        open(a, "w").write(src)
        p = subprocess.run([mue, "-a", a, b], capture_output=True, timeout=60)
        if p.returncode != 0 or not os.path.exists(b):
            return 1, (p.stdout + p.stderr).decode("utf-8", "replace")
        p = subprocess.run([mue, "-r", b], input=stdin, capture_output=True,
                           timeout=60)
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


# AH=00 then store AX
GET = """mov ah,0
int 16h
mov bx,ax
ret
"""

# AH=01 twice: the value, then again to prove it was not consumed
PEEK = """mov ah,1
int 16h
mov bx,ax
mov ah,1
int 16h
mov cx,ax
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

    # Set-1 scan codes for the keys a terminal can type.  A=1Eh, so K=28h;
    # digits start at 1=02h; Enter is 1Ch, Esc 01h, Tab 0Fh, Space 39h.
    cases = [
        (b"Q", 0x2E51, "Q"),
        (b"a", 0x1E61, "a"),
        (b"K", 0x284B, "K"),
        (b"Z", 0x375A, "Z"),
        (b"0", 0x0B30, "0"),
        (b"1", 0x0231, "1"),
        (b"\r", 0x1C0D, "Enter"),
        (b"\x1b", 0x011B, "Esc"),
        (b"\t", 0x0F09, "Tab"),
        (b" ", 0x3920, "Space"),
    ]
    for stdin, want, label in cases:
        rc, out = run(mue, GET, stdin)
        got = reg(out, "BX")
        check("scan-%s" % label, got == want,
              "%s want %#x got %s" % (label, want,
                                      hex(got) if got is not None else None))

    # AH=01 must report the pair and must not consume it.
    rc, out = run(mue, PEEK, b"K")
    first, second = reg(out, "BX"), reg(out, "CX")
    check("peek-reports-pair", first == 0x284B,
          hex(first) if first is not None else None)
    check("peek-does-not-consume", second == 0x284B,
          hex(second) if second is not None else None)

    # AH=00 consumes: two reads of "AB" give A (scan 1Eh) then B (scan 1Fh).
    rc, out = run(mue, """mov ah,0
int 16h
mov bx,ax
mov ah,0
int 16h
mov cx,ax
ret
""", b"AB")
    b, c = reg(out, "BX"), reg(out, "CX")
    check("get-consumes-in-order", b == 0x1E41 and c == 0x1F42, (b, c))

    print("INT16 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
