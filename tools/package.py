#!/usr/bin/env python3
"""Assemble a self-contained, emu8086-shaped build folder.

Usage:  python tools/package.py [destination] [source-tree]

The destination (default ``build``) gets ``muei.exe`` plus the same folder
names emu8086 installs:

    DEVICES/  documentation/  examples/  fasm/  inc/  MyBuild/  MySource/
    Options/  vdrive/  ReadMe.txt  LICENSE.txt  mue.ini

`mue_paths.c` recognises this layout: when the executable's own directory has
an ``inc\\`` folder it becomes the install root, so the packaged build never
reads or writes the developer's C:\\mue tree.

The source tree is taken from ``MUE_PACKAGE_SRC``, or the first of C:\\mue and
C:\\emu8086 that exists, or the second command-line argument.  On a machine
with emu8086 installed this copies its real templates, examples and devices;
otherwise it copies mue's own installed tree when one has been seeded.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def source_tree(explicit):
    if explicit:
        return explicit
    env = os.environ.get("MUE_PACKAGE_SRC")
    if env:
        return env
    for cand in (r"C:\mue", r"C:\emu8086", os.path.join(ROOT, "install")):
        if os.path.isdir(os.path.join(cand, "inc")):
            return cand
    return None


def copy_tree(src, dst):
    """Copy everything except build/user artefacts; keep the folder skeleton."""
    skip_dirs = {".git", "__pycache__", ".uiobj", ".obj"}
    for name in sorted(os.listdir(src)):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.isdir(s):
            if name in skip_dirs:
                continue
            os.makedirs(d, exist_ok=True)
            copy_tree(s, d)
        else:
            shutil.copy2(s, d)


def empty_dir(path):
    os.makedirs(path, exist_ok=True)
    for name in os.listdir(path):
        p = os.path.join(path, name)
        try:
            if os.path.isdir(p) and not os.path.islink(p):
                shutil.rmtree(p)
            else:
                os.unlink(p)
        except OSError:
            pass


def main():
    dst = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                          os.path.join(ROOT, "build"))
    src = source_tree(sys.argv[2] if len(sys.argv) > 2 else None)
    ui = os.path.join(ROOT, "src", "mueui.exe")
    cli = os.path.join(ROOT, "src", "mue.exe")

    if not os.path.isfile(ui):
        print("error: run `make ui` first (src/mueui.exe is missing)",
              file=sys.stderr)
        return 1
    if not src:
        print("error: no source tree found; set MUE_PACKAGE_SRC",
              file=sys.stderr)
        return 1

    os.makedirs(dst, exist_ok=True)
    copy_tree(src, dst)

    # emu8086 ships these two folders empty; a packaged build should too.
    empty_dir(os.path.join(dst, "MyBuild"))
    empty_dir(os.path.join(dst, "MySource"))

    # Point the marker config at the packaged root, not the developer's tree.
    ini = os.path.join(dst, "mue.ini")
    if os.path.isfile(ini):
        with open(ini, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        text = text.replace("HOME=C:\\mue", "HOME=" + dst)
        text = text.replace("VDRIVE_PATH=C:\\mue\\vdrive",
                            "VDRIVE_PATH=" + os.path.join(dst, "vdrive"))
        with open(ini, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    shutil.copy2(ui, os.path.join(dst, "muei.exe"))
    if os.path.isfile(cli):
        shutil.copy2(cli, os.path.join(dst, "mue.exe"))

    folders = [n for n in sorted(os.listdir(dst))
               if os.path.isdir(os.path.join(dst, n))]
    files = [n for n in sorted(os.listdir(dst))
             if os.path.isfile(os.path.join(dst, n))]
    print("packaged %s" % dst)
    print("  exe:     muei.exe%s" %
          (" + mue.exe" if os.path.isfile(cli) else ""))
    print("  folders: %s" % ", ".join(folders))
    print("  files:   %s" % ", ".join(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
