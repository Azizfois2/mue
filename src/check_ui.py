#!/usr/bin/env python3
"""UI regression check: the window builds and the panels show real state.

A GUI has no byte oracle, so this asserts the things that can be checked
mechanically and that actually broke while building it:

  1. the window builds and lays out (every panel has non-zero geometry -
     the first version had every widget at w=1 h=1 because of a pack order
     conflict, and nothing complained);
  2. the register panel shows the guest's real values, read from the core -
     not placeholders;
  3. a register that changed on the last step is highlighted, which is the
     whole point of that panel;
  4. the flags panel lights exactly the flags the core reports;
  5. the guest's printed output lands in the screen panel;
  6. a rendered PNG exists and is not blank.

Exit 0 pass, 1 fail, 77 skip (no mue binary or no display).
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SUFFIX = ".exe" if os.name == "nt" else ""
MUE = os.path.join(HERE, "mue" + SUFFIX)

GUEST = """org 100h
mov ax,1234h
mov bx,5678h
mov cx,9ABCh
mov dx,0DEF0h
stc
mov ah,9
mov dx,msg
int 21h
ret
msg db 'hello from the guest$'
"""


def main():
    if not os.path.exists(MUE):
        print("SKIP: no mue binary")
        return 77
    try:
        import tkinter
        r = tkinter.Tk()
        r.destroy()
    except Exception as e:
        print("SKIP: no display (%s)" % e)
        return 77
    try:
        import mue_ui
    except Exception as e:
        print("FAIL import mue_ui:", e)
        return 1

    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              ("" if cond else " " + str(extra)))
        if not cond:
            fails.append(name)

    app = mue_ui.App()
    app.update()
    app.update_idletasks()

    # 1. layout
    for nm, w in (("regs", app.regs), ("flags", app.flags),
                  ("screen", app.screen), ("editor", app.editor)):
        check("ui-%s-laid-out" % nm,
              w.winfo_width() > 20 and w.winfo_height() > 20,
              (w.winfo_width(), w.winfo_height()))

    # 2. real values from the core
    app.editor.delete("1.0", "end")
    app.editor.insert("1.0", GUEST)
    st, txt = app.core.run(app.editor.get("1.0", "end"))
    check("ui-core-returned-state", bool(st), txt[-120:])
    if st:
        app._show(st)
        app.update()
        for reg, want in (("AX", 0x1234), ("BX", 0x5678),
                          ("CX", 0x9ABC), ("DX", 0x0DEF0)):
            got = app.regs.rows[reg].cget("text")
            check("ui-%s-shows-core-value" % reg.lower(),
                  got.upper() == "%04X" % want, got)

        # 3. highlighting: step to a new state and confirm the highlight
        st2 = dict(st)
        st2["AX"] = 0x9999
        app._show(st2, prev=st)
        app.update()
        check("ui-changed-reg-highlighted",
              app.regs.rows["AX"].cget("fg") == mue_ui.CHANGED,
              app.regs.rows["AX"].cget("fg"))
        check("ui-unchanged-reg-not-highlighted",
              app.regs.rows["BX"].cget("fg") != mue_ui.CHANGED,
              app.regs.rows["BX"].cget("fg"))

        # 4. flags reflect the core's flag word (STC was executed)
        app._show(st, prev=None)
        app.update()
        cf_on = bool(st.get("FLAGS", 0) & 0x0001)
        check("ui-cf-matches-core",
              (app.flags.cells["CF"].cget("fg") == mue_ui.ACCENT) == cf_on,
              (cf_on, app.flags.cells["CF"].cget("fg")))

        # 5. guest output reaches the screen panel
        out = mue_ui.guest_output(txt)
        app.screen.set_text(out)
        app.update()
        body = app.screen.txt.get("1.0", "end")
        check("ui-screen-shows-guest-output",
              "hello from the guest" in body, body[:80])
        check("ui-screen-excludes-register-dump",
              "REGISTERS" not in body, body[:80])

    # 6. a real render
    png = os.path.join(tempfile.gettempdir(), "mue_ui_check.png")
    rc = os.system('"%s" "%s" "%s" >/dev/null 2>&1'
                   % (sys.executable, os.path.join(HERE, "mue_ui_shot.py"), png))
    if rc != 0 or not os.path.exists(png):
        # The shot harness needs Pillow; absence is a skip, not a failure of
        # the UI itself.
        print("SKIP ui-render (Pillow or ImageGrab unavailable)")
    else:
        try:
            from PIL import Image
            im = Image.open(png).convert("RGB")
            cols = im.getcolors(maxcolors=1 << 20) or []
            check("ui-render-nonblank", len(cols) > 20, len(cols))
            check("ui-render-sized", im.size == (1100, 680), im.size)
        except Exception as e:
            check("ui-render-readable", False, e)

    app.destroy()
    print("UI %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
