# emu8086 UI specification — measured from the running application

Status: **in progress**. Everything below is CONFIRMED from a live capture of
`C:\emu8086\emu8086.exe` (version 4.08) on this machine unless a line says
otherwise. Measurements are logical pixels (the app is DPI-unaware; at 125%
display scaling Windows stretches it, so captures are taken with
`re/uicap.py --virtual` to get the app's own pixel grid).

Tools used, both in `re/`:

| tool | what it does |
|---|---|
| `uicap.py` | launch/find the app, click a client-relative point, `PrintWindow` every visible top-level window to PNG + a JSON manifest of window/client rects |
| `uiprobe.py` | scripted evidence runs (menus, dialogs, panels) that label each capture |

Why capture instead of trusting the documentation GIFs: the GIFs in
`re/ui_ref/` are from an unknown build and an unknown Windows colour scheme.
The live app is the build being cloned. Where they disagree, the live capture
wins — and they do disagree (see "Colour scheme" below).

---

## 1. Colour scheme (measured)

The app is a VB6 program using **system colours**, so the exact values come
from the host's classic-colour scheme. On this machine:

| role | value | where seen |
|---|---|---|
| form face | `#F0F0F0` | window background, toolbar, group interiors |
| field face | `#FFFFFF` | register boxes, address boxes, memory/disassembly lists, source pane |
| text | `#000000` | labels, values, disassembly |
| address text | `#0000FF` | memory row addresses, disassembly operand addresses |
| hex byte text | `#008000` | the memory list's HEX column |
| decimal text | `#808080` | the memory list's DECIMAL column |
| selection / current | `#0000FF` background, `#FFFFFF` text | selected memory rows and the current disassembly line |
| changed register | `#0000FF` text | register value after a step changed it |
| source current line | `#FFFF00` background | the yellow row in `original source code` |
| disabled | `#808080` icon + `#808080` label | `step back` before anything has run |

**This corrects `re/UI_REFERENCE.md`.** It records "changed register drawn on
yellow `#FFFF96`" and a `#D4D0C8` form. Both are wrong for this build:

* the form is `#F0F0F0` here — `#D4D0C8` is the Windows-2000-era face and is
  what the documentation GIF's palette suggests;
* a register that just changed is drawn in **blue text**, not on a yellow
  field. The yellow band in the emulator belongs to the **source viewer**,
  which highlights the source line being executed.

Verified by single-stepping `Hello.asm`: after `CS:IP` moved, `IP 0113` is
blue while every unchanged register stays black on white.

## 2. Fonts (measured)

* Labels, menus, buttons, group titles: **MS Sans Serif 8 pt** (`#000000`).
* Register values, address fields, memory and disassembly text: a bitmap
  monospace, **bold**, roughly 8 pt, with a *dotted zero* — this is the
  classic `Terminal`/`Fixedsys` raster font, not Courier New. On the clone the
  closest portable match is Courier New bold at 12–13 px; the *boldness and
  the monospace grid* are what matter for the shape.
* The source editor uses **Courier New 10 pt**, not bold, with syntax colours
  (see §5).

## 3. The main editor window

Default size **738 x 782** (client 722 x 723), resizable, maximizable.

Bands, top to bottom (window coordinates):

| band | y | height |
|---|---|---|
| title bar | 0 | 30 |
| menu bar | 30 | 24 |
| toolbar | 54 | 52 |
| editor | 106 | ~640 |
| horizontal scrollbar + status bar | 746 | 36 |

### Menu bar

Lowercase captions, left to right:

`file  edit  bookmarks  assembler  emulator  math  ascii codes  help`

### Toolbar

Icon above label; buttons are flat with a 1px raised border, face `#F0F0F0`,
label MS Sans Serif 8 pt. Eleven buttons in four groups separated by 2px
vertical grooves:

| group | buttons | notes |
|---|---|---|
| file | `new`, `open`, `examples`, `save` | `examples` and `save` carry a small black down-arrow |
| build | `compile`, `emulate` | |
| tools | `calculator`, `convertor` | |
| help | `options`, `help`, `about` | |

Button cell ≈ 64 wide x 46 high; icons are 16–24 px and drawn in
`#0000FF`/`#008000`/`#FFFF00` on a white page.

Icons, as drawn in the original: `new` = white page; `open` = yellow folder
with a green up-arrow; `examples` = yellow star; `save` = black/grey floppy;
`compile` = blue-chip grid; `emulate` = green play triangle; `calculator` =
blue calculator; `convertor` = blue "01" keys; `options` = hammer + wrench;
`help` = yellow question mark; `about` = green chip.

### Editor pane

* Gutter: ~24 px of `#C0C0C0`-ish grey with a 14 px line-number strip.
* Line numbers: decimal, two digits, dark blue-grey, right aligned.
* Text canvas: white, carriage-return caret at column 1.
* Right vertical scrollbar, bottom horizontal scrollbar (classic sunken,
  arrow buttons at both ends).

### Status bar

Four sunken panels; the wide one (centre/right) reads `drag a file here to
open` when nothing is loaded. A size grip sits in the bottom-right corner.
Panel edges are visible as 2px grooves at x ≈ 40, 80, 160, 190 and 650.

## 4. The emulator window

Default size **581 x 443** (client 565 x 384), resizable. Title is
`emulator: <binary name>` once a program is loaded, `8086 microprocessor
emulator` before that. It is opened by `emulate`, not at startup.

### Menu bar

`file  math  debug  view  external  virtual devices  virtual drive  help`

(Item captions per menu are in `re/UI_CONTROLS.md`, which extracted them from
the binary's string table; the nesting there is INFERRED and the live menus
are captured in `re/cap/` to settle it.)

### Toolbar (y 0–53 of the client)

Five icon-over-label buttons, then the step-delay control at the right:

`Load` (capital L) · `reload` · `step back` · `single step` · `run`

* Cell ≈ 88 x 46, face `#F0F0F0`, 1px raised border.
* `Load` is the default: it wears a **2px black border** (focus/default), as
  the original does.
* `step back` is **disabled (grey icon, grey label) until at least one step
  has been executed**, then becomes a red/orange left triangle with a bar.
* `single step` is a green right triangle with a bar; `run` is a green double
  chevron; `reload` is a blue circular arrow.
* Right of the buttons: a horizontal slider with 6 ticks, and below it the
  label `step delay ms: 0`. The slider handle starts at the far left.

### Body (client y 53 onward)

Three columns inside one sunken panel area:

| column | x | width | contents |
|---|---|---|---|
| registers | 8 | 112 | group box, `registers` title on the border |
| memory | 150 | 190 | address field over the byte list + scrollbar |
| disassembly | 345 | 226 | address field over the instruction list + scrollbar |

**Registers group** (`registers`, title on the top border, 1px etched frame):

* `H` and `L` column headers, centred over the two byte columns.
* `AX BX CX DX` each as **two separate 20x20 sunken white fields** (`AH AL`,
  `BH BL`, …), one per byte — independently editable in the original.
* `CS IP SS SP BP SI DI DS ES` each as one **40x20 sunken white field**.
* Row pitch 27 px. Label column is right-aligned MS Sans Serif 8 pt.
* Values are bold monospace; a value that changed since the last step is
  **drawn in blue**, everything else black.
* Startup state before loading: `CS=0050`, all other registers `0000`.

**Memory panel**:

* Address field: 165x25 sunken white box, centred text, `0700:0113` style,
  editable (typing a new `SSSS:OOOO` re-anchors the view).
* List: white, 18 px rows, one byte per row, four columns:
  1. physical address, 5 hex digits + `:` (e.g. `07100:`), `#0000FF`;
  2. the byte, two hex digits, `#008000`;
  3. the byte in decimal, 3 digits, `#808080`;
  4. the character, or `NULL` when not printable, bold black.
* The **bytes of the instruction at CS:IP are selected** — blue background,
  white text — for exactly as many rows as the instruction is long.
* Vertical scrollbar, classic sunken, arrow buttons.

**Disassembly panel**:

* Address field, same style as memory, `0700:0113`.
* List: white, 18 px rows, bold monospace, one instruction per row, no address
  column — the original shows the mnemonic and operands only
  (`MOV SI, 0000h`, `CMP AL, 0h`, `JZ 0122h`).
* The **current instruction (CS:IP) is selected**: blue background, white
  text.
* The list starts at the pane's anchor, not at CS:IP, so the current
  instruction can appear mid-list; when the anchor is `0700:0000` the first
  rows are whatever the bytes decode to, and a trailing `...` marks the end of
  decoded memory.
* Vertical scrollbar.

### Bottom button row

Eight equal buttons across the memory + disassembly width (x 150–571,
y 396–423), MS Sans Serif 8 pt, classic 1px raised border:

`screen` `source` `reset` `aux` `vars` `debug` `stack` `flags`

The registers group runs the full height of the body on the left, so this row
does **not** extend under it.

### Screen panel (resizes in)

The window remembers a taller size (the user's is 728 x 556). When it is tall
enough, a `screen` panel appears **below** the button row, spanning the full
width, with the guest's text screen drawn black-on-black until something is
printed. This is the only part whose height is elastic.

## 5. The `original source code` window

Opened by the `source` button. Default 289 x 286, resizable, title
`original source code`.

* Same gutter + line numbers as the editor (decimal, `01`-based).
* Syntax colours: instructions/keywords **bold blue**, strings **Olive**,
  comments **dark green**, numbers black, registers dark red/brown.
* The **line currently executing is highlighted with a yellow band**
  (`#FFFF00`) across the full width.
* Vertical and horizontal scrollbars.

## 6. Runtime behaviour (measured)

These are the behaviours that make the clone feel like the original, each
confirmed by driving the live app:

1. **Emulate** assembles the editor's source, loads it, and opens the emulator
   window with `CS=IP=0700:0100`, `SS=0700`, `SP=FFFE`; the memory and
   disassembly panes both anchor at `0700:0100`.
2. **Run** executes until a breakpoint, a halt, or a keypress; the panels
   update continuously. While running the `run` button keeps its double
   chevron (it is not turned into a stop button); the menus provide `stop`.
3. **Single step** executes exactly one instruction:
   * `IP` advances; registers that changed are drawn in **blue text**;
   * the memory selection moves and covers the new instruction's bytes;
   * the disassembly selection moves to the new current instruction;
   * the source window's yellow band moves if it is open;
   * `step back` becomes enabled on the first step.
4. **Step back** rewinds one instruction, restoring registers *and* the
   memory writes the instruction made; the highlight follows.
5. **Reset** puts the machine back to the loaded-but-not-yet-run state
   (`CS:IP=0700:0100`, registers as in (1)) and clears the highlights.
6. **Editing a register field** and pressing Enter writes that register: the
   byte fields write `AH`/`AL` independently, the word fields write the whole
   register. `CS`, `IP`, `SS`, `SP` writes re-anchor the memory/disassembly
   panes to the new address.
7. **Editing an address field** re-anchors that pane without moving the CPU.
8. **Clicking a memory or disassembly row** selects it (blue) and shows the
   corresponding address in the pane's address field.
9. **Flags** are displayed as nine cells (`CF PF AF ZF SF TF IF DF OF`), value
   `0`/`1`, and are clickable to toggle the flag.
10. **Step delay** slider ranges 0..10 and is labelled `step delay ms: 0`;
    the value is milliseconds between automatic steps in `run`.
11. **The bottom buttons open windows**, they are not panels:
    `screen` (screen panel/window), `source` (source window),
    `reset` (action), `aux` (auxiliary-tools **menu**), `vars` (variables),
    `debug` (debug log), `stack` (`SS:SP` bottom-to-top), `flags`.

## 7. Menu trees (captured live)

Every popup below was opened by a scripted click on the caption and captured as
its own top-level window, so the size in the heading is the popup's real
window size. Popups are flat: white face, no icons, no left gutter, no
underline accelerators, no check column. Shortcut text is right-aligned and
grey (`#808080`), item text is black. Separators are single 1px `#D0D0D0`
rules, ~6px tall. Item pitch is **25px** except where a group is tightly
packed.

> **`ascii codes` is not a menu.** It is a top-level VB6 menu item with no
> children, so clicking the caption fires immediately and opens the
> `ascii codes` window (743x437). No popup appears. It is the only caption on
> either menu bar that behaves this way.

### Editor menu bar — client y 12

`file  edit  bookmarks  assembler  emulator  math  ascii codes  help`

**file** (366x544)
```
new                          >
examples                     >
---
open...
save
save as...
---
print...
export to HTML...
---
<15 most-recently-used .asm paths, full path, elided from the left with "..." >
---
exit
```

**edit** (280x421)
```
undo
redo
---
cut
copy
paste
---
select all
---
find...              Ctrl+F
find next            F3
replace...           Ctrl+H
---
go to line...        Ctrl+G
---
indent               Tab
outdent              Shift+Tab
---
comment block        Ctrl+Q
uncomment block      Ctrl+W
---
advanced editor macros     >
advanced                   >
```
`undo`, `redo`, `cut`, `copy` are greyed when they cannot apply (captured
state: nothing selected, nothing undone).

**bookmarks** (292x165)
```
toggle bookmark       Ctrl+F2
---
previous bookmark     Shift+F2
next bookmark         F2
---
jump to first
jump to last
---
clear all bookmarks
```

**assembler** (286x103)
```
compile
compile and load in the emulator    F5
fasm
---
set output directory...
```

**emulator** (280x50)
```
show emulator
assemble and load in the emulator
```

**math** (208x50)
```
multi base calculator
base converter
```
Note the captions: `multi base calculator` opens the window titled
`calculator - expression evaluator`; `base converter` opens the window titled
`base convertor`. The menu and the window title disagree by one letter.

**help** (261x90)
```
documentation and tutorials    F1
---
check for an update...
---
about...
```

### Emulator menu bar — client y 12

`file  math  debug  view  external  virtual devices  virtual drive  help`

**file** (319x544)
```
load executable...
reload                              F4
---
examples                            >
---
reset emulator and ram
---
set command line parameters
---
save the emulator's state
load from previous state            (greyed until a state was saved)
---
<15 most-recently-used .com/.bin/.exe paths>
---
close the emulator
```

**math** (208x50) — identical to the editor's math menu.

**debug** (374x324)
```
single step          F8
step over            Shift+F8
---
step back            F6   (greyed until the first step)
```
The popup is 374x324 but only these three rows carry text; the remaining
height is empty white. That is the original's own quirk (the popup is sized
for items that are hidden at runtime), so the clone should reproduce the
short visible list, not invent entries to fill the box.

**view** (270x413)
```
log and debug.exe emulation        F11
---
extended value viewer
---
stack
---
variables
memory
---
symbol table
listing
---
original source code
---
options
---
arithmetic & logical unit
---
flags
---
lexical flag analyzer
---
ascii codes
---
emulator screen
```

**external** (266x90)
```
start debug.exe (external)      F12
---
command prompt (external)
---
run (external)
```

**virtual devices** (196x213) — generated by listing the virtual-devices
folder, so the contents are machine-specific:
```
LED_Display.exe
Printer.exe
Robot.exe
Simple.exe
simplest.com
Stepper_motor.exe
Thermometer.exe
Traffic_Lights.exe
VGA_STATE.exe
```

**virtual drive** (320x112)
```
boot from virtual floppy                           >
---
write 512 bytes at 0000:7c00 to boot sector        >
write '.bin' file to floppy...
---
create new floppy drive
```

**help** (261x90) — identical to the editor's help menu.

### `ascii codes` window (743x437)

Opened directly by the menu item. Title `ascii codes`. Not resizable.

* CP437 table, **eight columns of 32**, one entry per row:
  `NNN:` decimal code, zero-padded to three digits, then the glyph.
* The seven control codes are named instead of drawn, in **red**:
  `null` (000), `beep` (007), `back` (008), `tab` (009), `newl` (010),
  `cret` (013), `spa` (032).
* Code 255 prints `res` in red where a glyph would be.
* Text is bold black monospace on white; no separators, no selection.

## 8. Window catalogue

Every window below was opened from a menu or button on the live app and
captured whole. Sizes are the **window** rectangles (what Win32 reports), so
they include the title bar; subtract ~27px for the client height.

| window | size | opened by | resizable |
|---|---|---|---|
| `edit: <path>` | 738x782 | the program itself | yes |
| `emulator: <bin>` | 581x443 | toolbar `emulate` / F5 | yes |
| `original source code` | 289x286 | emulator `source` button, `view`>original source code | yes |
| `ascii codes` | 743x437 | menu bar `ascii codes` (direct), `view`>ascii codes | no |
| `about emu8086` | 433x326 | editor `help`>about..., toolbar `about` | no |
| `options` | 508x333 | toolbar `options`, `view`>options | no |
| `base convertor` | 252x253 | `math`>base converter, toolbar `convertor` | no |
| `calculator - expression evaluator` | 319x247 | `math`>multi base calculator, toolbar `calculator` | yes |
| `Random Access Memory` | 661x189 | `view`>memory, ALU submenu>memory | yes |
| `stack` | 177x307 | `view`>stack, bottom row `stack` | yes |
| `variables` | 255x240 | `view`>variables, bottom row `vars` | yes |
| `flags` | 97x286 | `view`>flags, bottom row `flags` | no |
| `debug log - debug.exe emulation` | 693x248 | `view`>log and debug.exe emulation (F11), bottom row `debug` | yes |
| `extended value viewer` | 263x361 | `view`>extended value viewer | no |
| `lexical flag analyser` | 297x295 | `view`>lexical flag analyzer | yes |
| `emulator screen (80x25 chars)` | 743x374 | `view`>emulator screen | yes |
| `ALU - arithmetic & logic unit` | 435x132 | ALU submenu>ALU | no |
| `FPU` | 367x178 | ALU submenu>FPU | no |
| `stop on condition` | 436x122 | ALU submenu>stop on condition | no |
| `assembler status` | 413x417 | toolbar `compile` | yes |
| `choose code template` | 423x291 | startup when no document is given | no |

### `options` (508x333)

Not the flat list the clone currently draws. Two columns of group boxes:

Left column, top to bottom:
```
auto-indent mode: [indent on PROC v]
[ ] confine caret to text
[ ] keyword case normalization
[ ] show line numbers on thumb scroll
+ keywords ----------------------------+
| item: [...................... v]      |
| foreground color: [swatch]            |
| background color: [swatch]            |
| font style: [.......... v]            |
+---------------------------------------+
```

Right column:
```
+ fonts and colors --------------------+
| control: [source code editor v]      |
| background color: [swatch] [set font ...] |
+---------------------------------------+
+ tabs --------------------------------+
| tab size: [4]                        |
| [x] convert tabs to spaces while typing |
+---------------------------------------+
+ line numbers ------------------------+
| style: [decimal v]                   |
| start at: [1]                        |
+---------------------------------------+
                    [ close ]
                    [ reset all ]
[ associate .asm and .inc files ]   (full width, bottom)
```

The swatches are small sunken white squares that open the colour picker.
`close`, `reset all` are ordinary buttons; the associate button is full width
under them.

### `base convertor` (252x253)

```
(o) 8 bit          ( ) 16 bit
+---------------------------------------+
| hex:                        [ 0 ]     |
+---------------------------------------+
|              signed      unsigned     |
| dec:         [ 0 ]       [ 0 ]        |
+---------------------------------------+
| ascii char:                 [  ]      |
| oct:        [ 0                     ] |
| bin:        [ 00000000              ] |
+---------------------------------------+
```
The `ascii char:` field is short (about 30px) and right-aligned; `oct` and
`bin` span the width. All value fields are sunken white with bold monospace.

### `calculator - expression evaluator` (319x247)

```
+ show result as -----------------------+
| (o) decimal  ( ) hex  ( ) oct  ( ) bin|
+---------------------------------------+
+ treat hex,oct,bin as: ---+ [ clear ]
| [ ] signed   ( ) word ( ) byte | [ help  ]
+---------------------------+
+---------------------------------------+
|  multi-line expression / result pane   |
|                                       |  <- vertical + horizontal
|                                       |     scrollbars, size grip
+---------------------------------------+
```
`word`/`byte` are greyed when the result is decimal. The `clear`/`help`
buttons are stacked at the right of the second group.

The pane keeps both scrollbars on show (the VB6 control's `ScrollBars = 3`),
each 15 logical pixels thick with arrow buttons, and a dotted size grip in the
corner between them. The form is resizable; the controls keep their measured
size and position and only the pane and its bars grow, so maximising the
window adds canvas rather than stretching 11px type.

### Application icon

The About button's green chip is also the program's icon: `src/mue.ico` holds
it at 16, 20, 24, 32, 40, 48, 64, 128 and 256 px, is generated from the same
16x16 table by `python re/make_icon.py`, and is linked into the exe by
`mue.rc` (`1 ICON "src/mue.ico"`). Explorer, the taskbar and the title bar all
show it; `icon_app()` supplies the 32x32 HICON the window itself sets.

### `Random Access Memory` (661x189)

Row: `[ 0700:0100 ]` (sunken, bold) `[ update ]` `(o) table  ( ) list`.

`table` mode is a 16-byte hex dump per row:

```
0700:0100  EB 0E 48 65 6C 6C 6F 2C 20 77 6F 72 6C 64 21 00   .Hello, world!
0700:0110  BE 00 00 8A 84 02 01 3C 00 74 07 B4 0E CD 10 46   ...
```

* address column `SSSS:OOOO` in black bold;
* 16 hex byte columns, bold;
* ASCII column at the right, `.` for unprintable.
* The address increments by 16 per row and follows the segment, so the row
  labels are `0700:01F0` then `0700:0200`, not a rolling physical address.

`list` mode shows one byte per row instead (address, hex, decimal, char).

### `stack` (177x307)

Two columns, no header: `SSSS:OOOO` and the word at that address.
Rows start at `SS:SP` and go **down** in steps of 2 (`FFFE`, `FFFC`, `FFFA`,
...). The first row is selected when the window opens. Selection uses the
current Windows accent gradient, not a flat blue.

### `variables` (255x240)

```
size: [byte      v]     elements: [ 1 ]
[ edit ]
show as: [hex     v]
----------------------------------------
MSG                48h
```
The list has two columns (name, value); the value is rendered with its
size suffix (`48h` for hex). Row selection is the accent gradient.

### `flags` (97x286)

Eight rows, each a **combo box**, not a checkbox:

```
CF [0 v]     ZF [0 v]     SF [0 v]     OF [0 v]
PF [0 v]     AF [0 v]     IF [1 v]     DF [0 v]
[ analyse ]  (full width, bottom)
```

Order is exactly `CF ZF SF OF PF AF IF DF` (note: not the bit order).
The numeral is **blue for 0 and red for 1**. `analyse` opens the lexical flag
analyser.

### `lexical flag analyser` (297x295)

A read-only pane, bold monospace, both scrollbars:

```
=== unsigned ===
not below        (CF=0)
  above          (CF=0 and ZF=0)

=== signed ===
not less         (SF=0F)
  greater        (SF=0F and ZF=0)

not equal        (ZF=0)
positive         (SF=0)
no overflow      (0F=0)
parity odd       (PF=0)
not aux carry    (AF=0)
hw-int enabled   (IF=1)
forward dir      (DF=0)
```

The `SF=0F` is the original's own typo (meaning `SF=OF`); reproduce it
verbatim rather than "fixing" it, since the clone is a rendering comparison.

### `debug log - debug.exe emulation` (693x248)

Toolbar row: `[ clear ] [ save to file... ] [ ? ]`.
Body: a scrolled text pane in debug.exe format:

```
AX=0000  BX=0000  CX=0027  DX=0000  SP=FFFE  BP=0000  SI=0000  DI=0000
DS=0700  ES=0700  SS=0700  CS=0700  IP=0100  NU UP EI PL NZ NA PO NC
0700:0100  EB0E    JMP 0110h
```

Flags are the debug.exe two-letter pairs (`NU UP EI PL NZ NA PO NC`).
A one-line input strip sits at the very bottom.

### `extended value viewer` (263x361)

```
watch: [AL v]
 ( ) word      (o) byte
+---------------- L -------------------+
| hex:  [ 00        ]                  |
| bin:  [ 00000000  ]                  |
| oct:  [ 000       ]                  |
+--------------------------------------+
+ decimal 8 bit ------------------------+
| unsigned: [ 0 ]                       |
| signed:   [ 0 ]                       |
| ascii:    [   ]                       |
+---------------------------------------+
```
`L` is the right-aligned header over the value column (it becomes `H L` in
word mode). The rest of the window is empty form.

### `emulator screen (80x25 chars)` (743x374)

Title bar carries a yellow `80` icon. The client is a black 80x25 character
grid. A grey strip at the bottom holds `[ clear screen ]`, `[ change font ]`
and a right-aligned `row:col` cursor readout (`0:16`).

### `ALU - arithmetic & logic unit` (435x132)

A bit grid, 16 columns, no row labels:

* header nibbles `F E D C B A 9 8` in **dark red**, `7 6 5 4 3 2 1 0` in black;
* two value rows immediately under the header, then a gap, then a third row;
* values are bold monospace **blue** (`0`/`1`).

### `FPU` (367x178)

Plain text readout, grey bold captions, black bold values:

```
control register:
00 00 ... (14 values)
FPU registers:
00 x10   (eight rows)
```

### `stop on condition` (436x122)

```
        operand          condition        expression
    [ AX        v ]   [ =        v ]   [            ]
              hex values must start with 0 and have h suffix.   [ ? ]
```

Three centred column headers, a combo under each of the first two, a text
field under the third, a small hint line, and a `?` button at the bottom
right.

### `assembler status` (413x417)

```
[ external ] [ view ]
[ ---------------------------------------- ]   one-line list box
[ Assembled in 3 passes. Time spent: 0.113 seconds.
  entry point is ignored for .com files.
  "hello.com" is assembled successfully into 39 bytes.

  Listing is saved: "hello.com.list"
  Symbol table is saved: "hello.com.symbol" ]
[ black pane with caret                        ]
[ white pane                                   ]
[ view... ] [ external... ]   [ close ]  [ run ]
```

`view...` and `external...` are disabled until there is something to view.
The `run` button carries a small green play icon and is the widest.

Opening `compile` on a source that has never been built also raises the
Windows **Save As** dialog, defaulting to `C:\emu8086\MyBuild` and file type
`binary executable files (*.com)`.

The order of events was observed live, and the clone follows it exactly:

1. Clicking `compile` assembles immediately; the status window appears at once
   with `any code after 'END' directive is ignored.` (when that applies), the
   pass count and elapsed time, and `entry point is ignored for .com files.`
2. *While that window is already on screen*, the `Save As` box opens, owned by
   the editor so it stays above it, defaulting to `<home>\MyBuild\<stem>.com`.
3. Saving writes the image plus `<chosen>.~asm`, `.list`, `.symbol`, `.debug`
   and appends `"hello.com" is assembled successfully into 39 bytes.` plus the
   listing and symbol paths to the pane.
4. Cancelling writes nothing and appends `executable file is not created!`.

`run` is greyed out (label and play icon) until step 3 has actually produced
the image; `view...` and `external...` stay disabled.

### `about emu8086` (433x326)

Centred stack, all on the form face:

```
            emu8086            (bold monospace, large)
          version 4.08         (bold monospace)
   8086 microprocessor emulator
+-- this product is registered to: ------------------+
|                    ic0n0statse                     |
+----------------------------------------------------+
| Portions Copyright 1997-2008 Barry Allyn.  All rights reserved. |
+----------------------------------------------------+
+----------------------------------------------------+
| Flat Assembler      version 1.64                   |
| Copyright (c) 1999-2008, Tomasz Grysztar.          |
| All rights reserved.                               |
+----------------------------------------------------+
| Copyright (c) 2008 emu8086.com  All rights reserved. |
+----------------------------------------------------+
              www.emu8086.com      (blue, bold, link)
```

### `choose code template` (423x291)

Startup dialog when the program is launched without a document. Four template
radios (`COM`, `EXE`, `BIN`, `BOOT`) which load
`inc\0_com_template.txt` … `inc\3_boot_template.txt`, plus a `new, empty
workspace` / `the emulator` choice and `OK` / `Cancel`.

## 9. Toolbar dropdowns and editor submenus

The two toolbar buttons with a black down-arrow open their own popups.

**`examples`** (about 210x300):
```
Hello, world
add / subtract
calculate sum
compare numbers
binary, hex and octal values
traffic lights
palindrome
LED display test
stepper motor
simple i/o
---
more examples...
```

**`save`** (single item):
```
Save as...
```

**`edit` > `advanced editor macros`**:
```
Play Macro 1 .. Play Macro 10   (all greyed until recorded)
---
Repeat Next Command...      Ctrl+R
Record new Keystroke Macro
```

**`edit` > `advanced`**:
```
[x] show line numbers
---
tabify selection
untabify selection
---
lowercase selection         Ctrl+L
uppercase selection         Ctrl+U
---
display whitespace
```

These are the only menu items in either menu bar with a `>` chevron.

## 10. Status bar (editor)

Four sunken panels plus a size grip:

```
|            |             |                                    |
|  line: 5   |   col: 67   |        drag a file here to open     |
```

`line:`/`col:` are the caret position, 1-based; the wide panel shows hints
(`drag a file here to open` when idle). Panel boundaries sit at x ≈ 8, 40, 96,
190 and the grip at the right edge.

## 11. Syntax colours in the editor

Sampled from the live editor with `Hello.asm` open:

| token | colour |
|---|---|
| instruction mnemonic | **bold blue** (`#0000FF`) |
| register operand | **dark red** (`#800000`-ish) |
| string literal | **green** (`#008000`) |
| comment | **green** (`#008000`) |
| number | blue-black (rendered bold, near-black) |
| directive (`org`, `ends`, `end`) | bold blue |
| plain text | black |

These come from `C:\emu8086\Options\Keywords.dat`, which stores eleven
`(foreground, background, ?)` triples of 12 bytes each in `COLORREF` order
(low byte = B). The clone should read the same idea but ship sane defaults.

## 12. Filesystem layout

The install tree is documented separately in `re/FILESYSTEM.md`; the UI
consequences are: builds go to `\MyBuild`, new sources default to
`\MySource`, the virtual drive is `\vdrive\<letter>\`, and templates and
includes live in `\inc`.

## 13. Virtual devices

The `virtual devices` menu is generated at start-up by enumerating
`DEVICES\*.exe` (emu8086's documented behaviour), not from a hard-coded
list. Built-in models answer for `LED_Display.exe`, `Traffic_Lights.exe`,
`Stepper_motor.exe`, `Thermometer.exe`, `Robot.exe`, `Printer.exe`,
`Simple.exe` and `VGA_STATE.exe`; any other file found in the folder is
launched with `ShellExecute`, exactly as the original does.

Each device is an in-process FLTK window driven by the shared port hook in
`src/io.c` (see `re/DEVICES.md` for protocols, measured panels and the
reason for the black background). Selecting a device from the menu opens it;
assembling a source that mentions a device file name also opens it, after
all device state has been reset. The thermometer and robot advance on a
200 ms timer; the printer, LED display and traffic lights redraw on `OUT`.

The `file/examples/more examples...` action opens `examples\` in Explorer,
and `virtual drive/write '.bin' file to floppy...` copies a chosen image into
`vdrive\A\` before opening that folder. `virtual drive/create new floppy
drive` creates the next unused lettered folder. The emulator's
`file/load executable...` starts in `MyBuild\` with a `.com`/`.bin` filter
and loads the image at 0100h.

