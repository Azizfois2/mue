# emu8086 4.08 install tree — reverse-engineered

Status: **confirmed** by listing `C:\emu8086` and reading the files. This is
the on-disk shape the clone should mirror, because the UI is built around it:
the examples menu reads `examples\`, the build writes `MyBuild\`, the virtual
drive is `vdrive\<letter>\`, the code templates are `inc\*_template.txt`, and
the editor's colours and fonts are read from `Options\*.dat`.

```
C:\emu8086\
├── DEVICES\             12 files, 389 KB   virtual devices (external .exe)
├── documentation\       36 files, 335 KB   offline help (HTML)
│   └── img\             41 files, 245 KB
├── examples\            86 files, 196 KB   the "examples" menu contents
├── fasm\                 3 files,  77 KB   Flat Assembler 1.64
│   └── INCLUDE\          6 files,  18 KB
├── inc\                 10 files,  25 KB   templates + emu8086.inc
├── MyBuild\            237 files, 340 KB   build outputs (the default target)
├── MySource\             2 files,   1.4 KB user sources
├── Options\              5 files,   2 KB   editor settings, keywords, fonts
├── vdrive\               A B C D E F tmp  virtual floppy/drive letters
├── BIOS_ROM             49,147 bytes
├── default.binf          1,522 bytes
├── diasm.dll            98,304 bytes       disassembler
├── emu8086.exe       2,244,608 bytes       VB6 main program
├── emu8086.ini           5,786 bytes       configuration (plain INI)
├── FLOPPY_0          1,474,560 bytes        1.44 MB floppy image
├── INT_VECT              1,045 bytes
├── LICENSE.txt           3,225 bytes
├── MicroAsm.dll          9,216 bytes       assembler
├── ReadMe.txt            1,938 bytes
├── reg.ini                  58 bytes       registration name/key
├── unins000.dat/.exe                      Inno Setup uninstaller
└── version.txt          16,205 bytes       full changelog
```

## 1. Top-level files

| file | role |
|---|---|
| `emu8086.exe` | the VB6 application: editor, assembler front end, emulator, every window in `UI_SPEC.md` |
| `MicroAsm.dll` | the 8086 assembler proper ("emu8086 assembler version: 4.08") |
| `diasm.dll` | the disassembler used by the disassembly pane and the listing |
| `fasm\FASM.EXE` | Flat Assembler 1.64, used when the source carries `#fasm#` |
| `BIOS_ROM` | 49,147-byte ROM image mapped into the guest |
| `INT_VECT` | initial interrupt vector table content |
| `FLOPPY_0` | 1,474,560-byte (1.44 MB) floppy image for the virtual drive |
| `default.binf` | starts with the ASCII `0100...gm... 0`, a default binary info record |
| `emu8086.ini` | all user settings, plain text with comment lines |
| `reg.ini` | `username=`, `regkey=`, `q=` — the registration that makes `about` say "this product is registered to: <name>" |

### emu8086.ini keys worth mirroring

`STARTUPWIN=true` is what makes the `choose code template` window appear at
startup; set it false and the editor opens directly. Others: `LISTING=true`
(write `*.list`), `WARN_RUN`, `ASK_OPEN_URL`, `VDRIVE_PATH=default`,
`EMUPORT=c:\emu8086.io`, `HW_INTERRUPT_FILE=c:\emu8086.hw`,
`HARDWARE_INTERRUPTS`, `ACTIVATE_SCREEN`, `ACTIVATE_SCREEN_WHEN_STOPPED`, plus
a `[fonts]` section.

The comment above `MASM_PATH` explains the `#masm#` directive: the source can
be handed to MASM 6.11 instead of the built-in assembler.

## 2. Build outputs — the `_` suffix

Every build writes **five** files into `MyBuild\`, and the binary's extension
carries a trailing underscore:

```
hello.com_         39 bytes    the binary the emulator loads
hello.com_.debug  1296         debug info for the listing/step-back
hello.com_.list   4731         listing (machine code <- source)
hello.com_.symbol  776         symbol table
hello.com_.~asm   1181         backup copy of the source at build time
```

The same scheme is used for `.exe_` and `.bin_`. Files without the underscore
(`hello.com`, `hello.com.debug`, …) also exist, which is what the Save As
dialog produces when you export a build explicitly.

### `.list` format

```
EMU8086 GENERATED LISTING. MACHINE CODE <- SOURCE.

hello.com_ -- emu8086 assembler version: 4.08

[ 11/13/2026  --  11:23:45 AM ]

===================================================================================================
[LINE]     LOC: MACHINE CODE                          SOURCE
===================================================================================================

[   1]        :                                       ; "hello, world!" step-by-step char-by-char way...
[  11]    0100: EB 0E                                 jmp     start
[  14]    0102: 48 65 6C 6C 6F 2C 20 77 6F 72 6C 64   msg db 'Hello, world!', 0
                21 00
```

Line number, `SSSS:` + up to 12 bytes of machine code (wrapped with a hanging
indent), then the source line verbatim.

### `.symbol` format

```
< THE SYMBOL TABLE >  hello.com_  --  emu8086 assembler version: 4.08
===================================================================================================
Name                     	Offset    	Size      	Type      	Segment
===================================================================================================
MSG                      	00102     	1         	VAR       	(NOSEG)
START                    	00110     	-1        	LABEL     	(NOSEG)
```

Tab-separated, offsets shown in hex without a suffix, `-1` size for labels.

## 3. `Options\` — editor settings

| file | size | content |
|---|---|---|
| `Fonts.dat` | 464 | fixed-width records holding the `Terminal` face name padded with spaces, one per editor/view |
| `Keywords.dat` | 132 | eleven 12-byte triples `(foreground, background, 0)` in `COLORREF` order (low byte blue) |
| `hotkeys.dat` | 1337 | the key bindings shown right-aligned in the menus |
| `LoadFlag.dat` | 34 | literal text `Load options from hdd flag file.` |
| `Other.dat` | 45 | binary flags (auto-indent mode, tab size 4, line-number style/start) |

`Keywords.dat` decoded (fragment of the important slots):

| # | foreground | background |
|---|---|---|
| 0 | `#008000` green | `#FFFFFF` |
| 1 | `#FF0000` red | `#FFFFFF` |
| 2 | `#A0A0A0` grey | `#E6E6E6` |
| 3 | `#000000` black | `#FFFFFF` |
| 4 | `#C86400` orange | `#FFFFFF` |
| 6 | `#800080` purple | `#FFFFFF` |
| 7 | `#0000C8` blue | `#FFFFFF` |
| 8 | `#000080` navy | `#FFFFFF` |
| 9 | `#640000` dark red | `#FFFFFF` |

That is why the editor's syntax colouring is not hard-coded: it is a user
setting, and `options` edits exactly these slots.

## 4. `inc\` — templates and includes

```
0_com_template.txt      org 100h / ; add your code here / ret
1_exe_template.txt      data+stack+code segments, pkey db "press any key...$"
2_bin_template.txt
3_boot_template.txt
fasm_0_com_template.txt .. fasm_3_boot_template.txt
emu8086.inc             the macro library (PRINT, PRINTN, ...)
__README_IMPORTANT.txt  warns that this folder is overwritten by updates
```

The four templates are exactly the four radios of the `choose code template`
startup dialog. `0_com_template.txt` in full:

```asm
; You may customize this and other start-up templates;
; The location of this template is c:\emu8086\inc\0_com_template.txt

org 100h

; add your code here

ret
```

`1_exe_template.txt` builds the classic three-segment EXE skeleton with
`pkey db "press any key...$"`, `mov ax, data / mov ds, ax / mov es, ax`,
`int 21h` with `ah=9`, a key wait with `ah=1`, and `mov ax, 4c00h / int 21h`.

## 5. `examples\` — 86 files

The `examples` toolbar dropdown is a curated list, not the folder listing:

```
Hello, world        -> hello.asm
add / subtract      -> 1_sample.asm or 2_sample.asm
calculate sum
compare numbers
binary, hex and octal values
traffic lights      -> traffic_lights.asm
palindrome
LED display test
stepper motor
simple i/o
more examples...    -> opens the folder
```

The folder also holds the virtual-device sources (`robot.asm`,
`thermometer.asm`, `PrinterDemo.asm`), the micro-OS (`micro-os_kernel.asm`,
`micro-os_loader.asm`), `snake.asm`, `matrix.asm`, `mouse.asm`,
`self_modifying_code.asm`, `far_call*.asm`, the BCD/string conversion
examples, and `_ReadMe.txt`.

## 6. `DEVICES\` — external virtual devices

```
LED_Display.exe   53,248
Printer.exe       32,768   (+ Printer.txt)
Robot.exe         69,632   (+ robot_map.dat, 54 bytes)
Simple.exe        36,864
simplest.com         781   (a device written as a COM program)
Stepper_motor.exe 45,056
Thermometer.exe   65,536
Traffic_Lights.exe 53,248
VGA_STATE.exe     28,672
_READ_ME.txt
```

The `virtual devices` menu is generated by listing this folder, which is why
the captured menu contains exactly these names (minus the `_READ_ME.txt`).

## 7. `vdrive\` — virtual drive

Subfolders `A` … `F` and `tmp`, all empty by default. The emulator maps DOS
drive letters onto them, and `virtual drive > create new floppy drive` makes
the next letter. `FLOPPY_0` is the image used by `boot from virtual floppy`.

## 8. What the clone must reproduce structurally

1. `MyBuild\` as the default output folder, with the `<name>.<ext>_` naming
   and the four sidecars (`.debug`, `.list`, `.symbol`, `.~asm`).
2. `inc\` templates driving `file > new` and the startup dialog.
3. `examples\` as the source of the curated dropdown and "more examples".
4. `Options\Keywords.dat` semantics (per-category fg/bg) even if mue stores
   its own file, so `options` edits something real.
5. `vdrive\<letter>\` so `virtual drive` is meaningful.

## 9. The clone's own tree — implemented

`src/mue_paths.c` is the only module that knows where the tree lives;
everything else asks it (`mue_home()`, `mue_dir_build()`, …).

### Root resolution

| order | root | when |
|---|---|---|
| 1 | `$MUE_HOME` | always wins, and is created when missing |
| 2 | `<exe>\..` | a shipped layout: `inc\` sits beside the binary's folder |
| 3 | `C:\mue` | the default; created on first run |
| 4 | `C:\emu8086` | the reference install, used when `C:\mue` cannot be created |

The UI exports the resolved root as `MUE_HOME` at start-up, so core code that
touches the tree (INT 13h reads `FLOPPY_<n>`) uses the same place.

### First-run seeding

`mue_tree_ensure()` runs before the first paint.  It creates the whole
directory set — `examples`, `inc`, `Options`, `MyBuild`, `MySource`,
`documentation\img`, `DEVICES`, `fasm\INCLUDE`, `vdrive\A..F` and
`vdrive\tmp` — and then imports `inc\`, `examples\`, `Options\` and
`documentation\` **verbatim** from the reference install when one is found
(`$MUE_REF`, else `C:\emu8086`).  Files that already exist are never
overwritten, so edited templates survive.

`mue.ini` is the marker: when it exists the seeding is skipped, which keeps
start-up at a few milliseconds after the first run.

Where no reference install exists, the same files are written from literals in
`mue_paths.c`: the four templates (plus their `#fasm#` twins), a
`Keywords.dat` holding the decoded colours from section 3, `Other.dat` with
the documented flags, a help page, `ReadMe.txt` and `LICENSE.txt`.

### Build artefacts (implemented)

`emulate` (and the reset that re-runs the last source) writes the five files
of section 2 into `MyBuild\` with the underscore suffix:

```
hello.com_          39 bytes   the image the emulator loads
hello.com_.debug               one `line <n> <addr> <len>` record per statement
hello.com_.list                header, then [LINE] LOC: MACHINE CODE  SOURCE
hello.com_.symbol              upper-cased name, 10-char columns
hello.com_.~asm                the source as it stood at build time
```

`compile` is different, because emu8086 asks where the binary goes: it
assembles first (showing the status window), then a Save-As box defaulting to
`MyBuild\<stem>.com` decides the destination.  Whatever path is chosen gets
the image and its four sidecars **without** the underscore — exactly the
`hello.com`, `hello.com.list`, … set that an explicit export makes.  Cancel
writes nothing at all and the status window appends
`executable file is not created!`.

The listing and symbol table come straight from the assembler: the converged
pass now leaves its merged source, its per-line address/length records and its
label table behind (`asm_last_lines()`, `asm_last_source()`,
`asm_last_labels()`), so the reports cannot disagree with the binary.  The
`.list` column layout was checked against `C:\emu8086\MyBuild\hello.com_.list`
and matches to the character; the only intentional differences are the
program name in the header (`mue assembler`) and `.debug`, which is mue's own
line-oriented text format instead of emu8086's private binary one.

### Packaged build (`make package`, implemented)

`tools/package.py` produces a portable folder that mirrors emu8086's install
layout rather than writing into `C:\mue`:

```
build/
  muei.exe  mue.exe          UI + CLI
  DEVICES/ documentation/ examples/ fasm/ inc/ Options/ vdrive/
  MyBuild/ MySource/         emptied on every package run
  ReadMe.txt LICENSE.txt mue.ini
```

`mue_paths.c` resolves the install root in this order: `$MUE_HOME`, then the
executable's own directory when it contains `inc\`, then the parent directory
when it contains `inc\`, then `C:\mue`, then `C:\emu8086`.  A packaged
`build\muei.exe` therefore reads its own `examples\` and `DEVICES\` and writes
its builds into its own `MyBuild\`.  Verified live: the editor opened
`build\examples\hello.asm` and the trace logged
`home: C:\Users\acer\Documents\mue\build`.

