#!/usr/bin/env python3
"""Capture and drive the real emu8086 windows, for UI reverse engineering.

Why this exists: the UI has no byte oracle.  Everything else in this project is
settled by diffing bytes; a window has to be *looked at*.  The documentation
GIFs in `re/ui_ref/` are useful but they are crops from a build we cannot
verify, so the authority is the running `emu8086.exe` on this machine, captured
at its real size with its real fonts.

This tool does three things, each useful on its own:

  * `shot`    - capture every visible top-level window of a process to PNG,
                together with its window/client rectangles (JSON sidecar).
  * `click`   - click at a client-relative point, so scripts can open menus
                and dialogs instead of a human doing it.
  * `sample`  - print the colour at a point of a captured PNG, to settle
                "what grey is the form" with a number instead of an opinion.

Usage:
    python re/uicap.py shot  --exe C:\\emu8086\\emu8086.exe --out re/cap
    python re/uicap.py shot  --proc emu8086 --out re/cap
    python re/uicap.py click --proc emu8086 --title emu8086 --x 363 --y 74
    python re/uicap.py sample --png re/cap/win0.png --points 5,5 100,40

Everything is physical pixels: the process is made DPI-aware before any window
is measured, because otherwise Windows silently scales the numbers and every
measurement in this file would be wrong on a scaled display.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
PW_RENDERFULLCONTENT = 0x00000002
SW_RESTORE = 9
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


class RECT(ctypes.Structure):
    _fields_ = [("left", wt.LONG), ("top", wt.LONG),
                ("right", wt.LONG), ("bottom", wt.LONG)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def dpi_aware():
    """Physical pixels, not Windows' pretend ones."""
    try:
        # PER_MONITOR_AWARE_V2
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass


def window_text(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.GetWindowTextW(hwnd, buf, n + 2)
    return buf.value


def window_pid(hwnd):
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def enum_windows(pid=None):
    out = []

    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _l):
        if not user32.IsWindowVisible(hwnd):
            return True
        if pid is not None and window_pid(hwnd) != pid:
            return True
        r = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if r.right - r.left <= 0 or r.bottom - r.top <= 0:
            return True
        out.append(hwnd)
        return True

    user32.EnumWindows(CB(cb), 0)
    return out


def grab(hwnd):
    """PrintWindow into a DIB; returns (png_bytes, width, height)."""
    from PIL import Image
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None, 0, 0
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
    if not ok:
        gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h          # top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    import io
    o = io.BytesIO()
    img.convert("RGB").save(o, "PNG")
    return o.getvalue(), w, h


def client_rect(hwnd):
    cr = RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    return cr.right - cr.left, cr.bottom - cr.top


def client_origin(hwnd):
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def click_screen(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


VK_ESCAPE = 0x1B


def press_key(vk):
    """Unused: global key injection goes to the FOREGROUND window, which is
    not necessarily the target, so it can type into unrelated applications.
    Kept only so old scripts fail loudly instead of silently."""
    raise RuntimeError("global key injection is unsafe; use dismiss()")


def window_rect(hwnd):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def post_close(hwnd):
    WM_CLOSE = 0x0010
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)


def dismiss(hwnd):
    """Close an open menu by clicking the target's title bar.

    Escape would be the obvious way, but `keybd_event` delivers to whatever
    window owns the foreground, and that is not necessarily the one being
    driven - it can be the terminal running this script. A click is absolute
    on screen, so it can only ever land on the intended window.
    """
    x, y, w, h = window_rect(hwnd)
    click_screen(x + 120, y + 12)


def find_pid(proc):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s.exe" % proc],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p for p in line.split() if p]
        if len(parts) >= 2 and parts[0].lower() == (proc + ".exe").lower():
            return int(parts[1])
    return None


def pick_window(pid, title_sub):
    wins = enum_windows(pid)
    if title_sub:
        for h in wins:
            if title_sub.lower() in window_text(h).lower():
                return h
        return None
    return wins[0] if wins else None


def cmd_shot(a):
    exe = os.path.abspath(a.exe) if a.exe else None
    if exe:
        cmd = [exe] + ([os.path.abspath(a.open)] if a.open else [])
        subprocess.Popen(cmd, cwd=os.path.dirname(exe))
        time.sleep(a.wait)
    pid = find_pid(os.path.splitext(os.path.basename(exe or a.proc))[0])
    if pid is None:
        pid = find_pid(a.proc)
    if pid is None:
        print("no process", a.proc, file=sys.stderr)
        return 1
    # Optional scripted clicks: "x,y[:wait]" repeated, applied to the first
    # window of the process, so a dialog can be opened without a human.
    for spec in a.clicks or []:
        x, y = (int(v) for v in spec.split(":")[0].split(","))
        wait = float(spec.split(":")[1]) if ":" in spec else 1.5
        h = pick_window(pid, a.title or "")
        if h is None:
            print("click: no window", file=sys.stderr)
            break
        user32.ShowWindow(h, SW_RESTORE)
        user32.SetForegroundWindow(h)
        time.sleep(0.4)
        ox, oy = client_origin(h)
        click_screen(ox + x, oy + y)
        print("clicked client(%d,%d) at screen(%d,%d) %r"
              % (x, y, ox + x, oy + y, window_text(h)))
        time.sleep(wait)
    os.makedirs(a.out, exist_ok=True)
    manifest = []
    for i, h in enumerate(enum_windows(pid)):
        data, w, hgt = grab(h)
        if not data:
            continue
        name = os.path.join(a.out, "win%d.png" % i)
        with open(name, "wb") as f:
            f.write(data)
        wr = RECT()
        user32.GetWindowRect(h, ctypes.byref(wr))
        cw, ch = client_rect(h)
        cx, cy = client_origin(h)
        rec = dict(file=name, title=window_text(h), hwnd=h,
                   win=[wr.left, wr.top, wr.right, wr.bottom],
                   client=[cx, cy, cw, ch])
        manifest.append(rec)
        print("saved %s %dx%d %r" % (name, w, hgt, rec["title"]))
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("windows:", len(manifest))
    return 0


def cmd_click(a):
    pid = find_pid(a.proc)
    if pid is None:
        print("no process", a.proc, file=sys.stderr)
        return 1
    h = pick_window(pid, a.title)
    if h is None:
        print("no window matching", a.title, file=sys.stderr)
        return 1
    if a.restore:
        user32.ShowWindow(h, SW_RESTORE)
    user32.ShowWindow(h, SW_RESTORE)
    user32.SetForegroundWindow(h)
    time.sleep(0.5)
    ox, oy = client_origin(h)
    tx, ty = ox + a.x, oy + a.y
    click_screen(tx, ty)
    print("clicked client(%d,%d) -> screen(%d,%d) %r" % (a.x, a.y, tx, ty,
                                                        window_text(h)))
    return 0


def cmd_sample(a):
    from PIL import Image
    im = Image.open(a.png).convert("RGB")
    for spec in a.points:
        x, y = (int(v) for v in spec.split(","))
        print(spec, im.getpixel((x, y)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--virtual", action="store_true",
                    help="stay DPI-unaware, so coordinates are the target's "
                         "own logical pixels (what a VB6 app draws in)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("shot")
    s.add_argument("--exe")
    s.add_argument("--open", help="file to pass on the command line")
    s.add_argument("--proc", default="emu8086")
    s.add_argument("--out", default="re/cap")
    s.add_argument("--wait", type=float, default=4.0)
    s.add_argument("--clicks", nargs="*", help="client x,y[:wait] to click")
    s.add_argument("--title", default="", help="title substring for clicks")
    s.set_defaults(fn=cmd_shot)

    c = sub.add_parser("click")
    c.add_argument("--proc", default="emu8086")
    c.add_argument("--title", default="")
    c.add_argument("--x", type=int, required=True)
    c.add_argument("--y", type=int, required=True)
    c.add_argument("--restore", action="store_true")
    c.set_defaults(fn=cmd_click)

    p = sub.add_parser("sample")
    p.add_argument("--png", required=True)
    p.add_argument("--points", nargs="+", required=True)
    p.set_defaults(fn=cmd_sample)

    a = ap.parse_args()
    if not a.virtual:
        dpi_aware()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
