#!/usr/bin/env python3
"""Capture mueui.exe on its FIRST frame, without touching anything.

This exists because a redraw bug shipped once: the eight bottom buttons were
laid out correctly but were not painted until a mouse event reached them, so
they looked missing until clicked. A screenshot taken after an interaction
would have hidden the bug, so this harness deliberately never clicks.

It then checks the things that "look present" implies:
  - the bottom button row has pixels (they are painted on first frame)
  - the screen panel has a visible border and a display area
  - no leftover tick marks in the button band

Exit 0 = first frame looks complete. Exit 1 = something was not painted.
Exit 77 = skipped (no display / binary missing), so CI never false-fails.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "mueui.exe")

try:
    from PIL import ImageGrab, Image
except ImportError:
    print("SKIP: Pillow not installed")
    sys.exit(77)

if not os.path.exists(EXE):
    print("SKIP: %s not built (make ui)" % EXE)
    sys.exit(77)

FAIL = []


def check(ok, msg):
    print(("  ok    " if ok else "  FAIL  ") + msg)
    if not ok:
        FAIL.append(msg)


def find_window(u, want):
    hits = []

    def cb(h, l):
        if u.IsWindowVisible(h):
            n = u.GetWindowTextLengthW(h)
            if n:
                b = ctypes.create_unicode_buffer(n + 1)
                u.GetWindowTextW(h, b, n + 1)
                if b.value == want:
                    r = wt.RECT()
                    u.GetWindowRect(h, ctypes.byref(r))
                    if r.right - r.left > 200:
                        hits.append((r.left, r.top, r.right, r.bottom))
        return True

    CB = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    u.EnumWindows(CB(cb), 0)
    return hits


def main():
    u = ctypes.windll.user32
    p = subprocess.Popen([EXE])
    try:
        # Wait for the toolkit to settle its FIRST frame before grabbing. The
        # point of the check is that the frame is complete WITHOUT any user
        # input, so nothing here clicks - but a grab that races the initial
        # paint reports a false failure.
        time.sleep(4.0)
        hits = find_window(u, "8086 microprocessor emulator")
        if not hits:
            check(False, "emulator window found")
            return 1
        check(True, "emulator window found")

        l, t, r, b = hits[0]
        im = ImageGrab.grab(bbox=(l, t, r, b)).convert("RGB")
        px = im.load()
        w, h = im.size
        print("  capture %dx%d" % (w, h))

        # Title bar height on Windows is ~26px; content coordinates in the
        # C++ layout are measured from below it.
        TITLE = 26

        # Bands as laid out in src/mue_ui.c. Keep these in step with it.
        BANDS = {
            "panels":  (86, 382),
            "flags":   (386, 482),
            "buttons": (490, 516),
            "screen":  (524, 666),
        }

        def white_in(y0, y1):
            return sum(1 for y in range(y0 + TITLE, min(y1 + TITLE, h))
                       for x in range(w) if px[x, y] == (255, 255, 255))

        def ink_in(y0, y1):
            return sum(1 for y in range(y0 + TITLE, min(y1 + TITLE, h))
                       for x in range(w) if sum(px[x, y]) < 300)

        # The buttons are visible only if they were actually painted.
        check(ink_in(*BANDS["buttons"]) > 500,
              "button row painted on first frame (got %d ink px)"
              % ink_in(*BANDS["buttons"]))

        # Each of the eight buttons must have its own label pixels; a collapsed
        # or unpainted button leaves an empty slot. Slots are 88px apart from x=4.
        # Buttons start at the layout margin M=6 and are 84 wide on an 88
        # pitch. Keep these in step with build_emulator().
        empty = []
        for i in range(8):
            x0, x1 = 6 + i * 88, 6 + i * 88 + 84
            if x1 > w:
                continue
            n = sum(1 for y in range(BANDS["buttons"][0] + TITLE,
                                    min(BANDS["buttons"][1] + TITLE, h))
                    for x in range(x0, min(x1, w)) if sum(px[x, y]) < 300)
            if n < 20:
                empty.append(i)
        check(not empty, "all eight buttons have content (empty slots: %s)"
              % (empty or "none"))

        # The screen panel should have a visible frame and a display box.
        check(ink_in(*BANDS["screen"]) > 200,
              "screen panel painted (got %d ink px)" % ink_in(*BANDS["screen"]))

        # Leftover tick marks from an earlier layout would show as isolated
        # dark pixels above the button row's top edge.
        stray = sum(1 for y in range(BANDS["flags"][1] + TITLE,
                                     BANDS["buttons"][0] + TITLE)
                    for x in range(w) if sum(px[x, y]) < 120)
        check(stray < 400, "no stray tick marks between flags and buttons "
                           "(got %d dark px)" % stray)

        # The flags panel must be readable: two rows of cells.
        check(white_in(*BANDS["flags"]) > 300 or ink_in(*BANDS["flags"]) > 300,
              "flags panel has content")

        return 1 if FAIL else 0
    finally:
        p.terminate()


if __name__ == "__main__":
    sys.exit(main())
