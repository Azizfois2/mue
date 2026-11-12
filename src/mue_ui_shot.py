#!/usr/bin/env python3
"""Render the mue UI to a PNG without a human looking at it.

The UI has no byte oracle, so the only honest way to check it is to look at
it.  `look_at_image` could not read files in this environment, so this script
does the next best thing: it drives the real App object, pumps Tk's event
loop, and writes a PNG.  A person (or a later visual check) can then inspect
that file, and the CI can at least assert the window builds and is non-blank.

Usage:  python mue_ui_shot.py [out.png]
Exit:   0 = rendered, 1 = failed to build/render.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "ui_shot.png")
    try:
        import mue_ui
    except Exception as e:
        print("FAIL import:", e)
        return 1
    try:
        app = mue_ui.App()
    except Exception as e:
        print("FAIL build:", e)
        return 1

    # Give the window real state so the panels are not all dashes: run the
    # default guest through the verified core.
    try:
        st, txt = app.core.run(app.editor.get("1.0", "end"))
        if st:
            app._show(st)
            app.screen.set_text(mue_ui.guest_output(txt))
            app.status.config(text="finished after %d instructions"
                                    % st.get("ICOUNT", 0))
    except Exception as e:
        print("note: could not populate state:", e)

    app.update_idletasks()
    app.update()
    app.after(300, app.quit)
    app.mainloop()

    ok = False
    try:
        from PIL import ImageGrab
        # Grab the whole desktop and crop to the window.  A bbox grab of the
        # window alone silently loses the right-hand panels on a scaled
        # display: Tk reports a virtual screen size (1536) while the real one
        # is 1920, so the window extends past what a bbox grab believes is
        # there and the register panel came back blank.
        x, y = app.winfo_rootx(), app.winfo_rooty()
        w, h = app.winfo_width(), app.winfo_height()
        im = ImageGrab.grab().crop((x, y, x + w, y + h))
        im.save(out)
        print("wrote", out, im.size)
        ok = True
    except Exception as e:
        print("FAIL grab:", e)
    try:
        app.destroy()
    except Exception:
        pass
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
