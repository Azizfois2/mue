#!/usr/bin/env python3
"""INT 10h regression: AH=13h (advance, attributes, CX limit), cursor,
cell buffer behind AH=08/09/0A, and AH=06 scroll.

Builds tiny .asm guests, runs them with `mue`, and reads the results back out
of the register dump. Derived from FINDINGS 5.2.

Each guest sets ES:BP (via `mov ax,cs` - `push cs`/`pop es` is a separate,
already-reported assembler defect) and then calls AH=13h, prints a '|' marker
through AH=0Eh, and finally reads the cursor with AH=03h so the resulting
position lands in DX for the dump.

Exit 0 pass, 1 fail.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(HERE, "..", "mue.exe"), os.path.join(HERE, "..", "mue")]

PROLOG = "org 100h\nmov ax,cs\nmov es,ax\n"
# Read the cursor before printing the marker: AH=0Eh advances it too, so the
# marker would otherwise add a column to what the check measures.  AH=0Eh does
# not touch DX, so the position survives for the dump.
EPILOG = ("mov ah,03h\nmov bh,0\nint 10h\n"        # cursor -> DX
          "mov ah,0eh\nmov al,'|'\nint 10h\n"      # marker
          "ret\n")


def winpath(p):
    if os.name == "nt":
        return p
    return p


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return c
    return None


def run(mue, src):
    """Assemble and run; return (exit code, combined output).

    Output is captured as bytes and decoded leniently: a guest that leaks an
    attribute byte to the console emits arbitrary non-UTF-8 bytes, and that
    has to show up as a failed assertion rather than a crash """
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        with open(a, "w") as f:
            f.write(src)
        p = subprocess.run([mue, a], capture_output=True, timeout=60)
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


def guest_text(out):
    """Everything the guest printed: after the assemble banner, before the dump."""
    body = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("assembled"):
            continue
        if s.startswith("-----"):
            break
        body.append(line.rstrip("\r"))
    return "\n".join(body)


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              (" " + str(extra) if extra and not cond else ""))
        if not cond:
            fails.append(name)

    # 1. AL=1: advance, one byte per cell. 12 chars from row 1 col 2.
    rc, out = run(mue, PROLOG +
                  "mov ah,13h\nmov al,1\nmov bh,0\nmov bl,0f3h\n"
                  "mov cx,12\nmov dx,0102h\nlea bp,msg\nint 10h\n" +
                  EPILOG + "msg db 'hello world!'\n")
    txt = guest_text(out)
    check("adv-prints-string", txt == "hello world!|", repr(txt))
    dx = reg(out, "DX")
    check("adv-moves-cursor", dx == 0x010E, hex(dx) if dx is not None else None)

    # 2. AL=3: advance + attribute pairs. CX counts cells, so 24 bytes are
    # consumed but only the 12 characters are printed (attribute bytes must
    # not leak to the console).
    rc, out = run(mue, PROLOG +
                  "mov ah,13h\nmov al,3\nmov bh,0\nmov bl,0f3h\n"
                  "mov cx,12\nmov dx,0302h\nlea bp,cmsg\nint 10h\n" +
                  EPILOG +
                  "cmsg db 'h',0cfh,'e',8bh,'l',0f0h,'l',5fh,'o',3ch,' ',0e0h\n"
                  "     db 'w',0b3h,'o',2eh,'r',0cah,'l',1ah,'d',0ach,'!',2fh\n")
    txt = guest_text(out)
    check("attr-skips-attribute-bytes", txt == "hello world!|", repr(txt))
    dx = reg(out, "DX")
    check("attr-moves-cursor", dx == 0x030E, hex(dx) if dx is not None else None)

    # 3. AL=0: write only, cursor stays where it was put.
    rc, out = run(mue, PROLOG +
                  "mov ah,13h\nmov al,0\nmov bh,0\nmov bl,0f3h\n"
                  "mov cx,6\nmov dx,0505h\nlea bp,msg\nint 10h\n" +
                  EPILOG + "msg db 'abcdef'\n")
    txt = guest_text(out)
    check("noadv-prints-string", txt == "abcdef|", repr(txt))
    dx = reg(out, "DX")
    check("noadv-keeps-cursor", dx == 0x0505, hex(dx) if dx is not None else None)

    # 4. AH=0Eh teletype moves the cursor: printable advances one column,
    # CR returns to column 0, LF drops a row.
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,0205h\nint 10h\n"
                       "mov ah,0eh\nmov al,'a'\nint 10h\n"
                       "mov ah,0eh\nmov al,'b'\nint 10h\n"
                       "mov ah,03h\nint 10h\nret\n")
    dx = reg(out, "DX")
    check("teletype-advances", dx == 0x0207, hex(dx) if dx is not None else None)
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,0240h\nint 10h\n"
                       "mov ah,0eh\nmov al,0dh\nint 10h\n"
                       "mov ah,0eh\nmov al,0ah\nint 10h\n"
                       "mov ah,03h\nint 10h\nret\n")
    dx = reg(out, "DX")
    check("teletype-cr-lf", dx == 0x0300, hex(dx) if dx is not None else None)

    # 5. AH=02 sets the cursor, AH=03 reads it back.
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,0a14h\nint 10h\n"
                       "mov ah,03h\nmov bh,0\nint 10h\nret\n")
    dx = reg(out, "DX")
    cx = reg(out, "CX")
    check("set-read-cursor", dx == 0x0A14, hex(dx) if dx is not None else None)
    check("read-cursor-shape", cx == 0x0607, hex(cx) if cx is not None else None)

    # 6. AH=09 writes char+attribute and AH=0A writes the character only, so
    # AH=08 must report BL's attribute for 09 and the cell's existing one for
    # 0A.  This is what the cell buffer is for: the attribute byte has nowhere
    # to live on a stream console.
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,050ah\nint 10h\n"
                       "mov ah,09h\nmov al,'A'\nmov bh,0\nmov bl,1eh\n"
                       "mov cx,1\nint 10h\n"
                       "mov ah,08h\nmov bh,0\nint 10h\n"
                       "mov bx,ax\nret\n")
    bx = reg(out, "BX")
    check("attr-write-read", bx == 0x1E41, hex(bx) if bx is not None else None)
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,050ah\nint 10h\n"
                       "mov ah,09h\nmov al,'A'\nmov bh,0\nmov bl,1eh\n"
                       "mov cx,1\nint 10h\n"
                       "mov ah,0ah\nmov al,'B'\nmov bh,0\nmov bl,0\n"
                       "mov cx,1\nint 10h\n"
                       "mov ah,08h\nmov bh,0\nint 10h\n"
                       "mov bx,ax\nret\n")
    bx = reg(out, "BX")
    check("char-write-keeps-attr", bx == 0x1E42,
          hex(bx) if bx is not None else None)

    # 7. Everything else that reaches the screen fills a cell too: AH=0Eh uses
    # attribute 0x07, AH=13h uses the attribute byte from its string, and a
    # whole-screen AH=06 scroll blanks the window to (BH, space).
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,0200h\nint 10h\n"
                       "mov ah,0eh\nmov al,'Q'\nint 10h\n"
                       "mov ah,02h\nmov bh,0\nmov dx,0200h\nint 10h\n"
                       "mov ah,08h\nmov bh,0\nint 10h\n"
                       "mov bx,ax\nret\n")
    bx = reg(out, "BX")
    check("teletype-fills-cell", bx == 0x0751,
          hex(bx) if bx is not None else None)
    rc, out = run(mue, PROLOG +
                  "mov ah,13h\nmov al,2\nmov bh,0\nmov bl,0\n"
                  "mov cx,2\nmov dx,0400h\nlea bp,cmsg\nint 10h\n"
                  "mov ah,02h\nmov bh,0\nmov dx,0401h\nint 10h\n"
                  "mov ah,08h\nmov bh,0\nint 10h\n"
                  "mov bx,ax\nret\n"
                  "cmsg db 'A',1eh,'B',4eh\n")
    bx = reg(out, "BX")
    check("string-attr-reaches-cell", bx == 0x4E42,
          hex(bx) if bx is not None else None)
    rc, out = run(mue, "mov ah,02h\nmov bh,0\nmov dx,0a0ah\nint 10h\n"
                       "mov ah,09h\nmov al,'Z'\nmov bh,0\nmov bl,4eh\n"
                       "mov cx,1\nint 10h\n"
                       "mov ah,06h\nmov al,0\nmov bh,07h\n"
                       "mov cx,0\nmov dx,184fh\nint 10h\n"
                       "mov ah,02h\nmov bh,0\nmov dx,0a0ah\nint 10h\n"
                       "mov ah,08h\nmov bh,0\nint 10h\n"
                       "mov bx,ax\nret\n")
    bx = reg(out, "BX")
    check("scroll-blanks-cells", bx == 0x0720,
          hex(bx) if bx is not None else None)

    # 8. CX above 0x7D0 is refused and halts, as the original does
    # (0x522F70 -> log + [obj+0x7AC] halt).  Nothing is written, and the
    # marker after the call never runs.
    rc, out = run(mue, PROLOG +
                  "mov ah,13h\nmov al,1\nmov cx,07d1h\nmov dx,0\n"
                  "lea bp,msg\nint 10h\n" +
                  EPILOG + "msg db 'abc'\n")
    txt = guest_text(out)
    check("cx-limit-writes-nothing", txt == "", repr(txt))
    check("cx-limit-halts", rc != 0, rc)
    check("cx-limit-diagnostic", "above the allowed limit" in out, repr(out[-200:]))

    print("INT10 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
