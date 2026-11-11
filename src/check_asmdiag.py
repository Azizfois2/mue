#!/usr/bin/env python3
"""Assembler diagnostics: what must be rejected, and what must still assemble.

FINDINGS 43.  The clone used to drop an instruction whose operands it could
not parse, so `mov ax,undefined_sym` produced bytes for everything *except*
that instruction and no diagnostic at all.  Making that an error immediately
exposed several syntax forms the clone never really supported (octal
literals, `b.`/`w.` size prefixes, `cs:sym` destinations, late `size = ...`
definitions), each of which is checked here as a positive case.

Two checks assert bytes, not just success, so "assembles" cannot be satisfied
by silently emitting nothing.

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


def asm(mue, src):
    """Assemble; return (rc, stderr+stdout, image bytes or None)."""
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        b = winpath(os.path.join(d, "t.bin"))
        open(a, "w").write(src)
        p = subprocess.run([mue, "-a", a, b], capture_output=True, timeout=60)
        out = (p.stdout + p.stderr).decode("utf-8", "replace")
        data = open(b, "rb").read() if os.path.exists(b) else None
        # Only report text after the line number, so temp paths do not leak
        # into assertions.
        msg = out.strip().split(": ", 1)[-1] if out.strip() else ""
        return p.returncode, msg, data


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

    # ---- things that must be REJECTED ---------------------------------
    rc, msg, data = asm(mue, "org 100h\nmov ax, undefined_sym\nret\n")
    check("rejects-undefined-symbol", rc != 0, (rc, msg))
    check("reports-undefined-symbol",
          "undefined variable" in msg or "can not be evaluated" in msg, msg)

    rc, msg, data = asm(mue, "org 100h\nmov al, 300\nret\n")
    check("rejects-byte-overflow", rc != 0, (rc, msg))
    check("reports-overflow", "overflow" in msg, msg)

    rc, msg, data = asm(mue, "org 100h\nmov ax, 99999999999h\nret\n")
    check("rejects-wide-hex", rc != 0, (rc, msg))

    # A far-too-wide value in a data directive must also fail rather than
    # silently truncate.
    # A value too wide for the directive is a data error, reported by the
    # db/dw path rather than the immediate path.
    rc, msg, data = asm(mue, "org 100h\ndb 12345\nret\n")
    check("rejects-wide-db", rc != 0 and "overflow" in msg, (rc, msg))

    # ---- things that must still ASSEMBLE, with correct bytes ----------
    cases = [
        # (name, source, expected byte prefix, note)
        ("accepts-octal-literal",
         "org 100h\nmov cl, 10o\nret\n", b"\xb1\x08",
         "10o == 8 (examples/5_sample.asm)"),
        ("accepts-cs-symbol-dest",
         "org 100h\nmov cs:flag, 0\nret\nflag dw 0\n", b"\x2e\xc6\x06",
         "MOV CS:make_minus,0 uses C6 (imm8) for a value that fits a byte"),
        ("accepts-byte-prefix",
         "org 100h\nmov al, b.val\nret\nval dw 0\n", b"\xa0",
         "b.temp reads the byte of a word"),
        ("accepts-word-prefix",
         "org 100h\nmov w.val, ax\nret\nval dw 0\n", b"\xa3",
         "w. week writes the word at week (space optional)"),
        ("accepts-late-equ",
         "org 100h\nmov cx, size\nbuf db 1,2\nsize = ($ - buf) / 2\nret\n",
         b"\xb9\x01\x00", "size defined after use -> mov cx, imm16, not a load"),
        ("accepts-dd-doubleword",
         "org 100h\njmp a\nx dd 12345678h\na: ret\n", None,
         "dd must keep all 32 bits"),
        ("accepts-word-value-in-symbol",
         "org 100h\nfoo dw ?\nmov foo, 300\nret\n", None,
         "a bare dw symbol holds more than 127"),
    ]
    for name, src, want, note in cases:
        rc, msg, data = asm(mue, src)
        ok = rc == 0 and data is not None
        if ok and want is not None:
            ok = data.startswith(want)
        check(name, ok, (rc, msg[:70],
                         data[:6].hex() if data else None, "want", 
                         want.hex() if want else None))

    print("ASMDIAG %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
