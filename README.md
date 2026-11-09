# mue — clean-room 8086 assembler + emulator

A behaviour-compatible reimplementation of `emu8086.exe`, derived by static
reverse engineering (see `re/FINDINGS.md`, `re/CPU_SPEC.md`).

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

`C:\emu8086\Examples\*.asm`: 79 of 85 assemble (the rest use source-level
features such as `include` that are not yet implemented).
