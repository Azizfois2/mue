# mue — clean-room 8086 assembler + emulator

A behaviour-compatible reimplementation of `emu8086.exe` (the 8086
assembler and emulator by Yuri Starodubtcev), written from scratch in C by
observing the original's external behaviour. See `re/FINDINGS.md` and
`re/CPU_SPEC.md` for the analysis, with every claim labelled CONFIRMED,
INFERRED or DESIGN.

This is an independent project. It is not affiliated with, endorsed by, or
supported by the authors of emu8086. The name is used only to say what the
program is compatible with.

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

Checks 2 and 3 read fixtures from `C:\emu8086`, so they need emu8086 installed;
`make` alone never touches it.

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

## Use

    mue <file.asm>            assemble and run (.COM)
    mue -a <file.asm> <out>   assemble only (MZ EXE if the source is
                              `.model` without ORG, else flat binary)
    mue -r <file.bin>         run a raw binary

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

## Licence

MIT — see `LICENSE`. You may use, modify and redistribute this code freely,
including commercially, provided the copyright notice is retained.

`emu8086` itself is a separate, proprietary product with its own licence; none
of its code is included here. This project only reproduces its observable
behaviour and output format.
