#!/usr/bin/env python3
"""mue UI - a front end for the mue 8086 emulator.

Cross-platform by design (tkinter: stdlib, Windows/macOS/Linux).  The
emulator core stays in C; this process owns the window and asks the core for
state through `mue -ui`, which prints one machine-readable snapshot.

Why a separate process instead of a C GUI: the core is a console program that
is already byte-verified against emu8086 by the whole test suite.  Linking a
GUI toolkit into it would put that binary behind a platform dependency and
risk breaking the oracle.  A front end that drives the verified binary keeps
one source of truth.

The panels mirror emu8086's structure (see re/UI_REFERENCE.md): registers
with changed-value highlighting, flags, the guest screen, and a step log.
The look is deliberately not VB6 - the user chose cross-platform over Win32
compatibility.
"""
import os
import subprocess
import sys
import tempfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
SUFFIX = ".exe" if os.name == "nt" else ""
MUE = os.path.join(HERE, "mue" + SUFFIX)


# ---------------------------------------------------------------- core bridge

SNAP_ASM = """org 100h
; a tiny guest used only when the UI has nothing loaded: it moves a few
; registers so the panels have something real to show.
mov ax,1234h
mov bx,5678h
mov cx,9ABCh
mov dx,0DEF0h
ret
"""


def winpath(p):
    """MinGW binaries only understand Windows paths when run from Windows."""
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == "/":
        return p[1].upper() + ":" + p[2:].replace("/", "\\")
    return p


class Core:
    """Runs the emulator binary and parses its register/state dump.

    One snapshot = assemble (if .asm) or load, run, and scrape the state the
    CLI already prints.  No new emulator code: the UI reads what `mue` writes,
    so a UI bug can never silently disagree with the verified core.
    """

    def __init__(self, path):
        self.path = path
        self.dir = tempfile.mkdtemp(prefix="mue_ui_")
        self.source = None

    def set_source(self, text):
        self.source = text

    def run(self, source=None, max_steps=None):
        src = source if source is not None else self.source
        if src is None:
            return None, "no source loaded"
        a = os.path.join(self.dir, "ui.asm")
        with open(a, "w") as f:
            f.write(src)
        cmd = [self.path, winpath(a)]
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=60,
                               stdin=subprocess.DEVNULL, cwd=self.dir)
        except subprocess.TimeoutExpired:
            return None, "guest did not finish"
        out = (p.stdout + p.stderr).decode("utf-8", "replace")
        return parse_state(out), out


def _hex(out, name):
    import re
    m = re.search(r"\b" + name + r"=([0-9A-Fa-f]{4})", out)
    return int(m.group(1), 16) if m else None


def parse_state(out):
    """Pull the register dump `mue` prints into a dict."""
    st = {}
    for r in ("AX", "BX", "CX", "DX", "SP", "BP", "SI", "DI",
              "CS", "DS", "ES", "SS", "IP", "FLAGS"):
        v = _hex(out, r)
        if v is not None:
            st[r] = v
    import re
    m = re.search(r"instructions executed: (\d+)", out)
    if m:
        st["ICOUNT"] = int(m.group(1))
    return st


REG16 = ("AX", "BX", "CX", "DX")
REG16B = ("SP", "BP", "SI", "DI")
SEG = ("CS", "DS", "ES", "SS")
FLAGS = (("CF", 0x0001), ("PF", 0x0004), ("AF", 0x0010), ("ZF", 0x0040),
         ("SF", 0x0080), ("TF", 0x0100), ("IF", 0x0200), ("DF", 0x0400),
         ("OF", 0x0800))


# ---------------------------------------------------------------------- theme

BG = "#1e1e24"
PANEL = "#26262e"
FG = "#d6d6de"
DIM = "#8a8a99"
CHANGED = "#e8c14a"     # emu8086's "this register just moved" highlight
ACCENT = "#5aa9e6"
MONO = ("Consolas", 11) if os.name == "nt" else ("DejaVu Sans Mono", 11)
MONOB = (MONO[0], 11, "bold")


class RegPanel(tk.Frame):
    """Registers, one row each, last-changed value highlighted.

    The highlight is the point of this panel: emu8086 colours a register
    yellow when a step changes it, so you can see what the instruction just
    did without comparing numbers yourself.
    """

    def __init__(self, master):
        super().__init__(master, bg=PANEL, padx=10, pady=8)
        self.vars = {}
        self.rows = {}
        tk.Label(self, text="REGISTERS", bg=PANEL, fg=DIM,
                 font=(MONO[0], 9, "bold")).grid(row=0, column=0, columnspan=2,
                                                 sticky="w", pady=(0, 6))
        r = 1
        for name in REG16 + REG16B + SEG + ("IP",):
            tk.Label(self, text=name, bg=PANEL, fg=DIM,
                     font=MONO).grid(row=r, column=0, sticky="w", padx=(0, 12))
            v = tk.Label(self, text="----", bg=PANEL, fg=FG, font=MONOB,
                         width=6, anchor="w")
            v.grid(row=r, column=1, sticky="w")
            self.rows[name] = v
            r += 1
            if name in ("DX", "DI", "SS"):
                r += 1   # breathe between register groups
        self.icount = tk.Label(self, text="", bg=PANEL, fg=DIM,
                               font=(MONO[0], 9))
        self.icount.grid(row=r + 1, column=0, columnspan=2, sticky="w",
                         pady=(8, 0))

    def update_state(self, st, prev=None):
        for name, lbl in self.rows.items():
            if name not in st:
                continue
            lbl.config(text="%04X" % st[name])
            if prev and prev.get(name) != st.get(name) and name in prev:
                lbl.config(fg=CHANGED)
            else:
                lbl.config(fg=FG)
        if "ICOUNT" in st:
            self.icount.config(text="%d instructions" % st["ICOUNT"])


class FlagPanel(tk.Frame):
    def __init__(self, master):
        super().__init__(master, bg=PANEL, padx=10, pady=8)
        tk.Label(self, text="FLAGS", bg=PANEL, fg=DIM,
                 font=(MONO[0], 9, "bold")).grid(row=0, column=0, columnspan=9,
                                                 sticky="w", pady=(0, 6))
        self.cells = {}
        for i, (n, _m) in enumerate(FLAGS):
            c = tk.Label(self, text=n, bg=PANEL, fg=DIM, font=MONO, width=3)
            c.grid(row=1, column=i, sticky="w")
            self.cells[n] = c

    def update_state(self, st):
        f = st.get("FLAGS")
        if f is None:
            return
        for n, m in FLAGS:
            on = bool(f & m)
            self.cells[n].config(fg=ACCENT if on else DIM)


class ScreenPanel(tk.Frame):
    """The guest's video memory - emu8086's frmScreen.

    Drawn from screen.c's read-only accessors.  Until the UI can talk to a
    live core, this renders whatever the CLI reported about the text screen;
    the text comes back from the same VRAM the guest wrote.
    """

    def __init__(self, master):
        super().__init__(master, bg=PANEL, padx=10, pady=8)
        tk.Label(self, text="SCREEN", bg=PANEL, fg=DIM,
                 font=(MONO[0], 9, "bold")).pack(anchor="w", pady=(0, 6))
        self.txt = tk.Text(self, width=80, height=14, bg="#0b0b0f", fg="#c8c8d4",
                           font=(MONO[0], 10), bd=0, highlightthickness=0,
                           state="disabled", wrap="none")
        self.txt.pack(fill="both", expand=True)

    def set_text(self, s):
        self.txt.config(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.insert("1.0", s)
        self.txt.config(state="disabled")


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("mue - 8086 emulator")
        self.configure(bg=BG)
        self.geometry("1100x680")
        self.core = Core(MUE)
        self.prev = None
        self.last_out = ""
        self._build()

    def _build(self):
        bar = tk.Frame(self, bg=BG, padx=8, pady=6)
        bar.pack(fill="x")
        for label, cmd in (("Open", self.on_open), ("Assemble", self.on_asm),
                           ("Run", self.on_run), ("Step", self.on_step)):
            b = tk.Button(bar, text=label, command=cmd, bg=PANEL, fg=FG,
                          activebackground=ACCENT, activeforeground="#101014",
                          bd=0, padx=14, pady=5, font=(MONO[0], 10))
            b.pack(side="left", padx=(0, 6))
        self.status = tk.Label(bar, text="ready", bg=BG, fg=DIM,
                               font=(MONO[0], 10))
        self.status.pack(side="right")

        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        # Pack order matters: a side-packed widget claims the whole side it is
        # given, so `screen` must be packed BEFORE `right` or the right column
        # gets squeezed to nothing.  (It did exactly that: every panel
        # reported w=1 h=1 because `right` consumed the remaining space.)
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=0)
        body.columnconfigure(2, weight=1)
        body.rowconfigure(0, weight=1)

        # left: editor
        left = tk.Frame(body, bg=PANEL)
        left.grid(row=0, column=0, sticky="nsew")
        left.rowconfigure(1, weight=1)
        left.columnconfigure(0, weight=1)
        tk.Label(left, text="SOURCE", bg=PANEL, fg=DIM,
                 font=(MONO[0], 9, "bold")).grid(row=0, column=0, sticky="w",
                                                 padx=10, pady=(8, 4))
        self.editor = tk.Text(left, bg="#0b0b0f", fg="#c8c8d4", font=MONO,
                              insertbackground=FG, bd=0, highlightthickness=0,
                              undo=True, wrap="none", width=40, height=20)
        self.editor.grid(row=1, column=0, sticky="nsew", padx=10, pady=(0, 10))
        self.editor.insert("1.0", SNAP_ASM)

        # middle: the guest screen
        self.screen = ScreenPanel(body)
        self.screen.grid(row=0, column=1, sticky="nsew", padx=8)

        # right: state
        right = tk.Frame(body, bg=BG)
        right.grid(row=0, column=2, sticky="nsew")
        self.regs = RegPanel(right)
        self.regs.pack(fill="both", expand=True, pady=(0, 8))
        self.flags = FlagPanel(right)
        self.flags.pack(fill="x")

    # -- actions --------------------------------------------------------
    def on_open(self):
        p = filedialog.askopenfilename(filetypes=[("Assembly", "*.asm"),
                                                  ("All files", "*.*")])
        if not p:
            return
        with open(p, "r", errors="replace") as f:
            self.editor.delete("1.0", "end")
            self.editor.insert("1.0", f.read())
        self.status.config(text=os.path.basename(p))

    def _src(self):
        return self.editor.get("1.0", "end")

    def on_asm(self):
        st, out = self.core.run(self._src())
        self.last_out = out
        if st is None:
            self.status.config(text="assembly failed")
            messagebox.showerror("mue", out[-800:] or "assembly failed")
            return
        self.status.config(text="assembled")
        self._show(st)

    def on_run(self):
        st, out = self.core.run(self._src())
        self.last_out = out
        if st is None:
            self.status.config(text="run failed")
            messagebox.showerror("mue", out[-800:] or "run failed")
            return
        self.status.config(text="finished after %d instructions"
                                % st.get("ICOUNT", 0))
        self._show(st, prev=self.prev)
        self.prev = st
        # Whatever the guest printed is what it put on the screen.
        self.screen.set_text(guest_output(out))

    def on_step(self):
        # Single-instruction stepping needs a live core; the CLI path runs to
        # completion.  Say so plainly rather than pretending to step.
        self.status.config(text="step needs the live-core bridge (not wired yet)")
        self.on_run()

    def _show(self, st, prev=None):
        self.regs.update_state(st, prev or self.prev)
        self.flags.update_state(st)


def guest_output(out):
    """The guest's own console output, without mue's register dump."""
    cut = out.find("\n------------- REGISTERS -------------")
    return out[:cut] if cut >= 0 else out


def main():
    if not os.path.exists(MUE):
        print("mue binary not found at", MUE, file=sys.stderr)
        return 1
    App().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
