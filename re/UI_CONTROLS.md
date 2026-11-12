# The emulator window: real control names and menu tree

Extracted from `emu8086.exe` string data. Every name below is CONFIRMED by an
exact match in the image, at the offset given; the captions are CONFIRMED where
the string sits next to its control name.

This replaces guesswork. Earlier notes reconstructed the emulator's menus from
a screenshot; these are the identifiers the program itself uses, so the clone
can wire the same commands to the same places instead of inventing a shape.

## The forms

emu8086 is a VB6 program, and each window is a form. The emulator is
`frmEmulation`, captioned `8086 microprocessor emulator` (CONFIRMED,
`0x6e53c`). The other windows the emulator can open:

| form | window caption | what it is |
| --- | --- | --- |
| `frmEmulation` | `8086 microprocessor emulator` | the main emulator |
| `frmMemory` | `Random Access Memory` | memory view |
| `frmStack` | `stack` | stack view |
| `frmVars` | `variables` | variables |
| `frmFLAGS` | `flags` | the flags register |
| `frmDebugLog` | `debug log - debug.exe emulation` | debug.exe transcript |
| `frmFlagAnalyzer` | `lexical flag analyser` | which instruction set which flag |
| `frmEvaluator` | `calculator - expression evaluator` | calculator |
| `frmEvaluatorHelper` | `calculator help` | calculator help |
| `frmBaseConvertor` | `base convertor` | base conversion |
| `frmASCII_CHARS` | `ascii codes` | ascii table |
| `frmExtendedViewer` | `extended value viewer` | one value, many forms |
| `frmEditVariable` | `edit variable or buffer` | edit a variable |
| `frmStopOnCondition` | `stop on condition` | conditional breakpoint |
| `frmSymbolTableViewer` | `symbol table` | symbols |
| `frmDOS_FILE` | `DTA settings` | DTA/find state |
| `frmAbout` | `about emu8086` | about box |
| `frm_mBox` | `message` | message box |
| `frmInfo` | `assembler status` | build progress |

All CONFIRMED at the `frm` name offsets 0x5aee4–0x6eb3f.

## The emulator's control names

These are real identifiers, CONFIRMED as contiguous names in the image.

**Registers** — `fraRegisters` frames the register group, and every register
is a separate text box, which is why each is independently editable:

    txtAH txtAL   txtBH txtBL   txtCH txtCL   txtDH txtDL
    txtSS txtSP   txtBP txtSI    txtDI txtDS   txtES

plus `txtIP` and `txtIntegratedMemoryAddr` for the memory bar.

**Toolbar and status** — `cmdReload` (`reload`), `cmdStep` (`single step`),
`cmdBack` (`step back`), `chkAutoStep` (`run`, tooltip
`make [single steps] automatically`), `lblStepTime` (the `step delay ms:`
label), `frmDat` (`8086 emulator`).

**Bottom button row** — eight commands, and this is the important part: their
identifiers and tooltips say exactly what each one opens.

| # | identifier | caption | tooltip |
| --- | --- | --- | --- |
| 1 | `cmdShowMemory`/`cmdDOS` | `screen` | — |
| 2 | `cmdShowActualSource` | `source` | `original source code` |
| 3 | `cmdReset` | `reset` | — |
| 4 | `cmdALU` | `aux` | `auxiliary tools` |
| 5 | `cmdVars` | `vars` | — |
| 6 | `cmdLog` | `debug` | — |
| 7 | `cmdStack` | `stack` | `memory at SS:SP from bottom to top` |
| 8 | `cmdFlags` | `flags` | `show flags register` |

The three tooltips are CONFIRMED verbatim at 0x6f9b6, 0x6fa10, 0x6fa8b,
0x6fa50. They settle what the panels are:

- **`stack`** is memory from `SS:SP` read **bottom to top**, not a call trace.
- **`source`** is the *original source*, not a disassembly.
- **`aux`** is the auxiliary tools menu, i.e. a menu, not a panel.
- **`flags`** is the flags register on its own.

Two more names worth recording: `picDragger` (a drag handle) and
`ON_EMULATOR_HIDE_PART1` / `ON_EMULATOR_HIDE_PART2` (teardown hooks),
`timerStartTurboMode` and `timerINT15_86` (the run timer and the real-time
clock behind INT 15h AH=86h).

## The menu bar, with real captions

Eight menus: `file` `math` `debug` `view` `external` `virtual devices`
`help`. Every item below is CONFIRMED (name at the offset, caption next to it).

### file
| identifier | caption |
| --- | --- |
| `mnuLoad` | `load executable...` |
| `mnuReLoad` | `reload` |
| `mnuSamples` | `examples` (a submenu of 10 shipped programs, then `more examples...`) |
| `mnuResetEmulator_and_RAM` | `reset emulator and RAM` |
| `mnuCommandLineParameters` | `set command line parameters...` |
| `mnuSaveCurrentState` | `save the emulator's state...` |
| `mnuLoadPreviousState` | `load from previous state...` |
| `mnuRecent` | `Recent 1`–`Recent 24`, then `other files...` |
| `mnuExit` | `close the emulator` |

### math
| `mnuEvaluator` | `multi base calculator` |
| `mnuCalculator` | `base converter` |

(Note the captions are the reverse of what the names suggest: `mnuEvaluator`
is the *multi base calculator*, `mnuCalculator` is the *base converter*.)

### debug
| `mnuSingleStep` | `single step` |
| `mnuStepOver` | `step over` |
| `mnuSingleStepBack` | `step back` |
| `mnuStopOnCondition` | `stop on condition...` |
| `mnuRunUntilSelected` | `run until (...)` |
| `mnuRun` | `run` |
| `mnuStop` | `stop` |
| `mnu_Set_BreakPoint` | `set break point (...)` |
| `mnu_Clear_BreakPoint` | `clear break point` |
| `mnuShowBreakPoint` | `show current break point` |
| `mnuSelect_lines_at_CS_IP` | `show current instruction` |
| `mnu_set_CS_IP_to_selected` | `set CS:IP to selected` |

### view
| `mnuDebugLog` | `log and debug.exe emulation` |
| `mnuExtendedRegisterView` | `extended value viewer` |
| `mnuStack` | `stack` |
| `mnuVariables` | `variables` |
| `mnuExternalMemoryViewer` | `memory` |
| `mnuShowSymbolTable` | `symbol table` |
| `mnuShowListing` | `listing` |
| `mnuActualSource` | `original source code` |
| `mnuOptions` | `options` |
| `mnuAlu` | `arithmetic && logical unit` |
| `mnuFlags` | `flags` |
| `mnuFlagAnalyzer` | `lexical flag analyzer` |
| `mnuASCII` | `ascii codes` |
| `mnuUserScreen` | `emulator screen` |

### external
| `mnuMS_DEBUG` | `start debug.exe (external)` |
| `mnuCommandPrompt` | `command prompt (external)` |
| `mnuExternalRun` | `run (external)` |

### virtual devices / help
`virtual devices` → `mnuDrive` (`virtual drive`), `mnuBootFromFloppy`
(`boot from virtual floppy`, with `FLOPPY_0`–`FLOPPY_3`),
`mnuWriteBootRecord` (`write 512 bytes at 0000:7C00...`),
`mnuWriteBinFileToFloppy` (`write '.bin' file to virtual floppy`),
`mnuMakeEmptyFloppy` (`create new floppy drive`).

## Why this list is worth having

The earlier UI was built from a screenshot, so it could only reproduce what
was visible. These identifiers are the parts a screenshot cannot show:

- that `aux` is a **menu of tools**, not a panel;
- that `stack` reads **`SS:SP` bottom-to-top**, not a call chain;
- that `source` shows the **original source**, not disassembly;
- that `math` holds two separate tools with **swapped-looking captions**;
- that the register boxes are **16 separate fields**;
- that `step back` and `step over` are distinct commands.

Labels: every form name, control name and caption above is CONFIRMED by
string match. The *submenu nesting* (which items sit under which menu) is
INFERRED from the menu captions and the documented screenshots, because the
VB6 menu metadata is a property stream and the tree is not stored as plain
text. Where nesting matters it is marked in the table above.
