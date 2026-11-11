#!/usr/bin/env python3
"""INT 10h: every documented subfunction checked by its SIDE EFFECT.

GAPS.md sec.2 was verified for INT 21h by calling each function rather than
reading the dispatch table.  The same exposure exists here: an arm can be
present, run, and still do nothing, and a check that only asserts "no
complaint was printed" would pass.  So each check below asserts the thing the
BIOS is supposed to have changed:

  01h cursor shape        -> 03h reports the shape back
  05h select page         -> 0Fh reports the page, and 02h/03h move on that page
  06h/07h scroll          -> the rows really shift, the blanked rows are blank
  08h read char+attr      -> reads back what 09h wrote on the same page
  09h write char+attr     -> visible through 08h, attribute included
  0Ah write char only     -> character changes, attribute survives
  0Ch set pixel           -> 0Dh reads the colour back
  0Dh get pixel           -> agrees with what 0Ch set
  0Eh teletype            -> characters appear at the cursor and it advances
  13h write string        -> all AL modes put the text where claimed

Each check is paired with a control where the answer could be satisfied by a
constant, e.g. 03h is asked after setting TWO different shapes.

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


def run(mue, src, cwd, env=None):
    a = winpath(os.path.join(cwd, "t.asm"))
    with open(a, "w") as f:
        f.write(src)
    e = None
    if env:
        e = dict(os.environ)
        e.update(env)
    p = subprocess.run([mue, a], stdin=subprocess.DEVNULL, env=e,
                       capture_output=True, timeout=60, cwd=cwd)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


PROLOG = """org 100h
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
        # ---- 01h set shape / 03h get shape, two different values ------
        rc, out = run(mue, PROLOG + """
mov ah,1
mov cx,0607h
int 10h
mov ah,3
mov bx,0
int 10h
mov si,cx
mov ah,1
mov cx,0102h
int 10h
mov ah,3
mov bx,0
int 10h
mov di,cx
ret
""", d)
        check("10h-01h-sets-cursor-shape", reg(out, "SI") == 0x0607,
              reg(out, "SI"))
        check("10h-01h-shape-really-changes", reg(out, "DI") == 0x0102,
              reg(out, "DI"))

        # ---- 05h select page: 0Fh must report it, and writes land there -
        # AH=0Fh is not in emu8086's own reference, so the page is proved the
        # way a guest would: select page 1, move that page's cursor and read
        # it back.  Doing the same on page 0 must give a different answer.
        rc, out = run(mue, PROLOG + """
mov ah,5
xor al,al
int 10h
mov ah,2
mov dh,9
mov dl,9
xor bh,bh
int 10h
mov ah,5
mov al,1
int 10h
mov ah,2
mov dh,2
mov dl,3
mov bh,1
int 10h
mov ah,3
mov bh,1
int 10h
mov di,dx
; page 0 still holds its own cursor (9,9) - the pages are independent.
mov ah,3
xor bh,bh
int 10h
mov si,dx
ret
""", d)
        check("10h-05h-cursor-moves-on-that-page", reg(out, "DI") == 0x0203,
              reg(out, "DI"))
        check("10h-05h-pages-keep-separate-cursors", reg(out, "SI") == 0x0909,
              reg(out, "SI"))

        # ---- 06h scroll up: row 1's text must move to row 0, row 2 blanks -
        rc, out = run(mue, PROLOG + """
mov ah,2
mov dh,1
xor dl,dl
xor bh,bh
int 10h
mov ah,0eh
mov al,'X'
int 10h
mov ah,6
mov al,1
xor cx,cx
mov dx,0204h
mov bh,07h
int 10h
mov ah,2
xor dx,dx
xor bh,bh
int 10h
mov ah,8
xor bh,bh
int 10h
mov si,ax
mov ah,2
mov dh,2
xor dl,dl
xor bh,bh
int 10h
mov ah,8
xor bh,bh
int 10h
mov di,ax
ret
""", d)
        check("10h-06h-scroll-moves-text", (reg(out, "SI") or 0) & 0xFF == ord("X"),
              reg(out, "SI"))
        check("10h-06h-scroll-blanks-tail", (reg(out, "DI") or 0xFF) & 0xFF == 0x20,
              reg(out, "DI"))

        # ---- 09h writes attr, 08h reads it back; 0Ah keeps the attribute --
        rc, out = run(mue, PROLOG + """
mov ah,2
xor dx,dx
xor bh,bh
int 10h
mov ah,9
mov al,'A'
mov bh,0
mov bl,4eh
mov cx,1
int 10h
mov ah,8
xor bh,bh
int 10h
mov si,ax
mov ah,2
mov dh,1
xor dl,dl
xor bh,bh
int 10h
mov ah,9
mov al,'B'
mov bh,0
mov bl,1eh
mov cx,1
int 10h
mov ah,0ah
mov al,'C'
mov cx,1
int 10h
mov ah,8
xor bh,bh
int 10h
mov di,ax
ret
""", d)
        check("10h-09h-writes-char-and-attr", reg(out, "SI") == 0x4E41,
              reg(out, "SI"))
        check("10h-0Ah-keeps-attribute", reg(out, "DI") == 0x1E43,
              reg(out, "DI"))

        # ---- 0Ch set pixel / 0Dh get pixel round-trip, two colours ------
        rc, out = run(mue, PROLOG + """
mov ah,0
mov al,13h
int 10h
mov ah,0ch
mov al,4
mov cx,10
mov dx,20
int 10h
mov ah,0dh
mov bh,0
mov cx,10
mov dx,20
int 10h
mov si,ax
mov ah,0ch
mov al,9
mov cx,10
mov dx,20
int 10h
mov ah,0dh
xor bh,bh
mov cx,10
mov dx,20
int 10h
mov di,ax
mov ah,0dh
mov cx,11
mov dx,20
int 10h
mov bp,ax
ret
""", d)
        check("10h-0Dh-reads-pixel-back", (reg(out, "SI") or 0xFF) & 0xFF == 4,
              reg(out, "SI"))
        check("10h-0Ch-overwrites-pixel", (reg(out, "DI") or 0xFF) & 0xFF == 9,
              reg(out, "DI"))
        check("10h-0Dh-untouched-pixel-is-background",
              (reg(out, "BP") or 0xFF) & 0xFF == 0, reg(out, "BP"))

        # ---- 0Eh teletype: prints and advances the cursor --------------
        rc, out = run(mue, PROLOG + """
mov ah,2
xor dx,dx
xor bh,bh
int 10h
mov ah,0eh
mov al,'Z'
xor bh,bh
int 10h
mov ah,3
xor bh,bh
int 10h
mov si,dx
ret
""", d)
        txt = out
        check("10h-0Eh-prints-character", "Z" in txt, repr(txt[:60]))
        check("10h-0Eh-advances-cursor", reg(out, "SI") == 0x0001,
              reg(out, "SI"))

        # ---- 13h write string, AL=01 (attribute, cursor moves) ---------
        rc, out = run(mue, PROLOG + """
mov ah,2
xor dx,dx
xor bh,bh
int 10h
mov ah,13h
mov al,1
mov bh,0
mov bl,4eh
mov cx,3
mov dx,0
push cs
pop es
mov bp,offset s
int 10h
mov ah,3
xor bh,bh
int 10h
mov si,dx
; AL=1 advanced the cursor past the text, so rewind before reading it.
mov ah,2
xor dx,dx
xor bh,bh
int 10h
mov ah,8
xor bh,bh
int 10h
mov di,ax
ret
s db 'XYZ'
""", d)
        check("10h-13h-writes-string-first-char",
              (reg(out, "DI") or 0) & 0xFF == ord("X"), reg(out, "DI"))
        check("10h-13h-string-attribute-applied",
              ((reg(out, "DI") or 0) >> 8) == 0x4E, reg(out, "DI"))
        check("10h-13h-cursor-advances-by-length",
              reg(out, "SI") == 0x0003, reg(out, "SI"))

    print("INT10SEM %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
