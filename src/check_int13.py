#!/usr/bin/env python3
"""INT 13h regression: reset / read / read-write-readback / persistence / DL.

Builds tiny .asm guests, runs them with `mue -r`, and checks CF/AH and
byte movement both ways. Derived from FINDINGS 5.3/5.6.
Exit 0 pass, 1 fail, 77 skip (no mue binary or no FLOPPY_0).
"""
import os
import struct
import subprocess
import sys
import tempfile


def winpath(p):
    """MinGW mue only understands Windows paths; translate /tmp/... etc."""
    if os.name == "nt":
        return p
    if len(p) > 2 and p[2] == "/":
        return p[0].upper() + ":" + p[2:].replace("/", "\\")
    return p

HERE = os.path.dirname(os.path.abspath(__file__))
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(HERE, "..", "mue.exe"), os.path.join(HERE, "..", "mue")]
FLOPPY = os.environ.get("MUE_FLOPPY") or "C:/emu8086/FLOPPY_0"
EMU = os.environ.get("EMU8086_MYBUILD", "C:/emu8086/MyBuild")


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return c
    return None


def run(mue, src):
    with tempfile.TemporaryDirectory() as d:
        a = winpath(os.path.join(d, "t.asm"))
        b = winpath(os.path.join(d, "t.bin"))
        open(a, "w").write(src)
        # -r runs a raw binary image; assemble to it first with -a.
        p = subprocess.run([mue, "-a", a, b], capture_output=True,
                           text=True, timeout=60)
        if p.returncode != 0 or not os.path.exists(b):
            return "ASSEMBLE-FAIL\n" + p.stdout + p.stderr
        p = subprocess.run([mue, "-r", b], capture_output=True, text=True,
                           timeout=60)
        return p.stdout + p.stderr


def reg(out, name):
    for line in out.splitlines():
        for tok in line.strip().split():
            if tok.upper().startswith(name.upper() + "="):
                try:
                    return int(tok.split("=", 1)[1], 16)
                except ValueError:
                    pass
    return None


T_RESET = ("mov ah,0\nint 13h\npushf\npop bx\nret\n")
T_READ = ("mov ax,1000h\nmov es,ax\nmov bx,0\nmov ah,2\nmov al,1\n"
          "mov ch,0\nmov cl,1\nmov dh,0\nmov dl,0\nint 13h\n"
          "mov si,ax\npushf\npop cx\nmov ax,si\nret\n")
T_BAD = ("mov ah,2\nmov al,1\nmov ch,0\nmov cl,0\nmov dh,0\nmov dl,0\n"
         "int 13h\npushf\npop cx\nmov ax,0\nret\n")
T_ELSE = ("mov ah,1\nint 13h\npushf\npop cx\nmov ax,0\nret\n")

# Sector I/O exercised end to end inside the guest, with no host-file oracle
# (the clone keeps the image in memory; it does not write it back):
#   read  CHS 79/1/18  -> ES:0000      (patterned sector, head 1 on purpose)
#   write ES:0000      -> CHS  0/0/1
#   read  CHS  0/0/1   -> ES:0200
#   compare the two buffers byte by byte
# Observations: DX low bit = CF of the first read, DH = last byte read,
# AX = first two bytes read (AH,AL), BX = mismatch count, BL.
# DL selects the image file (the original builds "FLOPPY_" & DL).  Drive 1
# only exists if someone put a FLOPPY_1 next to the install, so the check
# below asserts the failure only when it is absent.
T_DRIVE1 = ("mov ax,1000h\nmov es,ax\nxor bx,bx\n"
            "mov ah,2\nmov al,1\nmov ch,0\nmov cl,1\nmov dh,0\nmov dl,1\n"
            "int 13h\npushf\npop cx\nmov ax,0\nret\n")

T_RW = (
    "mov ax,1000h\nmov ds,ax\nmov es,ax\n"
    "mov ah,2\nmov al,1\nmov ch,79\nmov cl,18\nmov dh,1\nmov dl,0\n"
    "xor bx,bx\nint 13h\npushf\npop dx\n"
    "mov ah,3\nmov al,1\nmov ch,0\nmov cl,1\nmov dh,0\nmov dl,0\n"
    "xor bx,bx\nint 13h\n"
    "mov ah,2\nmov al,1\nmov ch,0\nmov cl,1\nmov dh,0\nmov dl,0\n"
    "mov bx,200h\nint 13h\n"
    "xor bx,bx\nxor si,si\nmov di,200h\nmov cx,512\n"
    "cmp_loop:\nmov al,[si]\ncmp al,[di]\nje same\ninc bl\n"
    "same:\ninc si\ninc di\ndec cx\njnz cmp_loop\n"
    "mov dh,[511]\nmov ax,[0]\nret\n")


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    if not os.path.exists(FLOPPY):
        print("SKIP: no FLOPPY_0 (set MUE_FLOPPY)")
        return 77
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              (" " + str(extra) if extra and not cond else ""))
        if not cond:
            fails.append(name)

    out = run(mue, T_RESET)
    fl = reg(out, "BX")
    check("reset-clears-cf", fl is not None and not (int(fl) & 1),
          hex(fl) if fl is not None else None)

    with open(FLOPPY, "rb") as f:
        sec0 = f.read(512)
    out = run(mue, T_READ)
    cx = reg(out, "CX")
    ax = reg(out, "AX")
    check("read-cf-clear", cx is not None and not (int(cx) & 1), cx)
    check("read-al-count", ax is not None and (int(ax) & 0xFF) == 1, ax)
    # byte movement is verified by the read/write/readback round trip below

    out = run(mue, T_BAD)
    cx = reg(out, "CX")
    check("bad-sector-sets-cf", cx is not None and bool(int(cx) & 1), cx)

    out = run(mue, T_ELSE)
    cx = reg(out, "CX")
    check("else-sets-cf", cx is not None and bool(int(cx) & 1), cx)

    # Sector I/O end to end, with the pattern on head 1 of the last track.
    # head=1 is load-bearing: DH is the register the clone once read from BH,
    # and on head 0 that mistake is invisible.  The image is a scratch copy so
    # the real FLOPPY_0 is never written, and it is passed via MUE_FLOPPY.
    pat = bytes(((i * 7 + 3) & 0xFF) for i in range(512))
    with tempfile.TemporaryDirectory() as d:
        img = winpath(os.path.join(d, "flop.img"))
        with open(FLOPPY, "rb") as f:
            data = bytearray(f.read(1474560))
        src = (79 * 2 + 1) * 18 + (18 - 1)     # 2879: last sector, head 1
        data[src * 512:(src + 1) * 512] = pat  # source sector
        data[0:512] = b"\x00" * 512           # sector 1 = destination
        with open(img, "wb") as f:
            f.write(data)
        os.environ["MUE_FLOPPY"] = img
        out = run(mue, T_RW)
        with open(img, "rb") as f:
            host = f.read(512)                 # what reached the file

    cx = reg(out, "CX")
    dx = reg(out, "DX")
    ax = reg(out, "AX")
    bx = reg(out, "BX")
    check("rw-cf-clear", dx is not None and not (int(dx) & 1), dx)
    # First two and last bytes of the sector actually read: proves DH selects
    # head 1 (head 0 of that track is zeros in the scratch image).
    check("rw-head-1-data", ax is not None and int(ax) == (pat[1] << 8) | pat[0],
          hex(ax) if ax is not None else None)
    check("rw-last-byte", dx is not None and (int(dx) >> 8) == pat[511],
          hex(dx) if dx is not None else None)
    # Read -> write -> read back must round-trip through the same buffer.
    check("rw-write-readback", bx is not None and int(bx) == 0,
          "mismatches=%s" % (bx & 0xFF if bx is not None else None))
    check("rw-loop-ran", cx is not None and int(cx) == 0, cx)
    # The original opens "FLOPPY_" & DL, Puts the sector and closes, so the
    # bytes must be on disk; the host file is a valid oracle now.
    check("rw-persists-to-image", host == pat,
          "first-diff@%d" % next(
              (i for i, (a, b) in enumerate(zip(host, pat)) if a != b), -1)
          if len(host) == len(pat) else "len %d" % len(host))

    # DL picks the file, so a drive with no image must fail rather than
    # silently reading drive 0.
    elsewhere = [os.path.join(os.path.dirname(FLOPPY), "FLOPPY_1"),
                 "FLOPPY_1"]
    if any(os.path.exists(p) for p in elsewhere):
        print("SKIP drive-1-missing-sets-cf (a FLOPPY_1 is present)")
    else:
        out = run(mue, T_DRIVE1)
        cx = reg(out, "CX")
        check("drive-1-missing-sets-cf",
              cx is not None and bool(int(cx) & 1), cx)

    print("INT13 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
