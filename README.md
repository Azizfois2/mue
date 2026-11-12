# mue — clean-room 8086 assembler + emulator

A behaviour-compatible reimplementation of `emu8086.exe` (the 8086
assembler and emulator by Yuri Starodubtcev), written from scratch in C by
observing the original's external behaviour. See `re/FINDINGS.md` and
`re/CPU_SPEC.md` for the analysis, with every claim labelled CONFIRMED,
INFERRED or DESIGN.

This is an independent project. It is not affiliated with, endorsed by, or
supported by the authors of emu8086. The name is used only to say what the
program is compatible with.

## Quick start

```console
$ make
$ ./src/mue.exe /c/emu8086/examples/Hello.asm
assembled 39 bytes
Hello, world!
------------- REGISTERS -------------
AX=0000  BX=0000  CX=0000  DX=0000
SP=0000  BP=0000  SI=000D  DI=0000
CS=0100  DS=0100  ES=0100  SS=0100  IP=0000
FLAGS=0206 PF IF 
instructions executed: 99
```

Assemble only, without running:

```console
$ ./src/mue.exe -a prog.asm prog.com     # flat .COM / .BIN
$ ./src/mue.exe -a prog.asm prog.exe     # MZ .EXE if the source has
                                         # .model and no ORG
$ ./src/mue.exe -r prog.bin              # run a raw binary
```

### It produces the same bytes as emu8086

The strongest evidence is that this assembler reproduces emu8086's own shipped
output exactly. Given a source emu8086 itself built:

```console
$ ./src/mue.exe -a hi-world.com_.~asm out.com
assembled 97 bytes -> out.com
$ cmp out.com hi-world.com_ && echo identical
identical
```

That holds for all 44 paired sources in emu8086's `MyBuild` directory,
including the MZ EXE header, and for all 46 `.list` listings byte for byte.
See [Verification](#verification).

## Build

    make                # -> src/mue.exe

(MinGW ships GNU make as `mingw32-make`; that name works too.) Without make:

    cd src
    gcc -O2 -Wall -Wno-unused-parameter -I. -o mue.exe \
        main.c exec.c mem.c alu.c io.c dos.c asm_main.c asm_sup.c asm_ops.c exe.c

## Test

One command runs every check and exits non-zero if any of them regresses:

    make test

It performs three independent checks, in order:

1. **CPU self-test** (`src/test_cpu.c`) — flags, stack, call/ret, loop, mul/div.
2. **Assembler vs emu8086 `.list` listings** (`src/check_asm.py`) — every emitted
   byte compared at the offset the listing reports.
3. **Byte round-trip** (`src/check_roundtrip.py`) — reassemble emu8086's own
   shipped sources and require the output to be byte-identical to the binaries
   emu8086 itself produced, MZ EXE header included.
4. **Runtime regressions** (`src/check_runtime.py`) — the behaviours the
   examples depend on: PUSHA/POPA stepped over, AAA/AAS/DAA/DAS adjusting BCD,
   INT 21h AH=05h refusing, INT 19h halting, the IVT answering a
   `call far es:[40h]`, and an unimplemented INT 21h function reporting.
5. **Every shipped example runs** (`src/check_run.py`) — all 85 `.asm` files
   are assembled and executed with no diagnostic and a zero exit, which is
   what "the clone can run emu8086's own programs" actually means.

Checks 2, 3 and 5 read fixtures from `C:\emu8086`, so they need emu8086
installed; `make` alone never touches it. Where those fixtures are missing the
checks print `SKIP` and exit 77, which `make test` treats as skipped rather than
passed -- so a machine without emu8086 gets a green run that never silently
claims to have verified byte compatibility.

Fixture locations can be overridden with `EMU8086_MYBUILD` and
`EMU8086_EXAMPLES`.

### Continuous integration

`.github/workflows/build.yml` runs `make test` on Ubuntu and Windows for every
push and pull request. CI has no emu8086, so it covers the build, the CPU
self-test and the examples breadth check; the `.list` and round-trip checks
report `SKIP`. Full byte-compatibility verification is local-only.

### Examples breadth

    make examples

Assembles all 85 programs in `C:\emu8086\examples` and prints the count plus the
reason each remaining one fails. This is a breadth check, not a correctness
oracle -- it catches a whole feature going missing (an operand form, a directive,
a macro facility). It fails if the count drops below 85, so losing a program is loud.

All 85 now assemble. The last three gaps closed were:

| gap | fix |
| --- | --- |
| far `jmp seg:off` rejected | emit `EA`/`9A` with offset then segment |
| bare `label+n` without brackets | operand parser matched only the whole text against a label name, not its leading identifier |
| `JCXZ` beyond 127 bytes rejected | rewrite as `OR CX,CX` / `JNZ +3` / `JMP target` (emu8086's documented expansion) |

## Verification

`src/check_asm.py` assembles every `C:\emu8086\MyBuild\*.~asm` source and
compares the bytes against emu8086's own `.list` listings:

    PASS 46  FAIL 0  SKIP 0        # every emu8086 listing reproduced exactly

**The strongest check** is a byte round-trip against emu8086's own shipped
output: reassemble each `MyBuild\*.~asm` source and compare with the binary
emu8086 itself produced next to it (`*.bin_`, `*.com_`, `*.exe_`):

    44 / 44 byte-identical          # includes the MZ EXE header

CPU self-test (`test_cpu.exe`): ALL PASS (0 failures).

`make examples`: all 85 programs in `C:\emu8086\Examples` assemble.

### Where the clone is deliberately better

Everywhere a comparison is possible - every example's output, every `.list`
byte, every decoded interrupt function - `mue` matches emu8086 exactly. The
places it knowingly does not all follow one rule:

> **If emu8086 dispatches a function, match emu8086 exactly, bugs and all.**
> **If emu8086 has no arm for it, implement standard DOS.** Never copy a hole.

An absent arm is missing code, not a specification. Reproducing it means
shipping a call that reports an error and carries on, which is not what any
DOS program means by that call.

- **`INT 21h` 48h/49h/4Ah (allocate/free/resize memory).** emu8086 dispatches
  these and returns **without touching a single register**, so a program
  cannot tell success from failure. `mue` implements a real memory arena.
  The original's silence is still available with `MUE_INT21_MEM_NOOP=1`.
- **`INT 21h` AH=4Bh EXEC** really loads and runs a child. emu8086 has no 4Bh
  arm at all. On by default; `MUE_EXEC=0` restores the non-behaviour.
- **`INT 21h` AH=31h TSR** terminates with AL as the exit code. Also absent
  from emu8086.

This is intended behaviour - not an oversight, and not something to "fix"
back. See `re/GAPS.md` section 4 for the divergence list and
`re/FINDINGS.md` section 50b for the reasoning behind the rule.

## Licence

MIT — see `LICENSE`. You may use, modify and redistribute this code freely,
including commercially, provided the copyright notice is retained.

`emu8086` itself is a separate, proprietary product with its own licence; none
of its code is included here. This project only reproduces its observable
behaviour and output format.
