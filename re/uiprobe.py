#!/usr/bin/env python3
"""Scripted evidence run against the running emu8086.

`uicap.py` captures windows one call at a time.  Completing the UI
reverse-engineering needs the *whole* set: every menu popup and every dialog,
each labelled so the captures can be read later without guessing which click
produced them.

This drives one scripted sequence and writes `out/<step>_<n>.png` plus a
JSON manifest describing every window seen at that step.  Menu popups are
separate top-level windows of the same process, so they land in the manifest
as untitled windows - that is how a menu capture is identified.

Three things about driving emu8086 that this had to learn the hard way:

  * the menu bar is NON-CLIENT.  The client rectangle starts *below* it, so a
    click has to be aimed at `client_origin.y - 24 + y`.  `menu=True` does
    that.
  * an open popup keeps the mouse captured.  Clicking the next menu caption
    only closes the popup and does not open the next menu, so a run without a
    dismissal produces captures on every second caption.
  * dismissal must be a CLICK, never Escape.  `keybd_event` delivers to the
    foreground window, which is not necessarily the target, so an Escape can
    type into whatever else is on screen.  `uicap.dismiss()` clicks the
    target's own title bar instead, which is absolute and harmless.

Usage:
    python re/uiprobe.py menus_ed    # editor menu bar popups
    python re/uiprobe.py menus_em    # emulator menu bar popups
    python re/uiprobe.py view        # emulator view menu, every item
    python re/uiprobe.py alu         # the arithmetic & logical unit submenu
    python re/uiprobe.py toolbar     # options / help / about buttons
    python re/uiprobe.py aux         # the emulator's aux button
    python re/uiprobe.py compile     # assembler status window
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uicap

# Caption centres, in CLIENT x, measured from the 738x782 editor capture and
# the 581x443 emulator capture (text-column runs of the menu bar band, minus
# the 8 px window border).  Menu row: client y = 12 is the bar's centre line.
ED_MENUS = [("file", 14), ("edit", 47), ("bookmarks", 97), ("assembler", 168),
            ("emulator", 232), ("math", 286), ("ascii_codes", 340),
            ("help", 397)]

EM_MENUS = [("file", 12), ("math", 50), ("debug", 95), ("view", 138),
            ("external", 185), ("virtual_devices", 256),
            ("virtual_drive", 339), ("help", 396)]

# Editor toolbar icons, CLIENT x, measured from the icon band of the capture.
ED_TOOLBAR = [("new", 29), ("open", 88), ("examples", 147), ("save", 221),
              ("compile", 303), ("emulate", 362), ("calculator", 429),
              ("convertor", 488), ("options", 554), ("help", 614),
              ("about", 673)]

# Emulator bottom row, CLIENT x, measured from the label band of the capture.
EM_BUTTONS = [("screen", 150), ("source", 208), ("reset", 256), ("aux", 307),
              ("vars", 359), ("debug", 415), ("stack", 465), ("flags", 520)]


def focus(h):
    uicap.user32.ShowWindow(h, uicap.SW_RESTORE)
    uicap.user32.SetForegroundWindow(h)
    time.sleep(0.35)
    # A background process cannot always steal activation, so the first click
    # on a window would only focus it and not open the menu.  Clicking the
    # title bar first is harmless and makes activation deterministic.
    x, y, w, _h = uicap.window_rect(h)
    uicap.click_screen(x + 120, y + 12)
    time.sleep(0.45)


def snapshot(pid, outdir, name):
    recs = []
    for i, h in enumerate(uicap.enum_windows(pid)):
        data, w, hgt = uicap.grab(h)
        if not data:
            continue
        fn = os.path.join(outdir, "%s_%d.png" % (name, i))
        open(fn, "wb").write(data)
        cw, ch = uicap.client_rect(h)
        cx, cy = uicap.client_origin(h)
        recs.append(dict(step=name, file=fn, title=uicap.window_text(h),
                         size=[w, hgt], client=[cx, cy, cw, ch]))
    return recs


def shot_after(pid, name, outdir, clicks=None, title="", menu=False,
               pre_wait=1.1):
    """Click a client point (optional), then capture every visible window."""
    if clicks:
        h = uicap.pick_window(pid, title) if title else uicap.enum_windows(pid)[0]
        focus(h)
        ox, oy = uicap.client_origin(h)
        uicap.click_screen(ox + clicks[0], (oy - 24 + clicks[1]) if menu
                           else (oy + clicks[1]))
        time.sleep(pre_wait)
    recs = snapshot(pid, outdir, name)
    if clicks:
        uicap.dismiss(uicap.pick_window(pid, title) if title
                      else uicap.enum_windows(pid)[0])
    return recs


def popup_of(pid, before):
    return [w for w in uicap.enum_windows(pid)
            if w not in before and not uicap.window_text(w)]


def menu_item(pid, outdir, name, title, caption_x, item_y):
    """Open a menu and click one item inside it."""
    h = uicap.pick_window(pid, title)
    if h is None:
        print("no window", title, file=sys.stderr)
        return []
    focus(h)
    ox, oy = uicap.client_origin(h)
    before = set(uicap.enum_windows(pid))
    pop = []
    for _attempt in range(2):
        uicap.click_screen(ox + caption_x, oy - 24 + 12)
        time.sleep(0.8)
        pop = popup_of(pid, before)
        if pop:
            break
    if not pop:
        print("  !! no popup for %s" % name, file=sys.stderr)
        uicap.dismiss(h)
        return []
    px, py, pw, ph = uicap.window_rect(pop[0])
    uicap.click_screen(px + pw // 2, py + item_y)
    time.sleep(1.2)
    return snapshot(pid, outdir, name)


def submenu_item(pid, outdir, name, parent_x, parent_y, child_y):
    """Open a menu, click a parent item, then click inside its submenu."""
    h = uicap.pick_window(pid, "emulator")
    focus(h)
    ox, oy = uicap.client_origin(h)
    before = set(uicap.enum_windows(pid))
    uicap.click_screen(ox + parent_x, oy - 12)
    time.sleep(0.8)
    pop = popup_of(pid, before)
    if not pop:
        print("  !! no popup for %s" % name, file=sys.stderr)
        return []
    px, py, pw, ph = uicap.window_rect(pop[0])
    uicap.click_screen(px + pw // 2, py + parent_y)
    time.sleep(0.9)
    subs = [w for w in uicap.enum_windows(pid)
            if w not in before and w != pop[0]]
    if not subs:
        print("  !! no submenu for %s" % name, file=sys.stderr)
        return []
    sx, sy, sw, sh = uicap.window_rect(subs[0])
    uicap.click_screen(sx + sw // 2, sy + child_y)
    time.sleep(1.3)
    return snapshot(pid, outdir, name)


def close_new_windows(pid, baseline):
    for h in uicap.enum_windows(pid):
        if h not in baseline:
            uicap.post_close(h)
    time.sleep(0.6)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "menus_ed"
    out = sys.argv[2] if len(sys.argv) > 2 else \
        r"C:\Users\acer\AppData\Local\Temp\opencode\probe"
    os.makedirs(out, exist_ok=True)
    pid = uicap.find_pid("emu8086")
    if pid is None:
        print("emu8086 not running", file=sys.stderr)
        return 1
    all_recs = []
    base = set(uicap.enum_windows(pid))

    if mode == "menus_ed":
        for nm, x in ED_MENUS:
            all_recs += shot_after(pid, "ed_" + nm, out, (x, 12), "edit:",
                                   menu=True)
    elif mode == "menus_em":
        for nm, x in EM_MENUS:
            all_recs += shot_after(pid, "em_" + nm, out, (x, 12), "emulator",
                                   menu=True)
    elif mode == "view":
        for nm, y in [("log", 14), ("extval", 44), ("stack", 75),
                      ("variables", 106), ("memory", 131), ("symbols", 160),
                      ("listing", 182), ("source", 213), ("options", 244),
                      ("alu", 275), ("flags", 306), ("lexical", 337),
                      ("ascii", 367), ("screen", 398)]:
            all_recs += menu_item(pid, out, "view_" + nm, "emulator", 138, y)
            close_new_windows(pid, base)
    elif mode == "alu":
        for nm, y in [("memory", 16), ("alu", 35), ("fpu", 57), ("stop", 80),
                      ("symtab", 111), ("listing", 133)]:
            all_recs += submenu_item(pid, out, "alu_" + nm, 138, 275, y)
            close_new_windows(pid, base)
    elif mode == "toolbar":
        for nm, x in ED_TOOLBAR[-3:]:
            all_recs += shot_after(pid, "tb_" + nm, out, (x, 30), "edit:")
            close_new_windows(pid, base)
    elif mode == "toolbar_all":
        for nm, x in ED_TOOLBAR:
            all_recs += shot_after(pid, "tb_" + nm, out, (x, 30), "edit:")
            close_new_windows(pid, base)
    elif mode == "aux":
        for nm, x in EM_BUTTONS:
            all_recs += shot_after(pid, "btn_" + nm, out, (x, 370), "emulator")
            close_new_windows(pid, base)
    elif mode == "compile":
        all_recs += shot_after(pid, "compile", out, (303, 30), "edit:", 2.0)
    elif mode == "editor_view":
        for nm, x, y in [("calculator", 286, 13), ("convertor", 286, 44)]:
            all_recs += menu_item(pid, out, "edmenu_" + nm, "edit:", x, y)
            close_new_windows(pid, base)

    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(all_recs, f, indent=1)
    for r in all_recs:
        print("%-22s %-42s %sx%s" % (r["step"], r["title"][:42], *r["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
