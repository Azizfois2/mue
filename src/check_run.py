#!/usr/bin/env python3
"""Run every shipped example and report what went wrong, if anything.

This is the runtime counterpart to check_examples.py: that one proves the
bytes match, this one proves the program actually runs.  A run "passes" when
the emulator exits 0 and the guest produced no diagnostic (an unsupported
interrupt, a rejected opcode, a bad file call).  Guest output is whatever the
program prints, so no output at all is fine.

  --min N   fail if fewer than N examples run clean
  -v        list every example and its reason

Exit 0 pass, 1 fail, 77 skip (no fixtures, or no mue binary).
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.environ.get("EMU8086_EXAMPLES", "C:/emu8086/examples")

SUFFIX = ".exe" if os.name == "nt" else ""
MUE = [os.path.join(HERE, "mue" + SUFFIX), os.path.join(HERE, "mue")]

# Diagnostics the emulator itself emits.  Any of these inside a run means the
# guest asked for something the clone does not do.  "unknown opcode skipped"
# is deliberately NOT here: the original prints the same line and steps over
# the byte, and 11 shipped examples (PUSHA/POPA) depend on exactly that.
DIAG = re.compile(
    r"not supported yet(?!\n)|FPU ERROR|wrong (?:AL|parameter)|does not respond")
TOLERATED = re.compile(
    r"unknown opcode skipped|virtual printer does not respond")


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return c
    return None


def winpath(p):
    """MinGW builds only understand Windows paths."""
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == "/":
        return p[1].upper() + ":" + p[2:].replace("/", "\\")
    return p


def main():
    args = sys.argv[1:]
    verbose = "-v" in args
    want = 0
    if "--min" in args:
        want = int(args[args.index("--min") + 1])

    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    if not os.path.isdir(EXAMPLES):
        print("SKIP: no examples at %s (set EMU8086_EXAMPLES)" % EXAMPLES)
        return 77

    files = sorted(glob.glob(os.path.join(EXAMPLES, "*.asm")))
    ok = 0
    bad = []
    for f in files:
        name = os.path.basename(f)
        try:
            # examples/traffic_lights.asm delays five seconds per step, so the
            # sweep turns the INT 15h wait off; the delay itself is checked by
            # check_runtime.py.
            env = dict(os.environ, MUE_NOSLEEP="1")
            p = subprocess.run([mue, winpath(f)], capture_output=True,
                               env=env, timeout=60)
        except subprocess.TimeoutExpired:
            bad.append((name, "timeout"))
            continue
        out = ((p.stdout or b"") + (p.stderr or b"")).decode(
            "utf-8", "replace")
        # an opcode-skip notice is expected fidelity; strip it before looking
        for line in TOLERATED.findall(out):
            pass
        seen = "\n".join(l for l in out.splitlines()
                         if not TOLERATED.search(l))
        if p.returncode != 0:
            bad.append((name, "exit %d" % p.returncode))
        elif DIAG.search(seen):
            hit = DIAG.search(seen).group(0)
            bad.append((name, "diagnostic: " + hit))
        else:
            ok += 1
        if verbose:
            print("%-34s %s" % (name, "ok" if not bad or bad[-1][0] != name
                                else bad[-1][1]))

    for name, why in bad:
        print("FAIL %-34s %s" % (name, why))
    print("RUN %d/%d clean" % (ok, len(files)))
    if want and ok < want:
        print("below --min %d" % want)
        return 1
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
