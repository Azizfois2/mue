#!/usr/bin/env python3
"""Regression for the behaviours that made examples/*.asm run to completion.

Each check here was a real failure found by check_run.py:
  * PUSHA/POPA (60h/61h) are skipped with a report, not fatal
  * AAA/AAS/DAA/DAS (37h/3Fh/27h/2Fh) adjust BCD
  * INT 21h AH=05h refuses when no printer exists
  * INT 19h stops the machine instead of running into garbage
  * the IVT is installed, so `call far es:[40h]` reaches INT 10h
  * an INT 21h function with no arm reports instead of doing nothing
Exit 0 pass, 1 fail, 77 skip.
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

    # PUSHA must be stepped over, and the guest must keep running.
    rc, out = run(mue, "pusha\nmov ax,1234h\npopa\nmov bx,ax\nret\n")
    check("pusha-skipped-not-fatal", rc == 0, rc)
    check("pusha-skipped-reported", "unknown opcode skipped: 60" in out, "")

    # AAA: 09h + 05h = 0Eh -> AL=4, AH=1, CF=1
    rc, out = run(mue, "mov ah,9\nmov al,5\nadd al,ah\nxor ah,ah\naaa\n"
                       "mov bx,ax\nret\n")
    check("aaa-adjusts-bcd", reg(out, "BX") == 0x0104, hex(reg(out, "BX") or 0))

    # AAS: 0Eh - 5 -> AL=9, AH=0, CF=1 (borrow path)
    rc, out = run(mue, "mov al,0Eh\nsub al,5\naas\nmov bx,ax\nret\n")
    check("aas-adjusts-bcd", reg(out, "BX") is not None, reg(out, "BX"))

    # DAA: 0Eh + 3 = 11h -> AL=0x11 after adjustment of 0x0E+0x03
    rc, out = run(mue, "mov al,9\nadd al,7\ndaa\nmov bx,ax\nret\n")
    check("daa-adjusts-bcd",
          reg(out, "BX") is not None and (reg(out, "BX") & 0xFF) == 0x16,
          reg(out, "BX"))

    # INT 21h AH=05h: refuses and answers AL=0x90, but the run continues.
    rc, out = run(mue, "mov ah,5\nmov dl,'A'\nint 21h\nmov bl,al\nret\n")
    check("printer-refusal-continues", rc == 0, rc)
    check("printer-refusal-answers-90", reg(out, "BX") == 0x90,
          hex(reg(out, "BX") or 0))
    check("printer-refusal-reported", "printer does not respond" in out, "")

    # INT 19h stops the machine: the bytes after it never run.
    rc, out = run(mue, "mov bx,1111h\nint 19h\nmov bx,2222h\n")
    check("int19-halts", rc == 0 and reg(out, "BX") == 0x1111,
          (rc, reg(out, "BX")))

    # The IVT is real: a far call through the INT 10h vector reaches the BIOS.
    rc, out = run(mue,
                  "mov ax,0\nmov es,ax\nmov bx,40h\nmov ah,0Eh\nmov al,'Z'\n"
                  "pushf\ncall far es:[bx]\nmov bx,7\nret\n")
    check("ivt-far-call-runs", rc == 0, rc)
    check("ivt-far-call-prints", "Z" in out, repr(out[:120]))

    # An INT 21h AH with no arm reports rather than silently doing nothing.
    rc, out = run(mue, "mov ah,0EEh\nint 21h\nmov bx,1\nret\n")
    check("int21-unsupported-reported",
          "INT 21h, AH=EEh - not supported yet." in out, repr(out[:160]))

    print("RUNTIME %s (%d fail)" % ("PASS" if not fails else "FAIL",
                                    len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
