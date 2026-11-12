# The emu8086 UI, from shipped documentation screenshots

## Why this file exists

The GUI has no byte oracle. Everywhere else in this project correctness is
settled by running both implementations and diffing bytes; a window cannot be
diffed that way. The next best thing is **evidence from the original's own
files** rather than my memory of what emu8086 looks like.

emu8086 ships screenshots of its own interface in
`C:\emu8086\documentation\img\`. They are the same application, captured by
its authors, and are therefore a legitimate reference. Copies are in
`re/ui_ref/` (PNG, converted from GIF for inspection).

This is **not** a pixel-replica brief. The clone is cross-platform (the user's
explicit decision) and will not look like VB6 on Windows. What is being
matched is **structure and content**: which panels exist, what each one shows,
what a user can do. See "What carries over" at the end.

## The evidence

### `emulator02.png` - 773x436, the emulation panel

Panel frames measured from black separator lines in the screenshot:

| Geometry | Region |
|---|---|
| `x=257` | left edge of the register/display column |
| `x=323` | divider between the register column and the wide centre panel |
| `x=543` | divider before the right controls column |
| `x=769` | right edge |
| `y=125` | top of the panel body |
| `y=426` | bottom of the panel body |

So the panel is a three-column layout: a narrow register column (257-323), a
wide centre display (323-543), and a controls column (543-769).

Reading the register column directly from pixel glyphs:

- pairs of small square **checkboxes** in a left sub-column - these are the
  per-register "highlight/watch" toggles
- a two-character **hex value** beside each checkbox
- the **currently changed register is drawn on yellow** (`#FFFF96`), which is
  emu8086's signature: when a step changes a register, that register's value
  turns yellow so you can see what just moved. This is the single most
  recognisable behaviour of the debugger and is worth reproducing.
- registers listed in the order AX, BX, CX, DX, ... with flags below

Colours in the capture: form background `#D4D0C8` (VB6 grey), panel white
`#FFFFFF`, text black, changed-register highlight `#FFFF96`, plus a mid grey
`#808080` for disabled/secondary items.

### `emulator03.png` - 728x524, the emulation panel with more visible

Same layout, taller, showing the memory/stack regions that `emulator02` cuts
off.

### `cpu.png` - 540x284

Close-up of the register block, showing the two-column checkbox + hex-value
arrangement and the flag row.

### `flags.png` - 212x179

The flag display on its own: one labelled checkbox/indicator per flag
(CF PF AF ZF SF TF IF DF OF), with the mnemonic beside each.

### `compile01.png` / `compile03.png`

The main window: source editor on top, compile/emulate action, and the
message pane below.

## What carries over to mue

**Structure, not skin.** The clone keeps these, because they are what makes
the tool usable:

1. **A register panel**, one row per register, hex value, with the
   just-changed registers highlighted. The highlight needs the step engine to
   report which registers changed - `cpu_step()` already runs one
   instruction, so this is a diff before/after, not new emulator work.
2. **A flag row** with all nine flags.
3. **A screen panel** showing the guest's video memory. This is the reason
   `screen.h` exists: the VRAM was private to `dos.c` and no front end could
   draw it.
4. **A source editor with assemble + step + run**, which the CLI already
   provides underneath (`mue -a` and `mue`).
5. **A memory viewer** and **stack viewer**, both reads of `MEM`/`cpu.sr[SS]`.
6. **A step log** - emu8086's `frmDebugLog`. `disasm_line()` already produces
   the text.

**What does not carry over:** the VB6 grey, the Win32 control shapes, the
7-segment renderings, the exact 773x436 geometry. Those exist because
emu8086 is a 2003 VB6 application. A cross-platform app should look like a
current one. Reproducing VB6 chrome would be cosplay, and the user explicitly
ruled out chasing Win32 so that the result stays portable.

## Caveat on this evidence

These screenshots are from the documentation and may not be the current
release's exact appearance. They are sufficient for **structure** and were
the only visible reference available. Where a detail cannot be read from
them, the behaviour is taken from the emulator source (`dos.c`, `exec.c`,
`cpu.h`) instead, which is the stronger oracle and the one this project has
used throughout.


## The live capture (2026-11-11): the main window, first screen

Text read from the running application is authoritative in a way the
documentation GIFs are not: it is the same build being cloned. This
supersedes the `compile01.png` guesses below.

### Window: maximized ~1919x1034

**Title bar** - green chip icon, then
`emu8086 - assembler and microprocessor emulator 4.08`, and the three
standard window buttons.

**Menu bar** (lowercase): `file  edit  bookmarks  assembler  emulator  math
ascii codes  help`

**Toolbar** - icon above lowercase label:

| Group | Buttons |
|---|---|
| file | `new`, `open`, `examples` (star + dropdown), `save` (floppy + dropdown) |
| build | `compile` (chip icon), `emulate` (green play triangle) |
| tools | `calculator`, `convertor` |
| help | `options` (hammer+wrench), `help` (question mark), `about` (chip) |

Vertical separators split file / build / tools / help.

**Editor** - the dominant area:
- a narrow darker gutter (~32px),
- a line-number column showing faded `01`,
- a white text canvas with a blinking caret at 1,1,
- a vertical scrollbar on the right, a horizontal one along the bottom.

**Status bar** - divided into panels; the centre one reads
`drag a file here to open`.

### What this corrects

My earlier reads of the pixel art were wrong in three ways, all of which
would have produced a wrong UI:

1. I read the menu as `file edit Compile-and-Emulate ...`. The **menus are
   not the actions** - `compile` and `emulate` are *toolbar buttons*; the
   menus are `file edit bookmarks assembler emulator math ascii codes help`.
   A UI built from my reading would have had a menu bar full of buttons.
2. I had no `bookmarks`, `assembler`, `emulator`, `math`, `ascii codes`
   menus at all - five of eight.
3. The toolbar icon+label pairs (12 of them) were missing entirely; I had
   drawn 3 text buttons.

**Lesson, and it is the same one as sections 47-50:** reading evidence
partially and then building is how you ship something confidently wrong. The
pixel art gave me letter runs; only a description gave me the structure.
Where the user can supply a description of what is on screen, ask for it
before writing layout code.


## The live capture (2026-11-11): the emulator window

Second screen: what `emulate` opens. Titled
`8086 microprocessor emulator`, about 708x545, non-maximized.

**Menu bar** (lowercase): `file  math  debug  view  external  virtual
devices  virtual drive  help`.

**Toolbar** - icon above label, with a thick border on `Load` (default):

`Load` (folder, focused) - `reload` (blue refresh) - `step back`
(left triangle, **greyed out/disabled when nothing has run**) -
`single step` (right triangle with bar) - `run` (double chevron) -
then at the far right a **slider with ticks** and the label
`step delay ms: 0`.

**Registers** (left, ~x 5-148), titled group box:
- `H` and `L` column headers, then **AX/BX/CX/DX each as TWO 2-digit
  boxes** (`00 | 00`) - the high and low bytes, separately editable
- then one wide box each: `CS=0050`, `IP`, `SS`, `SP`, `BP`, `SI`, `DI`,
  `DS`, `ES`
- CS is 0050, everything else 0000, because nothing is loaded

**Memory** (centre, ~x 153-420): sunken address field `0050:0000`, then a
list of rows, each `00500:` (grey address) + `00` (**green** byte) + `000`
(decimal) + `NULL` (bold black). Scrollbar.

**Disassembly** (right, ~x 428-706): sunken address field `0050:0000`, then
bold monospaced instructions; with all-zero memory every row reads
`ADD [BX + SI], AL` and the list ends with `...`. Scrollbar.

**Bottom button row** (8 buttons): `screen` `source` `reset` `aux` `vars`
`debug` `stack` `flags`.

### What this changes versus what I built

| emu8086 | what I had built |
|---|---|
| AX..DX shown as **H and L byte pairs** | one 4-hex box each |
| no per-register checkboxes | checkboxes I invented from a misread |
| `step back`, `single step`, `run` | `Run`, `Step`, `Reset` |
| `step delay ms` slider | absent |
| **memory view**: address, byte, decimal, ASCII | absent |
| **disassembly view**: live disassembly of memory | absent |
| 8 bottom panels: screen/source/reset/aux/vars/debug/stack/flags | absent |
| registers ordered CS,IP,SS,SP,BP,SI,DI,DS,ES | order I guessed |
| `step back` disabled until something has run | no such state |

The H/L split matters functionally, not just visually: those boxes are
separately editable, so a user can set `AL` without touching `AH`. A single
16-bit box cannot express that.

**Where the description and the older GIFs disagree, the description wins** -
it is the same build being cloned.


## The live capture (2026-11-11): three more windows

### 3. `options` - the editor settings dialog (635x418, fixed size)

No menu bar. Two columns.

Left: `auto-indent mode:` dropdown (`indent on PROC`); three unchecked
checkboxes `confine caret to text`, `keyword case normalization`,
`show line numbers on thumb scroll`; a `keywords` group containing an
`item:` dropdown, a separator, then **disabled** `foreground color` swatch,
`background color` swatch, `font style` dropdown (disabled because no item is
selected).

Right: `fonts and colors` group with a `control:` dropdown
(`source code editor`), a white `background color` swatch and a
`set font ...` button; `tabs` group with `tab size: 4` and a **checked**
`convert tabs to spaces while typing`; `line numbers` group with
`style: decimal` and `start at: 1`.

Buttons: `close` (default, bold, thick border), `reset all`, and a wide
`associate .asm and .inc files` across the bottom.

### 4. `base convertor` - floating tool window (~306x312)

Title icon reads `HEX`. Body:
- radio `8 bit` (selected) / `16 bit`
- `hex:` field
- `signed` and `unsigned` column headers over a `dec:` row with two fields
- `ascii char:` with a small box
- `oct:` field
- `bin:` field (8 digits in 8-bit mode)
- maximize button is **greyed out**; only minimize and close are live.

### 5. `calculator - expression...` - floating, resizable (~384x298)

- `show result as` radio row: `decimal` (selected) / `hex` / `oct` / `bin`
- `treat hex,oct,bin as:` group with a `signed` checkbox and
  `word` / `byte` radios that are **greyed out** while `signed` is off
- `clear` and `help` buttons stacked at the right
- a large multi-line expression/result box with both scrollbars
- a **resize grip** in the bottom-right corner (this window is resizable,
  unlike the convertor)

### Windows inventory so far

| Window | Purpose | Resizable |
|---|---|---|
| main editor | source + menus + toolbar | yes (maximized) |
| `8086 microprocessor emulator` | registers / memory / disassembly | yes |
| `options` | editor settings | **no** |
| `base convertor` | hex/dec/oct/bin/ascii | **no** (maximize greyed) |
| `calculator - expression...` | expression evaluation | **yes** (resize grip) |

That "which windows can resize" column is not cosmetic: it is the visible
difference between a fixed dialog and a tool window, and it is the kind of
detail that makes a clone feel wrong when it is missing.
