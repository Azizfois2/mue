#!/usr/bin/env python3
"""INT 21h file services regression: 3Ch/3Dh/3Eh/3Fh/40h/41h/42h/43h/4Eh/4Fh/
56h, and 1Ah (DTA).

Each guest runs in a scratch directory, so the files it creates are the
oracle: the host reads them back.  Derived from FINDINGS 5.1 (3C/3D ->
0x5DDF80, 3E -> 0x5DE530, 42 -> 0x5DED70) plus the DOS contract the original
implements.

Exit 0 pass, 1 fail.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(HERE, "..", "mue.exe"), os.path.join(HERE, "..", "mue")]

PROLOG = "org 100h\n"


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return os.path.abspath(c)
    return None


def run(mue, src, cwd, stdin_data=None):
    """Assemble + run in cwd; return (exit code, combined output)."""
    a = os.path.join(cwd, "t.asm")
    with open(a, "w") as f:
        f.write(src)
    p = subprocess.run([mue, a], input=stdin_data, capture_output=True,
                       timeout=60, cwd=cwd)
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
    body = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("assembled"):
            continue
        if s.startswith("-----"):
            break
        body.append(line.rstrip("\r"))
    return "\n".join(body)


# 3Ch create, 40h write, 3Eh close: handle in BX, flags in DI.
T_CREATE = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fna
int 21h
mov bx,ax
mov ah,40h
mov cx,5
lea dx,msg
int 21h
pushf
pop di
mov ah,3eh
int 21h
ret
fna db 'a.txt',0
msg db 'hello'
"""

# 3Dh open, 3Fh read, then 3Ch/40h the buffer into b.txt: count in SI.
T_READ = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,3fh
mov cx,5
lea dx,buf
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,si
lea dx,buf
int 21h
mov ah,3eh
int 21h
mov ax,0
ret
fna db 'a.txt',0
fnb db 'b.txt',0
buf db 0,0,0,0,0
"""

# 42h: seek to 1 from the start, read 'ell' into b.txt, then ask for the size
# with AL=2, which comes back in DX:AX.
T_SEEK = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,0
mov cx,0
mov dx,1
int 21h
mov ah,3fh
mov cx,3
lea dx,buf
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,si
lea dx,buf
int 21h
mov ah,3eh
int 21h
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,2
mov cx,0
mov dx,0
int 21h
ret
fna db 'a.txt',0
fnb db 'b.txt',0
buf db 0,0,0
"""

# Errors: missing file is AX=2, bad handle is AX=6, both with CF set.
T_ERR = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fnx
int 21h
mov bp,ax
pushf
pop di
mov ax,0
mov ah,3eh
mov bx,30h
int 21h
pushf
pop si
ret
fnx db 'nope.txt',0
"""

# Reading at end of file is not an error: AX=0, CF clear.
T_EOF = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,2
mov cx,0
mov dx,0
int 21h
mov ah,3fh
mov cx,5
lea dx,buf
int 21h
pushf
pop di
ret
fna db 'a.txt',0
buf db 0,0,0,0,0
"""

# b.txt rewritten as 'xyz' (3 bytes) so the find tests have a known size.
T_TWO = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,3
lea dx,msg
int 21h
mov ah,3eh
int 21h
ret
fnb db 'b.txt',0
msg db 'xyz'
"""

# 4Eh/4Fh over *.txt: DTA attribute at +0x15, name at +0x1E, and the third
# call must report no more files (AX=12h, CF set).
T_FIND = PROLOG + """
mov ah,4eh
mov cx,0
lea dx,pat
int 21h
pushf
pop di
mov bx,0
mov bl,[0095h]
mov bh,[009eh]
mov ah,4fh
int 21h
pushf
pop si
mov cl,[009eh]
mov ah,4fh
int 21h
mov bp,ax
pushf
pop dx
ret
pat db '*.txt',0
"""

# A directory is only reported when CX asks for it (bit 10h).
T_FINDDIR = PROLOG + """
mov ah,4eh
mov cx,10h
lea dx,dpat
int 21h
mov bl,[0095h]
pushf
pop di
mov ah,4eh
mov cx,0
lea dx,dpat
int 21h
mov bp,ax
pushf
pop si
ret
dpat db 'sub',0
"""

# 1Ah moves the DTA: the result must land at DS:0200, size at +0x1A.
T_FINDDTA = PROLOG + """
mov ah,1ah
mov dx,200h
int 21h
mov ah,4eh
mov cx,0
lea dx,bpat
int 21h
mov bh,0
mov bl,[021eh]
mov bp,[021ah]
ret
bpat db 'b.txt',0
"""

# 41h deletes a.txt; deleting it again reports file not found.
T_DEL = PROLOG + """
mov ah,41h
lea dx,fna
int 21h
pushf
pop di
mov ah,41h
lea dx,fna
int 21h
pushf
pop si
ret
fna db 'a.txt',0
"""

# Handle 0 is stdin through the keyboard queue: the read stops at CR, keeps
# it, and the second read picks up after it.
T_STDIN = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,8
mov dx,200h
int 21h
mov si,ax
mov ah,3fh
mov bx,0
mov cx,8
mov dx,210h
int 21h
mov bp,ax
mov ah,3ch
mov cx,0
lea dx,f1
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,200h
int 21h
mov ah,3eh
int 21h
mov ah,3ch
mov cx,0
lea dx,f2
int 21h
mov bx,ax
mov ah,40h
mov cx,bp
mov dx,210h
int 21h
mov ah,3eh
int 21h
ret
f1 db 's1.txt',0
f2 db 's2.txt',0
"""

# End of input is not an error: AX=0, CF clear.
T_STDIN_EOF = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,4
mov dx,200h
int 21h
pushf
pop di
ret
"""

# 56h rename: the old name goes away and the new one keeps the contents.
T_RENAME = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fold
int 21h
mov bx,ax
mov ah,40h
mov cx,2
lea dx,msg
int 21h
mov ah,3eh
int 21h
mov ax,cs
mov es,ax
mov ah,56h
lea dx,fold
lea di,fnew
int 21h
pushf
pop bp
ret
fold db 'old.txt',0
fnew db 'new.txt',0
msg db 'hi'
"""

T_RENAME_MISS = PROLOG + """
mov ax,cs
mov es,ax
mov ah,56h
lea dx,fnope
lea di,fx
int 21h
pushf
pop si
ret
fnope db 'nope.txt',0
fx db 'x.txt',0
"""

# 43h: get (AL=0) then set read-only (AL=1) then get again.
T_ATTR = PROLOG + """
mov ah,43h
mov al,0
lea dx,fnew
int 21h
mov bx,cx
pushf
pop di
mov ah,43h
mov al,1
mov cx,1
lea dx,fnew
int 21h
pushf
pop si
mov ah,43h
mov al,0
lea dx,fnew
int 21h
mov bp,cx
ret
fnew db 'new.txt',0
"""

T_ATTR_ERR = PROLOG + """
mov ah,43h
mov al,0
lea dx,fnope
int 21h
mov bp,ax
pushf
pop di
mov ah,43h
mov al,2
lea dx,fnope
int 21h
mov bx,ax
pushf
pop si
ret
fnope db 'nope.txt',0
"""

# Handle 1 is stdout: the bytes must reach the console, not a file.
T_STDOUT = PROLOG + """
mov ah,40h
mov bx,1
mov cx,2
lea dx,hi
int 21h
ret
hi db 'ok'
"""


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

    with tempfile.TemporaryDirectory() as d:
        def host(name):
            try:
                with open(os.path.join(d, name), "rb") as f:
                    return f.read()
            except OSError:
                return None

        rc, out = run(mue, T_CREATE, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        check("create-returns-handle-5", bx == 5, bx if bx is not None else "AX?")
        check("create-write-cf-clear", di is not None and not (di & 1), di)
        check("create-write-hits-host", host("a.txt") == b"hello", host("a.txt"))

        rc, out = run(mue, T_READ, d)
        si = reg(out, "SI")
        check("read-count", si == 5, si)
        check("read-roundtrip", host("b.txt") == b"hello", host("b.txt"))

        rc, out = run(mue, T_SEEK, d)
        dx = reg(out, "DX")
        check("seek-read-from-offset", host("b.txt") == b"ell", host("b.txt"))
        check("seek-end-reports-size", dx == 5, dx)

        rc, out = run(mue, T_ERR, d)
        bp = reg(out, "BP")
        di = reg(out, "DI")
        si = reg(out, "SI")
        ax = reg(out, "AX")
        check("open-missing-file-not-found", bp == 2, bp)
        check("open-missing-sets-cf", di is not None and (di & 1), di)
        check("close-bad-handle-invalid", ax == 6, ax)
        check("close-bad-handle-sets-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_EOF, d)
        ax = reg(out, "AX")
        di = reg(out, "DI")
        check("read-at-eof-returns-zero", ax == 0, ax)
        check("read-at-eof-cf-clear", di is not None and not (di & 1), di)

        rc, out = run(mue, T_TWO, d)
        rc, out = run(mue, T_FIND, d)
        di = reg(out, "DI")
        si = reg(out, "SI")
        bx = reg(out, "BX")
        cx = reg(out, "CX")
        bp = reg(out, "BP")
        dx = reg(out, "DX")
        check("find-first-succeeds", di is not None and not (di & 1), di)
        # The guest puts the DTA attribute in BL and the name's first byte in
        # BH, so BX reads as name<<8 | attribute.
        check("find-first-file-attribute",
              bx is not None and (bx & 0xFF) == 0x20,
              hex(bx) if bx is not None else None)
        check("find-first-name-in-dta",
              bx is not None and (bx >> 8) in (ord("A"), ord("B")),
              hex(bx) if bx is not None else None)
        check("find-next-succeeds", si is not None and not (si & 1), si)
        check("find-next-advances",
              cx is not None and (cx & 0xFF) != ((bx or 0) >> 8),
              hex(cx) if cx is not None else None)
        check("find-next-ends-with-12h", bp == 0x12, bp)
        check("find-next-ends-with-cf", dx is not None and (dx & 1), dx)

        os.mkdir(os.path.join(d, "sub"))
        rc, out = run(mue, T_FINDDIR, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        bp = reg(out, "BP")
        si = reg(out, "SI")
        check("find-directory-needs-10h", bx is not None and (bx & 0xFF) == 0x10,
              hex(bx) if bx is not None else None)
        check("find-directory-first-cf-clear",
              di is not None and not (di & 1), di)
        check("find-directory-without-10h-fails", bp == 0x12, bp)
        check("find-directory-without-10h-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_FINDDTA, d)
        bx = reg(out, "BX")
        bp = reg(out, "BP")
        check("set-dta-redirects", bx is not None and (bx & 0xFF) == ord("B"),
              hex(bx) if bx is not None else None)
        check("find-reports-size", bp == 3, bp)

        rc, out = run(mue, T_DEL, d)
        di = reg(out, "DI")
        si = reg(out, "SI")
        ax = reg(out, "AX")
        check("delete-removes-file", host("a.txt") is None, host("a.txt"))
        check("delete-first-cf-clear", di is not None and not (di & 1), di)
        check("delete-again-not-found", ax == 2, ax)
        check("delete-again-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_STDIN, d, b"hi\r\nrest")
        si = reg(out, "SI")
        bp = reg(out, "BP")
        check("stdin-read-stops-at-cr", si == 3, si)
        check("stdin-keeps-cr", host("s1.txt") == b"hi\r", host("s1.txt"))
        check("stdin-continues-after-cr", bp == 5, bp)
        check("stdin-rest-unconsumed", host("s2.txt") == b"\nrest",
              host("s2.txt"))

        rc, out = run(mue, T_STDIN_EOF, d, b"")
        ax = reg(out, "AX")
        di = reg(out, "DI")
        check("stdin-eof-returns-zero", ax == 0, ax)
        check("stdin-eof-cf-clear", di is not None and not (di & 1), di)

        rc, out = run(mue, T_RENAME, d)
        bp = reg(out, "BP")
        check("rename-succeeds", bp is not None and not (bp & 1), bp)
        check("rename-old-name-gone", host("old.txt") is None, host("old.txt"))
        check("rename-keeps-contents", host("new.txt") == b"hi",
              host("new.txt"))

        rc, out = run(mue, T_RENAME_MISS, d)
        ax = reg(out, "AX")
        si = reg(out, "SI")
        check("rename-missing-not-found", ax == 2, ax)
        check("rename-missing-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_ATTR, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        si = reg(out, "SI")
        bp = reg(out, "BP")
        check("attrs-get-file-archive", bx == 0x20, hex(bx) if bx is not None else None)
        check("attrs-get-cf-clear", di is not None and not (di & 1), di)
        check("attrs-set-succeeds", si is not None and not (si & 1), si)
        check("attrs-readonly-readable", bp is not None and (bp & 1), bp)
        try:                                        # let the cleanup work
            os.chmod(os.path.join(d, "new.txt"), 0o666)
        except OSError:
            pass                                    # the checks already failed

        rc, out = run(mue, T_ATTR_ERR, d)
        bp = reg(out, "BP")
        di = reg(out, "DI")
        bx = reg(out, "BX")
        si = reg(out, "SI")
        check("attrs-missing-not-found", bp == 2, bp)
        check("attrs-missing-cf", di is not None and (di & 1), di)
        check("attrs-bad-subfunction", bx == 1, bx)
        check("attrs-bad-subfunction-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_STDOUT, d)
        txt = guest_text(out)
        check("stdout-handle-writes", "ok" in txt, repr(txt))

    print("INT21 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
