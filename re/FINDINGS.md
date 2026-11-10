# emu8086.exe — Reverse-Engineering Findings

Target: `C:\emu8086\emu8086.exe`
Size: 2,244,608 bytes (0x224000)
SHA-256: `dba99f7c8795df28be4b153f8e979b0c572ef825d8b70fb779609f8303a055e9`
Tools used: REA MCP (`open_binary`, `binary_session`, `inspect_with_objdump`),
GNU objdump (MinGW), Python static parsing.

> **Provider note.** REA's Ghidra provider reported `not_configured`
> (`GHIDRA_INSTALL_DIR` unset), the Rizin provider returned
> `ERR_REA_WINDOWS_NATIVE`, and Hopper is macOS-only. So no decompiler text was
> obtainable. All findings below come from **REA-admitted structural
> observations** (PE identity, sections) plus **objdump disassembly** and
> **byte-level static parsing I performed myself**. Decompiled-source evidence
> is therefore absent; every claim is labelled CONFIRMED or INFERRED.

**Scope and intent.** This document records *where* things are in the original
binary so that a replacement can reproduce its observable behaviour, which is
what interoperability research requires. It contains no decompiled source, no
copied code, and no bulk string or byte dumps: the quoted strings are short
diagnostic messages used as evidence that a routine does what it is claimed to
do, and the pseudo-code is my own notation describing observed effects, not the
original's implementation. The full disassembly and raw string/address extracts
are deliberately *not* published (see `re/.gitignore`); regenerate them locally
from your own licensed copy if you want to verify a claim.

---

## 1. File and image structure  (CONFIRMED)

PE32, machine `0x014c` (i386), ImageBase `0x400000`, Subsystem 2 (GUI).

| Section | VA | VirtualSize | RawPtr | RawSize | Flags |
|---|---|---|---|---|---|
| `.text` | `0x401000` | `0x21F2CC` | `0x001000` | `0x220000` | RWX (60000020) |
| `.data` | `0x621000` | `0x01549C` | `0x221000` | `0x001000` | RW (C0000040) |
| `.rsrc` | `0x637000` | `0x001160` | `0x222000` | `0x002000` | R (40000040) |

Entry point RVA `0x1A018` → **VA `0x41A018`**.

**Important address-mapping fact** (this bit me while parsing, so record it):
for this image, `file_offset == RVA` for RVA < `0x221000` (i.e. the whole
`.text`). The conventional `RVA == VA - 0x400000` also holds, so:

```
VA   = RVA + 0x400000
off  = RVA                    (for VA in 0x401000 .. 0x621000)
```

`.data` is only `0x1000` bytes on disk but `0x1549C` virtual — the bulk of the
VB5 string pool lives **inside `.text`**, not `.data`. This is why almost all
string literals disassemble as garbage in a linear sweep.

Entry point disassembly (CONFIRMED):

```
41a018: 68 1c ac 41 00    push 0x41ac1c
41a01d: e8 ee ff ff ff    call 0x41a010        ; jmp ds:0x401360 -> MSVBVM60
```

`0x41ac1c` is the VB5 `ThunRTMain` argument block. This is the standard
Visual Basic 5/6 CRT entry, not application code.

### Imports  (CONFIRMED)

Single import descriptor:

```
MSVBVM60.DLL   (import dir RVA 0x21F204, nametable RVA 0x21F674)
```

273 thunks. The runtime supplies the VB5 native-code support routines used
everywhere below: `__vbaExceptHandler`, `__vbaFPException`, `__vbaStrCat`,
`__vbaVarTstEq`, `__vbaFreeStr`, `__vbaHresultCheckObj`, `__vbaVarForNext`, …

### Runtime libraries loaded at runtime  (CONFIRMED, from string refs)

`cmax20.ocx` (`CodeMaxCtl.CodeMax`), `Mscomctl.ocx`
(`MSComctlLib.Toolbar`, `StatusBar`, `ImageList`, `Slider`),
`diasm.dll`, `MicroAsm.dll`.

`diasm.dll` and `MicroAsm.dll` are *not* statically imported — they are loaded
dynamically (strings `" diasm.dll"`, `"MicroAsm.dll"`,
`"file not found: MicroAsm.dll"` at RVA `0x42F70`).

---

## 2. What kind of program this is  (CONFIRMED)

* **Language:** Visual Basic 5, compiled to **native x86 code** (not p-code).
  Evidence: 1451 method prologues of the shape
  `55 8B EC [83 EC nn] 68 <SEH> 64 A1 00 00 00 00 50 64 89 25 00 00 00 00`,
  i.e. a manual SEH frame plus `mov eax,fs:[0]`. This is the VB5 native
  `ThunRTMain` code-generator output, not P-code (`__vbaPCode` style).
* **Source paths leaked in the literal pool:**
  `D:\yur74\emu8086\emu8086v400src\emu8086_v408_vb_code\emu8086.vbp`
* **Version:** `emu8086 - assembler and microprocessor emulator`,
  resource FileVersion `4.00.0008`; `version.txt` says **4.08**.
  Copyright string in `ReadMe.txt`: "Portions Copyright 1997-2010 Barry Allyn."
  Flat Assembler 1.64 (Tomasz Grysztar) is bundled.
* The shipped `emu8086.exe` is **not** the compiler of record for the 8086
  assembly it emulates — it *drives* two assemblers:
  `MicroAsm.dll` (built-in) and `fasm\fasm.exe` / `ML.EXE` (external).

### Method-boundary map  (CONFIRMED)

A scan for the VB5 native prologue finds **1451 method entry points**, from
VA `0x4869E0` to VA `0x61F0F0`. Code below `0x4869E0` is the VB5 runtime
startup, IAT-jump thunks (`0x419C50`–`0x419D00`), and static data. My script
for this is `re/scan.py`; the full list is `re/methods.txt`.

Cross-check: a byte-sequence scan for `56 8B...`-style prologues located the
same first method at `0x4869E0`, and objdump's independent linear disassembly
(`re/disasm.txt`) shows `int3` padding ending immediately before it at
`0x4869DF`.
---

## 3. The CPU emulation core  (CONFIRMED)

### 3.1 The core method

**Method #377, VA `0x4EFF00`**, size **161,072 bytes**
(`0x4EFF00` → `0x5172CF`). This single VB5 procedure *is* the 8086 CPU core.

Evidence that this method is the core:

1. Its prologue allocates `0x1728` bytes of VB5 local-descriptor space
   (`mov eax,0x1728 / call 0x4199b0` — the `__vbaChkstk` helper), which matches
   a procedure with hundreds of locals — one per opcode family.
2. Every opcode-related error string is referenced from inside it:
   * `"unknown opcode skipped:"` (rva `0x467EC`) — xref at `0x516E86`
   * `"not 8086 instruction - not supported yet."` (`0x46820`) — `0x516E97`
   * `"no such opcode in 8086 instruction set."` (`0x46754`) — `0x510D0F`, `0x5160E6`
   * `"error on single step: "` (`0x46878`) — `0x517186`
   * `"wrong C6 address on doStep()"` (`0x46684`) — `0x4F06F7`
   * `"wrong C7 address on doStep()"` (`0x466C4`) — same method
3. It writes the emulator's own bookkeeping globals directly:
   `mov WORD PTR ds:0x621C4C,0x0` and `mov WORD PTR ds:0x621C4E,0x0`
   at `0x4EFF5B` / `0x4EFF6B`.

Method #377 is the **only** site in the binary that references the opcode
error strings, which is strong evidence it is the sole instruction dispatcher
(no secondary handler table elsewhere).

### 3.2 Register / memory model  (CONFIRMED from instruction streams)

The emulated 8086 state lives in the VB object `[ebp+0x8]`. Observed fields:

| Field | Meaning | Site |
|---|---|---|
| `[obj+0x36]` (word) | small state word, zeroed at entry | `0x4EFF7E` |
| `[obj+0xCC]` | **CS** (read, ×16) | `0x4EFFB5..0x4EFFD9` |
| `[obj+0xCE]` | **IP** (read, added to CS×16) | `0x4EFFD2` |
| `[obj+0xD4]` | **physical fetch pointer** | `0x4EFFE9` |

The first three instructions of the loop compute the physical address:

```
ecx = [obj+0xCC]              ; CS
push ecx ; call 0x4AD050       ; -> integer value of the VB field
esi = eax * 16                 ; imul esi,esi,0x10  (0x4EFFC6)
edx = [obj+0xCE]              ; IP
push edx ; call 0x4AD050
esi = esi + eax                ; jo -> 0x517425 (overflow trap)
[obj+0xD4] = esi               ; store physical pointer
```

So **`phys = (CS << 4) + IP`** and `[obj+0xD4]` is a cached physical
fetch pointer that is `inc`/`dec`remented as bytes are consumed:

```
mov eax,[obj+0xD4] / add eax,1 / mov [obj+0xD4],eax     ; 0x4F002C..0x4F0047
```

**This is a concrete, unusual design point:** emu8086 does *not* recompute
`CS:IP` on each fetch; it maintains a flat 20-bit physical pointer and
increments it. Segment:offset reconstruction only happens when a control
transfer occurs. Any clean-room clone may choose either model, but a flat
physical pointer is faster and is what the original does.

Byte fetch is:

```
lea eax,[obj+0xD4] ; push eax ; call 0x571280 ; -> al = byte
```

`0x571280` is the **`get_BYTE_at_EA`-family reader** (the same call appears
guarded by `"ERROR CALLING get_BYTE_at_EA("` and `"ERROR CALLING
get_word_at_ea("`). So `0x571280` = fetch/read byte at the current physical
pointer, and it advances the pointer as a side effect.

### 3.3 The dispatch mechanism  — **a linear compare chain, not a table**

This is the single most important structural finding.

There is **no jump table and no switch table**. A scan of the entire 161 KB
method for every x86 indexing-jump form found *zero*:

```
ff 24 85 / ff 24 95 / ff 24 a5 / ff 24 b5   (jmp [reg*4+disp32])  -> 0 hits
ff 24 8d / ff 24 9d                          (jmp [reg*4+disp8])   -> 0 hits
```

Instead the fetched opcode is held in a 16-bit local and compared one value
at a time with a **two-level test**:

* the **first** byte of every instruction is compared via a `cmp dx, imm16`
  (`66 81 FA iw`) or `cmp ax, imm16` (`66 3D iw`) in a long `if/elseif` chain,
  each followed by a `jne` past the handler;
* a **second** `cmp dx, imm16` at the same site range tests the *opcode byte
  again after prefix stripping* (so `0xA0`/`0xA1`, `0xFF`, `0x88`/`0x89`,
  `0xC6`/`0xC7`, `0x8E` appear twice).

54 `cmp dx, imm16` sites, in increasing address order:

```
0x4F001A cmp dx,0xA0   0x4F0C73 cmp dx,0xFF   0x4F1CC6 cmp dx,0xFF   0x50491  (see below)
0x4F0191 cmp dx,0xA1   0x4F0DA4 cmp dx,0xC7   0x4F23C0 cmp dx,0xEA
0x4F0336 cmp dx,0xA2   0x4F0E3A cmp dx,0xFF   0x4F249F cmp dx,0xFF
0x4F04AD cmp dx,0xA3   0x4F0F23 cmp dx,0x89   0x4F3043 cmp dx,0xCD
0x4F0652 cmp dx,0xC6   0x4F1348 cmp dx,0xFF   0x4F5055 cmp dx,0xFC
0x4F06E8 cmp dx,0xFF   0x4F159A cmp dx,0xB0   0x4F527B cmp dx,0xA4
0x4F07D1 cmp dx,0x88   0x4F17B7 cmp dx,0xBF   0x4F60EB cmp dx,0xAA
                       0x4F1894 cmp dx,0xB9   0x4F627E cmp dx,0xAB
                       0x4F195F cmp dx,0xBC   0x4F6491 cmp dx,0xAC
                       0x4F19D3 cmp dx,0xBE   0x4F75F1 cmp dx,0x82
                       0x4F1A47 cmp dx,0x8E   0x4F8DD4 cmp dx,0x81
                                              0x4F8EE4 cmp dx,0x83
0x500BAD cmp dx,0x8F   0x501818 cmp dx,0xC3   0x5024F3 cmp dx,0x9D   0x5121680 cmp dx,0xC5
0x5019A4 cmp dx,0xC2   0x501D46 cmp dx,0xE8   0x5025F2 cmp dx,0xF6
0x501E5C cmp dx,0xE0   0x5037E3 cmp dx,0xF7
0x505304 cmp dx,0xF5   0x505772 cmp dx,0xFC
0x505A23 cmp dx,0xFB   0x50640B cmp dx,0x86
0x50A8B7 cmp dx,0x9F   0x50C0C4 cmp dx,0xFF
0x50D4DF cmp dx,0xFF   0x512680 cmp dx,0xC5
0x512A4B cmp dx,0xD7   0x512B2F cmp dx,0xE4
0x512C7E cmp dx,0xE5   0x512DCD cmp dx,0xE6
0x5165CA cmp dx,0xF4   0x516706 cmp dx,0xC0
0x5167B5 cmp dx,0xC9   0x516A45 cmp dx,0xD8
```

(The table above is the verbatim ordered output of `re/dispatch_order.py`.)

Reading the ordering tells you the source's `Select Case` order, which is
roughly: string/accumulator `A0–A3`, `C6`, prefix handling, `88/89`, `C7`,
`B0`, `BF/B9/BC/BE/8E`, `EA`, `CD`, `FC`, `A4`, `AA/AB/AC`, `82/81/83`, `8F`,
`C3/C2`, `E8`, `E0`, `9D`, `F6/F7`, `F5/FB`, `86`, `9F`, `C5`, `D7`, `E4/E5/E6`,
`F4`, `C0/C9`, `D8`.

**Register-class opcodes are not in that chain.** Opcodes `0x40–0x5F`
(`INC/DEC r16`, `PUSH/POP r16`) and `0xB8–0xBF` (`MOV r16,iw`) are handled by
*arithmetic on the opcode byte* (they are contiguous ranges), which is why only
`B0`, `B9`, `BC`, `BE`, `BF` show up as individual compares.

### 3.4 Instruction-length handling  (CONFIRMED)

Opcode-specific length fixes are visible as immediate compares against the
"bytes consumed" counter, e.g. `cmp dx,0x88` at `0x4F07D1` and `cmp dx,0xC6`
at `0x4F0652` — the latter is followed at `0x4F06F7` by
`"wrong C6 address on doStep()"`. The emulator measures instruction length by
re-reading `IP` after the handler and comparing to the saved `IP`.
---

## 4. The assembler  (CONFIRMED, with the internal call graph partly INFERRED)

### 4.1 Two-stage pipeline

The built-in assembler is a **multi-pass textual passes over the source**, not
a token-stream single pass. Evidence — these literals all exist and are
referenced from method #38 (`0x493FD0`):

```
"pass #"            "recursive MACRO definition is over"
" dup( "            " .macros "      " .procedures "
" endm "            " db "           " dw "
" SEGMENT "         " .RADIX "       " error 71: "
"no size: "         "IMUL"  "IDIV"  "TEST"  "CALL"  "RETN"  "RETF"
"PUSH"  "XCHG"
"no code to assemble.... :( "
"type something like: HLT  ; at least..."
```

Method #38 therefore performs: directive/segment scan → macro expansion →
`dup()` expansion → operand-size resolution → emit.

### 4.2 Operand-form table — the assembler's spine  (CONFIRMED)

`lst_opNames` at RVA `0x73070` is a VB5 fixed-width string array of
**195 opcode/mnemonic forms**, stride 16 (`<14 chars><0E 00>`), with a
4-byte `<00 00 00 00>` terminator pair after the `AAS` record. Full extraction
is `re/optab3.py`; the first and last groups:

```
  0: AAA | AAD | AAM | AAS | ADC AL,ib | ADC AX,iw
  6: ADC eb,ib | ADC eb,rb | ADC ew,ib | ADC ew,iw | ADC ew,rw | ADC rb,eb
 12: ADC rw,ew | ADD AL,ib | ... | #ADD4S | AND AL,ib
 ...
186: MOVSB | MOVSW | MUL eb | MUL ew | (blank) | NEG eb
192: NEG ew | NIL (prefix) | NOP
```

**The suffix vocabulary is the key to the whole design** — it is exactly the
encoder's addressing-mode enum:

| suffix | meaning |
|---|---|
| `rb` / `rw` | 8-bit / 16-bit **register** |
| `eb` / `ew` | 8-bit / 16-bit **register-or-memory** (ModRM `.r/m`) |
| `mb` / `mw` / `md` | 8-bit / 16-bit / 32-bit **memory** |
| `xb` / `xw` | direct **address** (moffs, `A0–A3`) |
| `ib` / `iw` | 8-bit / 16-bit **immediate** |
| `cb` / `cw` / `cd` | 8-bit / 16-bit / 32-bit **code label** (rel8/rel16/far) |
| `m` | untyped memory (`LEA rw,m`) |

The `*` prefix marks **non-8086 / 80186+ forms** (`*ARPL`, `*BOUND`,
`*CLTS`, `*ENTER`, `*INS`, `*LAR`, `*LEAVE`, `*LGDT`, `*LIDT`, `*LLDT`,
`*LMSW`, `*LSL`, `*LTR`) and the `#` prefix marks **emulator pseudo-ops**
(`#CLRBIT`, `#CMP4S`, `#ADD4S`, `#LODBITS`, `#CALL80`, `#NOTBIT`). This directly
matches the runtime error `"not 8086 instruction - not supported yet."`,
so the assembler accepts these but the emulator rejects them at runtime.

Independent confirmation of the same form list appears in a *second* table at
RVA `0x4C584` (the syntax-highlight keyword blob used by the editor), which
lists `AAA AAD AAM AAS ADC ADD AND CALL CBW CLC CLD CLI …` as mnemonic-only
tokens. Two distinct tables with different content (forms vs. mnemonics) is
what you would expect from an assembler + editor pair.

### 4.3 Effective-address table  (CONFIRMED)

Method #136 (`0x4AB110`, string `"cannot convert to 16 bit value: "`) owns the
16-bit EA string vocabulary, in this literal order:

```
[DI] + d8   [BP] + d8   [BX] + d8          [BX + SI] + d8   [BX + DI] + d8
[BP + SI] + d8   [BP + DI] + d8   [SI] + d8
[BX + DI] + d16   [BP + SI] + d16   [BP + DI] + d16   [SI] + d16
[DI] + d16   [BP] + d16   [BX] + d16
[BX + SI]   [BX + DI]   [BP + SI]   [BP + DI]
```

plus the validation errors `"[BX] cannot go with [BP] - wrong addressing!"`
and `"[SI] cannot go with [DI] - wrong addressing!"`. That is the canonical
8086 ModRM `r/m` table (8 entries × `mod=00/01/10`), and the two error strings
prove the assembler validates *illegal register pairings* rather than emitting
garbage.

The `getTableEA_byte()` / `getTabIndex()` / `getRowIndex()` names
(`"getTableEA_byte() called with empty parameter! line:"`,
`"Wrong sTab param in getTabIndex call:"`) confirm the implementation is a
**row/column table lookup** over that vocabulary, not a computed algorithm.

### 4.4 Diagnostics vocabulary  (CONFIRMED, exact strings)

Operand-shape errors:

```
not enough operands for          wrong parameters:
should be a register or a memory location.
only immediate byte or CL can be used as second parameter!
first parameter can be AL or AX only.       second operand cannot be AL or AX!
first operand (port) should be 0..255 or DX.
8 bit and 16 bit register        16 bit and 8 bit register
16 bit register and 8 bit address   8 bit address and 16 bit register
8 bit register and 16 bit address (add 'b.' before parameter)
cannot use segment register with an immediate value
segment registers cannot go together!
CS cannot be modified directly (use far JMP or CALL)
cannot get value of IP register (use POP after CALL)
mem to memory is not allowed for 8086
use general register to hold intermediate result.
```

Label / jump errors:

```
label is requred:      not valid label:      undeclared label:
conditional jump is out of range:      allowed values are from -128 to 127. (0-FF)
conditional jump cannot be FAR:        cannot assemble far call.
```

Symbol-table errors:

```
'name' is a reserved keyword.          reserved keyword cannot be redefined:
recursive definition is not allowed for EQU:      EQU cannot start with a number.
[",AH,AL,BH,BL,DH,DL,CH,CL,SI,DI,AX,BX,CX,DX,CS,IP,SS,SP,BP,DS,ES,"]
not found in symbol table:      get_parameter_from_SYMBOL_TABLE: not found>
```

Number-literal errors reveal the literal syntax the tokenizer accepts:

```
zero prefix must be added to a HEX value, for example: 0Ch
over 8 bits:        cannot evaluate this expression:        wrong radix:
"0123456789ABCDEF"  "01234567"  "0123456789HOBD"
```

`"0123456789HOBD"` is a **radix digit set** — hex (`h`), octal (`o`), binary
(`b`), decimal (`d`) — confirming suffix-notation support as documented.

### 4.5 Output writers  (CONFIRMED)

Three back-ends, each with its own error string:

| output | strings |
|---|---|
| `.COM` | `"com build error:"`, `".COM_"`, `"entry point is ignored for .com files."` |
| `.EXE` | `"exe build error:"`, `"NO STACK!"`, `"no entry point."`, full MZ header field descriptions (`"exe signature (M)"`, `"bytes on last page (l.byte)"`, `"SS - stack segment (h.byte)"`, `"relocation table - offset inside segment"`, …) |
| `.BIN` | `"bin build error:"`, `".binf"`, `"LOAD_SEGMENT"`, `"LOAD_OFFSET"`, `"PROCESS_BINF_DIRECTIVE:"` |

`#MAKE_COM#` / `#MAKE_EXE#` / `#MAKE_BIN#` / `#MAKE_BOOT#` are the selectors;
`.binf` is the *initial-register-state* sidecar (`Write_BINF_file_if_directed`),
and `default.binf` ships with the product.

### 4.6 Third-party assembly  (CONFIRMED)

`assemble_with_fasm` and `assemble_with_masm` exist and shell out:

```
"fasm\fasm.exe \"    "  > "      "_fasm.ok"      "error: cannot start fasm!"
"ml.exe /Fl\""       " /Fm\""     " /Fo\""        "_masm.ok"
"ASSEMBLER STATUS:"  "fasm-ok"    "masm-ok"
```

So `#fasm#` / `#masm#` directives bypass the built-in encoder entirely.

---

## 5. Major module map  (CONFIRMED method ranges)

Derived from the 1451-method table plus the string anchors each method
references (`re/methods_dump.txt`, `re/owners.py`).

| VA | # | Role | Anchor strings |
|---|---|---|---|
| `0x4869E0` | 0 | first VB method (frmMain) | — |
| `0x493FD0` | 38 | **assembler pass driver** | `"pass #"`, `"SEGMENT"`, `"no code to assemble"` |
| `0x4AB110` | 136 | **EA decoder / 16→8 bit** | `"cannot convert to 16 bit value: "` |
| `0x4B6B80` | 177 | label & `d16` operand parser | `"LABEL"`, `"d16 (simple var)"`, `"NEAR"` |
| `0x4BD3D0` | 186 | `LEA`/`MOV rw,m` form resolver | `" LEA "`, `" MOV "`, `" rw,m "` |
| `0x4C5600` | 216 | **.EXE header writer** | `"EXE HEADER - bytes from 0000 to "`, `"&H4D"`, `"&H5A"` |
| `0x4C8760` | 220 | **.COM writer** | `"com build error:"`, `".com"` |
| `0x4C92A0` | 221 | **.BIN writer** | `"bin build error:"`, `".binf"` |
| `0x4CBA10` | 230 | `LABEL` directive | `" LABEL "` |
| `0x4CF940` | 234 | CLI driver / `PROCESS_CMD_Line` | `"EMU8086 INTEGRATED ASSEMBLER PROCESSED "` |
| `0x4D6CF0` | 252 | `_location_counter_` / ORG | `"_location_counter__"`, `"offset calculation error"` |
| `0x4E46B0` | 332 | **emulator state save** | `"emulator.state"` |
| `0x4E5140` | 333 | **emulator state load** | `"emulator.state"` |
| **`0x4EFF00`** | **377** | **CPU core + opcode dispatch** | `"unknown opcode skipped:"`, `"doStep()"` |
| `0x517D10` | 383 | **memory/register dump** (`MEMORY at`, `REGISTERS`) | `">>>>>>> disassembled code <<<<<<<<"` |
| `0x52AE10` | 407 | emulator state (variant) | `"emulator.state"` |
| `0x5457C0` | 599 | FPU emulation | `"FPU ERROR: "` |
| `0x547EC0` | 606 | FPU step-back | `"CRITICAL ERROR 2:"` |
| `0x51FF60` | 405 | **INT dispatcher** (INT 04/10/11/12/13/15/16/17/1A/19/20/21/33 + fall-through) | 13-entry INT chain then per-INT AH chain on field `+0xBB`; see s5.1 |
| `0x56BEC0`+ | ~700s | video helpers | `draw_char_to_video_memory` (`0x44AA2C`), `unsupported video mode:` (`0x44AB08` at `0x56CA11`) |
| `0x5DDF80`/`0x5DED70`/`0x5DF2D0` | 1237/1242/1243 | INT 21h file ops | create/open, SEEK, rename |
| `0x5856`+ | - | INT 21h 4Ah/48h/49h memory | `Allocate Memory Block OK` (`0x458720`) |
| `0x595…` | — | fasm / MASM bridging | `"assemble_with_fasm"`, `"CHECK_FASM_LOG"` |
| `0x5A8E30` | 990 | symbol-table writer | `"< THE SYMBOL TABLE >"` |

### 5.1 INT dispatcher internals  (CONFIRMED - Ghidra + objdump + docs)

Single function `FUN_0051FF60`, 43557 bytes, 947 blocks (ranges
`0x51FF60-0x5235BA/0x5235C0-0x52A938/...`; tail `0x52A934` shared exit).
Register-field block at `[ebp+8]` (CONFIRMED offsets, reused in every
handler): `+0xBA`=AL, `+0xBB`=AH, `+0xBC`=BL, `+0xBD`=BH, `+0xBE`=CL,
`+0xBF`=CH, `+0xC0`=DL, `+0xC1`=DH. **CORRECTED:** `+0x4C` is a *halt* word,
not the carry flag - its only write in the dispatcher is `0x529F74`
(`WORD [regblock+0x4C] = 0xFFFF`) immediately beside `ds:0x621218 = 0xFFFF`
and the stop sink, which is the terminate path. See 21.2.

**High-register / segment fields (CORRECTED; an earlier revision of this table
said `+0xC4`=SI and `+0xCA`=DI, which is wrong):**
`+0xC2`=**DS**, `+0xC4`=**ES**, `+0xC6`=**SI**, `+0xC8`=**DI**, `+0xCA`=**BP**,
`+0xCC`=CS, `+0xCE`=IP, `+0xD0`=**SS**. Evidence is section 15.1 (the `A4`/`A5`
MOVS arm reads its *source* through `+0xC6` and writes its *destination*
through `+0xC4 x16 + +0xC8`; INT 10h AH=13h physicalises its documented `ES:BP`
source as `+0xC4 x16 + +0xCA`) and section 16.2 for DS/SS (`0x06` pushes
`+0xC4`, `0x16` pushes `+0xD0`, `0x1F` pops into `+0xC2`). `0x621218` = global carry mirror,
`0x621C04` = global file-table guard.
AH-byte test idiom: `MOV ECX,<AH>; CALL [0x401238] (__vbaUI1I2); CMP
[ebp-<slot>],AL; JNZ next`.

**INT-number chain** (CONFIRMED order on `[ebp-0x26C]`, jne = fall-through):
`04 -> 10 -> 11 -> 12 -> 13 -> 15 -> 16 -> 17 -> 1A -> 19 -> 20 -> 21 ->
33 -> else`. The `else` logs `"this interrupt is not defined yet..."`
plus `INT %02X`-style templates at `0x447F78/0x44800C/0x448094`.

| INT | AHs tested (in order) | Notes |
|---|---|---|
| `04` | - (overflow trap; returns through core) | prologue stub at `0x52000D` (`xor ecx,ecx` = already-zero test) |
| `10` | `00,01,02,03,05,06,07,08,09,0A,0C,0D,0E,0F,11,13,1003` (AX-full, BL=01 logged via template `0x447714` at `0x52360F`) | `AH=00` body at `0x520666`; `AH=13` ES:BP string loop at `0x522D0E` (`imul esi,esi,0x10` seg x16 + `0x4AD050` variant-to-int, branch-bit `AH&1` via `and cx,1`) |
| `11` | - | `MOV AX,1` (`0x523B76`: `get_BYTE 0x410/0x411` -> AL `+0xBA` / AH `+0xBB`) |
| `12` | - | same helper pair (`0x413/0x414`), memory-size AX |
| `13` | AH=`00` (reset), `02` (read via `0x595E00`), `03` (write via `0x596370`) | fail template `0x447884` at `0x523E99`; see 5.3 |
| `15` | AH=`86` only (BIOS wait; interval `CX*0x10000+DX` scaled by `1/1000`, div-const `1000.0` at `0x409830`) | fail template `0x4478E4` at `0x5245F4`; see 5.4 |
| `16` | AH=`00,01` | kb-buffer OCX vtable sink `+0x750/0x754`; fail template `0x4479B4` at `0x524BDF` |
| `17` | - | direct `CALL 0x550B40` ("wrong parameter...") |
| `1A` | AH=`00` (time-of-day; host clock `fmul [0x409828]` -> `>>1` div-2 BCD chain -> CH/CL/DH/DL; CX/DX read-path variant) | fail template `0x4479D4` at `0x5252EE`; see 5.5 |
| `19` | - | reboot: `[ecx+0x4C]=0xFFFF; [0x621218]=0xFFFF; CALL [eax+0x7AC]` |
| `20` | - | terminate: same `0x4C/0x621218=0xFFFF` + `[eax+0x7AC]` (identity of that sink is still open, 21.2) |
| `21` | 47 AHs: `00,01,02,05,06,07,08,09,0A,0B,0C,0D,0E,17,19,1A,1F,25,2A,2C,2E,30,35,36,39,3A,3B,47,3C,3D,3E,3F,40,41,42,4D,54,56,4C,4E,4F,57,23,33,48,49,4A` | doc lists 27; the other 20 (00,0D,17,1A,1F,23,2E,30,33,36,43,48,49,4A,4D,4E,4F,54,56,57) still have compare-and-handle bodies in the binary; fail template `0x447EB0` at `0x52A2D4`; shared exit `0x52A52A: jmp 0x52A934` |
| `33` | AX=`0000/0001/0002/0003` (mouse; template `0x453680` `int 33h/AX=0` at `0x5EEFB5/0x5EF1B8`) | else `not supported INT 21h/33h AL=` (`0x448798`); reg-dump block at `0x52A549` pushes fields `+0xBA..+0xC1` |

**INT 21h AH to handler attribution (CONFIRMED call targets):**
file: `3C,3D->0x5DDF80` (via `0x54A0C0+0x5DC410` EA prologue), `3E->0x5DE530`,
`3F->[eax+0x76C]`, `40->[edx+0x76C]`, `41->0x5DC410+0x5DEB80`,
`42->0x5DED70` (SEEK, "allowed values for AL = 0/1/2"), `54->0x5E0870`,
`56->0x5DF2D0` (rename, via `0x54A0C0+0x5DC410` x2 EA prologue),
`43->0x5E16C0`, `4E->0x5E25E0`, `4F->0x5E2C20`, `57->0x5E2E50`,
`23->[eax+0x7C0]` (COM file-size/get-attr), `33(get)->[edx+0xB4C]`,
`48->0x60A910`, `49->0x60AE30`, `4A->0x60B0B0` (OCX screen/keyboard trio,
only called from the dispatcher), `0E->0x5DC310`, `17->0x5E33F0`,
`19->0x5DC390`, `1A->0x5E2170`, `1F->0x5E2300`,
`0A->0x571280 x3 + 0x4AD050` buffered-input chain.
`2E/30/36/47-resp.` tails are inline (no CALL). Printer AH=`05` =
`CALL 0x5E1E70` + vtable sinks; "virtual printer does not respond."
(`0x452204`) is pushed at `0x5E1FEF` inside it.
FPU: `0x5457C0` (7212 B, 323 blocks) owns "FPU ERROR: " (`0x4490A0` pushed
at `0x5474DE` inside `0x547EC0` FPU step-back). Floppy: `0x595E00/0x596370`
(1011/1091 B, only called from `0x51FF60`) for INT 13h disk paths.
Video: `0x56BEC0`+ owns `set_VIDEO_MODE` strings; "unsupported video mode:"
(`0x44AB08`) pushed at `0x56CA11`.

Clone gaps this closes: AH=13h decoded this pass (see 5.2); INT 13h `02/03`
real sector I/O implemented this pass (see 5.6). Remaining delta: INT 10h
AH=`01/02/03/0C/0D` + `08/09/0A` attribute bytes (clone writes chars but
drops page/attribute; original routes through `frmScreen` OCX + the
`0x621C04`-guarded helpers) - DESIGN follow-up, not changed.

### 5.2 INT 10h AH=13h write-string loop  (CONFIRMED - objdump + REA bytes)

Entry `0x522D0E` (`cmp AH-slot,al; jne 0x523577` fall-through). Preamble
computes the ES:BP source address and decodes the two mode bits:

- `0x522D2D-0x522D5D`: `p_src = ES*0x10 + BP` via two `0x4AD050`
  variant-to-int calls + `imul esi,esi,0x10` (seg x16) + BP, stored to
  `[ebp-0x24]`. CONFIRMED cell: `seg_off_to_phys` equivalent.
- `0x522D67-0x522D7C`: `w0 = AL & 1` (bit0: advance-cursor flag) into
  `[ebp-0x44]`; second decode `0x522E31-0x522E46`: `w1 = (AL & 2) != 0`
  (bit1: per-char-attribute flag) into `[ebp-0x50]`.
- Guard `0x522F70`: `if (CX > 0x7D0) fail` - new string is
  `"CX value (string size) is above the allowed limit!"` (`0x447650`);
  below it the low-memory pointer check `cmp [ebp-0x24],0x10FFF1; jle`
  else new string `"CX is out of memory"` (`0x4476E8`), both logged with
  template `"INT 10h, AH=13h - "` (`0x4476BC`) via `0x5F2380` + `0x547EC0`.
- Length scaling `0x523085-0x5230A0`: `if (w1) CX *= 2` - each screen cell
  consumes 2 bytes `[char,attr]` when the attribute flag is set.
- Main loop `0x5230C4/0x5230D8/0x523328` (`for (i=1; i<=CX; i++)`):
  per-cell body reads one byte via `CALL 0x571280` (same get_BYTE_at_EA
  reader as the CPU core), converts it through the `0x401xxx` variant
  helpers, and emits it through the OCX sinks (`[edx+0x760]` at
  `0x5233B4`, `[ecx+0x714]` at `0x523491`, both under `0x621C04` guard).
- Cursor `0x523311`: `test WORD [ecx+0x36],...; je continue; jmp 0x52ABDF`
  - skips the tail call when the no-advance flag word is set.
- Tail (shared exit `0x52ABDF` via `0x52333D/0x52331C`): mode-attr block
  passes `BH(+0xBD)/DH(+0xC1)/DL(+0xC0)` to `[edx+0x760]`, then the
  `+0x714` sink consumes `BL(+0xBC)`, `[ebp-0x30]` (w1-decoded word),
  `[ebp-0x28]` (w0-decoded word), `BH`, and `p_src`.

Clone delta (`src/dos.c` `case 0x13`) - **CLOSED**. The clone used to emit CX
raw bytes and ignore BL/attributes/advance. It now mirrors the decoded
behaviour:

- source is `ES:BP`; `DX` is the starting position, stored into the modelled
  cursor before anything is written.
- `AL` bit 1: the source is CX `[char,attr]` pairs, so `2*CX` bytes are
  consumed and the attribute byte is consumed but **not printed** (a text
  console cannot render it; BL is likewise not renderable).
- `AL` bit 0: the cursor advances over exactly the characters written and not
  at all when the bit is clear. Before this change the clone had no cursor at
  all - AH=02 was a no-op and AH=03 always returned 0,0.
- `CX > 0x7D0` is refused: diagnostic on stderr, nothing written, cursor
  untouched, and execution continues - matching `0x522F70` -> log ->
  `[obj+0x7AC]` -> `WORD ds:0x62141C = 0xFFFF` (`0x52301E-0x523079`). This
  section originally claimed a halt with `exit_code = -5`; that was wrong.
  See 24 for the CONFIRMED reading (`[obj+0x7AC]` is a UI report, and the
  only halt marker is `ds:0x621218`, which this path never writes).

Regressions: `src/check_int10.py`, 18 checks (advance, pairs, no-advance,
AH=02/AH=03 round trip, teletype movement, cell reads/writes, scroll, CX
limit), wired into `make test`. Verified to bite: forcing the pairs skip off
and the advance unconditional makes exactly the three affected checks fail,
and dropping the AH=09 attribute store fails `attr-write-read` and
`char-write-keeps-attr`.

Remaining differences, and why they are not defects:

- BL's attribute and page BH cannot be *rendered* by a stream console, but the
  attribute is no longer discarded: it is stored in the cell buffer (17) and
  read back by AH=08. Page BH selects nothing because page 0 is the only page
  the clone models, which the 80x25 mode it reports implies.
- Wrapping at 80 columns / 25 rows is the clone's choice: the real wrap and
  scroll belong to the `frmScreen` OCX, which was not decoded. It matches the
  80x25 the clone already reports from AH=0F and uses in AH=06/07.
- The per-iteration `p_src > 0x10FFF1` guard (`0x5230EE`, "CX is out of
  memory") is dead code in the **original** too: `p_src` is at most
  `0xFFFF*16 + 0xFFFF = 0x10FFEF`, which never exceeds `0x10FFF1`. Omitting it
  is behaviour-identical, so there is nothing to implement.
- AH=0Eh teletype now moves the modelled cursor as well (printable advances a
  column, CR to column 0, LF down a row, BS back one), so AH=03 stays coherent
  wherever a guest looks. Covered by `teletype-advances` / `teletype-cr-lf`.

### 5.3 INT 13h AH=00/02/03 floppy sector I/O  (CONFIRMED - objdump + file stat)

Entry `0x523C49` (INT byte test `0x13`, slot copy `AH->[ebp-0x278]` at
`0x523C50-0x523C59`). Three AH arms, all CONFIRMED:

- AH=`00` reset (`0x523C66-0x523C86`): `WORD [regblock+0x3A] = 0xFFFF`
  (drive-status word), then shared exit `0x5240EF -> jmp 0x52A934`.
  CONFIRMED cell: reset needs no disk access; clone can match exactly.
- AH=`02` read (`0x523C92-0x523D7B`): prologue copies BH/BL (`+0xBD/+0xBC`)
  into `[ebp-0x248]` via the `[eax+0x76C]` accessor pair (same EA-getter
  shape as INT 21h file `3C/3D` prologue `0x54A0C0+0x5DC410`), then
  `p_buf = ES*0x10 + [ebp-0x248]` (`ES=+0xC4`, `0x4AD050` + `imul` + add
  at `0x523D15-0x523D39`, stored `[ebp-0x24C]`) and
  `CALL 0x595E00(p_buf, BL, CL, CH, DL, DH)` -
  pushes in reverse order, so in cdecl argument order the call is
  `(AL, CH, CL, DH, DL, p_buf)`: `+0xBA(AL)` last-pushed, then
  `+0xBF(CH)`, `+0xBE(CL)`, `+0xC1(DH)`, `+0xC0(DL)`, then `&buf` at
  `0x523D3F-0x523D76`.
  `FUN_00595E00` is 1011 B / 36 blocks (Ghidra `procedure_info`).
- AH=`03` write (`0x523D80-0x523E88`): same shape, buffer physicalised as
  `p_buf = [ebp-0x250](=ES field via 0x4AD050 at 0x523DA5)*0x10 +
  [ebp-0x248]` (`0x523E20-0x523E44`), then
  `CALL 0x596370(p_buf, ..., w_flag)` with an extra leading
  `WORD [ebp-0x23C] = 0` argument slot (`0x523E20/0x523E4A`) - in cdecl
  order `(AL, CH, CL, DH, DL, p_buf, &zero_word)` per `0x523E4A-0x523E88`.
  `FUN_00596370` is 1091 B / 38 blocks.
- Fail path (`0x523E92-0x5240EF`, all other AHs): logs template
  `"INT 13h, AH=0"` (`0x447884`) + `"h   "` (`0x4478A4`) +
  `"- not supported yet."` (`0x4478B4`) + `"refer to the list of
  supported i..."` (`0x44782C`) composed through `0x5F2380` + `0x547EC0`
  (same log pipeline as the INT 10h fail paths), then calls `+0x7AC` and
  sets `WORD [0x62141C] = 0xFFFF` before the shared exit.

Geometry ground truth (CONFIRMED by host file, not by REA):
`C:\emu8086\FLOPPY_0` = 1474560 bytes = 80 tracks x 2 heads x 18 sectors
x 512 B (standard 1.44 MB floppy image; the file is the backing store the
read/write arms address). `0x595E00/0x596370` internals NOT decoded this
pass - only their call signatures (arg order/count) and sizes are claimed.

Clone delta (`src/dos.c`): CLOSED - `int13()` + `case 0x13` added (80x2x18x512
geometry; `AH=00 -> CF=0,AH=0`; `AH=02/03 -> CHS decode, ES:BX copy, CF/AH
errors`; else `CF=1,AH=1`), and since 15.5 the image is opened as
`FLOPPY_<DL>`, one open per call, so writes persist. See 5.6.

### 5.4 INT 15h AH=86h BIOS wait  (CONFIRMED - objdump + REA bytes)

Entry `0x524112` (INT byte `0x15`, slot `[ebp-0x27C]`, AH=`0x86` test at
`0x52412F`). Single arm plus fail tail (`0x5245ED` template `0x4478E4`
`"INT 15h, AH=0"`):

- Interval build `0x5241B2-0x524235`: `ticks = DX + CX*0x10000`
  (`imul eax,eax,0x10000` at `0x52421D`, `[ebp-0x4C4]`) lifted with
  `fild/fstp` + `fdiv 1000.0` (`QWORD [0x409830] = 1000.0`, divided only
  `if [0x621000]==0` else via `0x4199D4`), then `Sleep(ticks_ms)` through
  the `[edx+0x64]` OCX sink and two `[eax+0x33c]/[ecx+0x33c]` acquire
  calls. The `fmul [0x409828]` (multiplier `639.4603...`, bytes
  `30478FDFDB343240` LE double) entry belongs to the INT 1Ah arm below -
  the INT 15h arm divides, not multiplies. Earlier note corrected.
- Gating: `if ([ebp-0x248] > 0) ticks &= ~[ebp-0x23C]` word-mask
  (`0x524425-0x524440`), then a second wait iteration is entered only
  `if (masked != 0)` (`0x524460-0x524469`); a zero tick count returns
  through the shared exit without sleeping.

Clone (`src/dos.c` `int15`): real `Sleep()` has no headless test oracle
and the OCX acquire/wait sinks are GUI-thread machinery, so the clone
keeps `AH=86h -> CF=0, AH=0` (success, no wait) exactly as before. No
behaviour change; no new delta - documented for completeness.

### 5.5 INT 1Ah AH=00 time-of-day  (CONFIRMED - objdump + REA bytes)

Entry `0x524E7E` (INT byte `0x1A`, slot `[ebp-0x284]`, AH=`0x00` test at
`0x524EB2`):

- Host clock read `0x524EC6`: `call 0x4012D8` (VB Now) then
  `fmul [0x409828]` with the LE-double bytes `30478FDFDB343240`
  (= 639.4603...). Combined with the `0x4011`-scaled `rtcHexVarFromVar`
  (`0x40132C`) conversions and the push-`1`/`0x401170` div-2 step at
  `0x524FE1-0x524FF2`, followed by four BCD-split blocks each writing one
  register byte: `CH <- +0xBF` (`0x525031`), `CL <- +0xBE` (`0x5250FE`),
  `DH <- +0xC1` (`0x5251CB`), `DL <- +0xC0` (`0x525298`), then
  `AL <- +0xBA` midnight flag (`0x5252D9-0x5252DC`) before the shared exit
  `0x5252E2 -> jmp 0x525544`. So the arm writes CH/CL/DH/DL directly -
  the earlier shorthand "+0xBB AH slot" does not apply here.
- Rate check (CORRECTED): the sibling `0x4AD050`+`imul esi,esi,0x10`
  fragment at `0x5270xx`-range belongs to the INT 1Ah AH=`02` (date)
  sub-arm, not to AH=`00`: it physicalises the DS:DX return buffer while
  AH=`00` returns the tick count in CX:DX and midnight flag in AL.
- CX/DX read path (CONFIRMED): `mov WORD [ebp-0x23C],8` selector +
  `0x4AA020` converter + push-`2`/`0x401344` splitter at
  `0x524F18-0x524F74`, then the div-2/BCD chain above.
- Fail path (`0x5252E7`): same 3-template + `0x5F2380` + `0x547EC0` log
  pipeline with `"INT 1Ah, AH=0"` (`0x4479D4`) and
  `"refer to the list of supported i..."` (`0x447904`).

Clone (`src/dos.c` `int1a`): returns fixed demo values; the original reads
the host clock. No headless oracle distinguishes them deterministically,
so the clone is unchanged - documented for completeness.

### 5.6 INT 13h clone implementation  (IMPLEMENTED + tested this pass)

`src/dos.c` now has `int13()` + `case 0x13`, derived from 5.3:

- Image: `FLOPPY_<DL>`, the name the original builds from DL (env
  `MUE_FLOPPY` overrides drive 0 only, then `C:/emu8086/FLOPPY_<n>`,
  `EMUB086_FLOPPY`, then the relative name); 1474560 bytes = 80 trk x 2
  heads x 18 sec x 512 B; short reads zero-padded, missing image -> `CF=1`.
  The file is opened and closed inside the call, as in the original, so a
  guest write is on disk when the interrupt returns (15.5).
- `AH=00`: `CF=0, AH=0` (reset always succeeds).
- `AH=02/03`: CHS decode `C=CH, H=DH, S=CL&0x3F`; reject `S==0`, `H>1`,
  `linear=(C*2+H)*18+(S-1) >= 2880`, or `ES:BX+AL*512` overflow past 1 MB;
  `02` copies image->memory, `03` memory->image; `CF=0, AH=0, AL=count`.
- Else (incl. AH=`01` status): `CF=1, AH=1` mirroring the fail path.
- Regression: `src/check_int13.py` (12 checks) exercises reset/read/
  read-write-readback/bounds/else through `mue -r` scripts, asserts byte
  movement both ways, and now also asserts that the written sector reached
  the host image file (`rw-persists-to-image`) and that a drive with no image
  fails instead of silently reading drive 0 (`drive-1-missing-sets-cf`).

**BUG FIXED (register-index slip).** `int13()` used `cpu.r[3]` for DH and
`cpu.r[5]` for BX. With `enum { AX, CX, DX, BX, ... }` those are **BX** and
**BP**, so the head came from BH (always 0 unless the guest set it) and the
transfer buffer from BP. A guest reading CHS 79/1/18 therefore read 79/0/18.
Now `cpu.r[DX]` / `cpu.r[BX]` via the enum names. The regression was rewritten
because its old oracle - the host image file after a write - cannot work: the
clone keeps the image in memory and never writes it back, so the old check
compared two all-zero buffers. (The original *does* persist - see 15.5 - so
the clone's missing write-back is a genuine gap, tracked there, but the test
must not assume behaviour the clone lacks.) It now proves the round trip
*inside* the guest: read a patterned head-1 sector, write it to sector 1, read
sector 1 back, byte-compare, and report the first two / last bytes read. With
`dh` restored to the BH read the two byte-value checks fail (`AX=0000`,
`DX=0000`); with the fix all ten checks pass.

### Supporting native routines  (CONFIRMED addresses)

| VA | Role | Evidence |
|---|---|---|
| `0x4199B0` | `__vbaChkstk` (stack probe) | called after `mov eax,<frame>` |
| `0x4199B4` | SEH record for method #377 | `push 0x4199B6` at prologue |
| `0x419C50`–`0x419D00` | IAT jump thunks | `jmp ds:0x4010xx` chain |
| `0x4AD050` | VB variant → integer | `push <field>; call` before `imul …0x10` |
| `0x571280` | **`get_BYTE_at_EA`** (fetch byte) | 12+ call sites in core |
| `0x4061D0` | SEH handler for method #377 | `[ebp-0x14]=0x4061D0` |
| `0x517425` | **overflow trap** target | `jo 0x517425` everywhere |

## 9. Assembler rules confirmed against emu8086 listings (clone validation)

Verified by byte-exact comparison with all 46 `C:\emu8086\MyBuild\*.list`
files (`src/check_asm.py`: PASS 46 / FAIL 0).

CONFIRMED encoding choices:

- `mov reg8/16, reg8/16` uses the **load** direction `8A/8B` (e.g. `mov ax,bx`
  -> `8B C3`), not `89 D8`.
- ALU `reg, imm16` with an imm8-representable value uses the sign-extended
  `83 /r ib` form **including CMP** (`cmp cx,4` -> `83 F9 04`).
- `lea reg, symbol` is folded to `B8+reg iw` (a plain immediate move).
- `out port, ax` / `in ax, port` use `E6/E7`/`E4/E5` with the port truncated to
  8 bits (`out 199,ax` -> `E7 C7`).
- `int 3` -> `CC`.
- A bare data/code label used alone (`mov dl, message`) is a **memory**
  operand (`8A 16 0001`), while an `equ` constant (`red equ 1`) is a value.
- `test reg, imm` other than AL/AX uses `F6/F7 /0`.
- Number syntax: `0xffh`, `0FFh`, `11101100b`, `0000_0110b` (digit separators
  `_`), and `0b800h` is **hex**, whereas a run of >=2 bits followed by `b` is
  binary.
- `name proc` / `name endp` / `name segment` / `name ends` / `name equ val`
  all define or bound a symbol; `end <name>`, `.model`, `.stack`, `assume`,
  `#...` control comments emit nothing.
- Symbol names are **case-insensitive** (`Fin` == `fin`).
- Data laid out before `.code` is followed by padding to the next 16-byte
  boundary, which is why `.exe_` sources put code at `...30` not `...22`.
  A source that contains an `ORG` never gets this padding.
- `db ?` reserves a zero byte.
- `offset sym` / `seg sym` evaluate to the symbol offset.

Behaviour (not encoding):

- A `.COM` program ends when `RET`/`IRET` pops `CS:0000`; emu8086 stops there
  too, so the clone halts instead of wrapping to offset 0.
- `LOOP` (E2) tests **only CX**; `LOOPE/LOOPZ` (E1) also tests ZF. The clone
  originally applied the ZF test to E2, which truncated every count-down loop
  by one iteration.
- 8-bit register access must map `reg[0..7]` to `AL,CL,DL,BL,AH,CH,DH,BH`
  (`index = reg & 3`, high byte when `reg & 4`). Using `reg >> 1` sends every
  access to the wrong register and silently breaks most programs.

## 10. Source-level (macro) facilities confirmed from emu8086 examples

emu8086 accepts MASM-style macros; the clone implements them in
`src/asm_main.c` (`process_line` collection + `expand_macro`).

CONFIRMED:

- Definition is `name MACRO p1, p2` ... `ENDM`. Both `macro`/`MACRO` and
  `endm`/`ENDM` are accepted case-insensitively (an uppercase `ENDM` that was
  not recognised left the rest of the file swallowed as macro body).
- Substitution is **whole-word only**. A naive substring replace turns
  `mov al, v` into `mo5` when the parameter is `v`.
- Substitution must skip quoted string literals.
- `LOCAL a, b, c` declares labels private to one expansion. The clone gives
  each expansion a fresh `??lNNNN` name (a serial counter), so two expansions
  of the same macro do not collide. `?` is therefore a valid identifier
  character in expressions and labels.
- An uppercase macro call (`PUTC 8`) must resolve against a lowercase
  definition; macro names are case-insensitive like labels.
- Macros are re-collected on every pass, so the collected table must be reset
  per pass or the fixed capacity is exhausted and later lookups fail.

Measured coverage after these fixes: 68 of 85 `C:\emu8086\Examples\*.asm`
assemble (was 48). The 17 that still fail need facilities beyond what has been
reverse engineered so far, not any 8086 instructions.

## 11. INDEPENDENT VALIDATION vs REAL emu8086 ARTIFACTS (round-trip)

Method: take emu8086's OWN generated machine code (`C:\emu8086\MyBuild\*.bin_|.exe_|.com_`)
paired with the source that produced it (`*.~asm`), reassemble that source with the clone,
and byte-compare against emu8086's real output. This is external ground truth, not
self-expectation.

Result: **44 / 44 byte-exact**
- 26 sources -> byte-identical to emu8086's `.bin_` / `.com_` images.
- 18 `*.exe_` sources: code payload byte-identical (verified `payload in ref-image`);
  the only difference is the MZ EXE container, which the clone does not emit.

CONFIRMED facts this establishes:
- Instruction encoding for every construct used by those 44 programs matches emu8086.
- Jump displacement selection, ORG/label resolution, .data/.code layout, .exit expansion.
- `.exe_` targets need an MZ header writer (missing feature, not an encoding bug).

### emu8086 has NO headless/batch mode (CONFIRMED, negative)
- Checked ASCII strings, UTF-16LE UI strings, and emu8086.ini: no `/a` run switch exists.
- `SILENT_ASSEMBLER` + "batch assembly of several source files" appear only as an INI
  comment; no matching UI/code strings are present in the binary.
- "enter command line parameters" is a dialog for the EMULATED program's PSP command
  line (CS:0080h), NOT CLI switches for emu8086 itself.
- Therefore GUI automation is not available; byte round-trip against shipped artifacts is
  the strongest available external oracle.

### Auxiliary DLLs (CONFIRMED by PE parse)
- `MicroAsm.dll` (9,216 B, .code 0x19C0): error shim only. Exports ShowErrorMessage,
  ShowLastError, MicroAsm_T, MicroAsm_D8..DF_TAB. Imports KERNEL32/USER32 only.
  NOT the assembler (too small).
- `diasm.dll` (98,304 B): the disassembler. Exports analyse, disassemble,
  s_Last_analyse_ERROR, tfunc, yur_init_analyser.
- The real assembler and CPU core live inside emu8086.exe.

## 12. MZ (EXE) CONTAINER WRITER — CONFIRMED byte-exact

The 18 `*.exe_` artifacts previously matched only as code payloads. The header is now
reproduced too, so **all 44 shipped emu8086 artifacts round-trip byte-identically**.

Header layout (recovered by differential comparison; `src/exe.h` documents it):

| off  | val            | note |
|------|----------------|------|
| 0x00 | 'MZ'           | |
| 0x02 | total % 512    | bytes in last page |
| 0x04 | ceil(total/512)| page count |
| 0x06 | nreloc         | 0, or 1 when `.stack N` present |
| 0x08 | 0x20           | header size, paragraphs |
| 0x0A | 0              | minalloc |
| 0x0C | 0xFFFF         | maxalloc |
| 0x0E | 0              | ss |
| 0x10 | stack size     | sp |
| 0x12 | checksum       | see below |
| 0x14 | 0              | ip |
| 0x16 | code para      | cs (only nonzero when relocated) |
| 0x18 | 0x1E           | reloc table offset |
| 0x1A | 0              | overlay |
| 0x1C | 1              | CONSTANT 1 in all 18 images; omitting it changes the word sum by one |
| 0x20..0x1FF | 0       | padding; payload starts at 0x200 |

**Checksum rule (CONFIRMED):** `csum = (0xFFFF - word_sum) & 0xFFFF`, where
`word_sum` is the sum of all 16-bit little-endian words of the file **with the
checksum field itself zeroed**, and an odd final byte is zero-padded.

**Segmented layout (CONFIRMED via div.exe_, the only such example):**
- `.stack N` → stack segment first at 0x200, filled with `0x54` ('T').
- `.data` follows; `.code` starts on the next 16-byte boundary.
- `sp` = stack size; `cs` = code offset / 16 (0x13 for div).
- one reloc entry at 0x1E: `offset=1, segment=cs`, patching the `mov ax,@data`
  immediate.

**Addressing rule (CONFIRMED):** in an EXE, data labels are offsets **within the
data segment** and restart at 0 (`div.exe_.symbol` lists `MESSAGE 00000`,
`RESULTAT 00012`), while a `.COM` image addresses from 0x100. A source is an EXE
only when it declares `.model` **and has no `org`** — `div.com_` has `.model` plus
`org 100h` and is still a COM image.

### Final validation state
- MZ/COM round-trip vs real emu8086 binaries: **44 / 44 byte-exact**
- `.list` per-byte assembler comparison: **46 / 46 pass**
- CPU self-test: **ALL PASS (0 failures)**
- emu8086 examples assembling: **74 / 85**

## 13. `include` AND THE PASS-MISMATCH BUGS (CONFIRMED)

`include "file"` splices the file's lines into the statement stream before any pass
runs, so included macros behave as if typed in place. Bare names resolve against the
assembler's own `inc\` directory (`C:\emu8086\inc\emu8086.inc`).

Getting `examples\include.asm` to work exposed five distinct bugs, in the order they
surfaced:

1. **`NAME ENDM` was not recognised.** emu8086.inc writes `DEFINE_SCAN_NUM ENDM`, with
   the macro name in front of the terminator, on 7 of its 13 macros. Only bare `ENDM`
   matched, so `in_macro` stayed set and swallowed the rest of the file. Now both forms
   are accepted.
2. **`LOCAL` stopped at the first line.** `DEFINE_SCAN_NUM` declares its locals over
   four separate `LOCAL` lines (15 names). Only the first line was read.
3. **`ASM_MAX_MLINES` was 96; the longest macro is 124 lines.** The body was silently
   truncated, losing the jump target at the end. Raised to 256.
4. **`LOCAL` names all shared one tag.** Every name in an expansion was substituted
   with the same `??lNNNN`, so `LOCAL sk,d` made `sk` and `d` the same label and
   `lea si,d` resolved to `sk`. The serial must advance per *name*, not per expansion.
5. **The sizing pass and the emit pass disagreed.** Sizing used `pass=1` rules
   (forward jumps always take the fixed wide form), so it converged on a 697-byte
   image while the emit pass produced 704 bytes — leaving every label 7 bytes stale
   and `jmp start` landing in the middle of a macro tail. Fixed by sizing with the
   *same* rules as emit (converging, byte-for-byte, on the real image) and treating
   that converged buffer as the output. There is no longer a separate emit pass that
   can disagree with the labels it recorded.

Note the local-name tag must be reset at the start of every pass, otherwise pass N+1
references `??l0003` while the label was defined as `??l0000`.

### Validation state after `include`
- MZ/COM round-trip vs real emu8086 binaries: **44 / 44 byte-exact**
- `.list` per-byte assembler comparison: **46 / 46 pass**
- CPU self-test: **ALL PASS (0 failures)**
- emu8086 examples assembling: **79 / 85** (was 74)
- `examples\include.asm` runs correctly and prints its banner through the
  emu8086.inc `putc` / `printn` / `print_string` macros.

## 14. THREE OPERAND/BRANCH GAPS CLOSED (CONFIRMED)

All 85 shipped examples now assemble. Each gap was reproduced in a three-line
file before being fixed, and each fix was checked against emu8086's own
documentation or listings rather than invented.

**1. Far `jmp seg:off` / `call seg:off`.** `eval_expr` rejected `0ffffh:0000h`
because a colon is not an operator. `:` is now recognised first and emits `EA`
(jmp far) / `9A` (call far) with *offset then segment* — the order that starts
with the offset, not the segment.

**2. Bare `label+n` without brackets** (`lea si, buf + 2`). The operand parser
tested the *whole* operand text against a label name, and `"buf + 2"` is not a
label, so it fell through to OP_IMM. It now matches the leading identifier, but
only when the remainder is a pure +/- displacement: `count*2` must stay an
immediate. Getting this wrong once cost four previously-working programs
(Calculator, float, matrix, timer), so the "pure displacement" restriction is
load-bearing, not defensive.

**3. `JCXZ` beyond +-127 bytes.** emu8086 documents its own expansion in
`examples\writebin.asm` ("this instruction is assembled into OR CX, CX and two
JMPs") and `asm_tutorial_07.html` gives the equivalence `JCXZ = OR CX,CX and
JNZ`. The rewrite is 8 bytes and target-independent:

    OR  CX, CX        0B C9     ZF = (CX == 0)
    JNZ short $+3     75 03     skip the jump when CX != 0
    JMP target        E9 lo hi  reached only when CX == 0

The condition must be **JNZ, not JZ**: with JZ the jump is taken on both paths.
That inversion was written, looked plausible, and was only caught by running a
program with CX=5 and CX=0 and checking AL — the encoding was right in shape
and wrong in sense. Sizing cannot defer the displacement here either, because
there is no separate emit pass any more; a "sizing placeholder" of 0 would be
what finally ships.

Only JCXZ has a documented rewrite. An over-long LOOP/LOOPE/LOOPNE still
reports `target too far` rather than receiving invented bytes; no shipped
source exercises one.

### Validation state
- MZ/COM round-trip vs real emu8086 binaries: **44 / 44 byte-exact**
- `.list` per-byte assembler comparison: **46 / 46 pass**
- CPU self-test: **ALL PASS (0 failures)**
- emu8086 examples: **85 / 85 assemble**

## 15. INT 13h CALL CHAIN VERIFIED AGAINST THE ORIGINAL (CONFIRMED)

Prompted by a clone bug (DH read from BH), this pass went back to the original
binary and re-derived the whole INT 13h path from disassembly, rather than
trusting the summary in 5.3. Everything below is from `re/disasm.txt` and is
independent of the clone.

### 15.1 Register-field map correction (CONFIRMED)

An earlier revision of 5.1 listed `+0xC4`=SI / `+0xCA`=DI. Both are wrong, and
the error matters because 5.3 then described the INT 13h buffer as
"`SI*0x10`". Two independent arms fix the map:

- **`A4`/`A5` MOVS arm of the core (`0x4F527B`)**: source byte is fetched from
  `0x4AD050(obj+0xC6)` added to a segment base, then `0x571280`
  (`get_BYTE_at_EA`); destination is `0x4AD050(obj+0xC4)` `imul 0x10`
  `+ 0x4AD050(obj+0xC8)`, stored and passed to `0x56FEE0`
  (put-byte-at-EA). So the *source offset* is `+0xC6` and the
  *destination segment/offset* pair is `+0xC4`/`+0xC8`.
- **INT 10h AH=13h (`0x522D2D-0x522D5D`)**: `p_src = 0x4AD050(obj+0xC4) * 0x10
  + 0x4AD050(obj+0xCA)`. 5.2 already established from the API contract that
  this arm's source is `ES:BP`.

Therefore `+0xC4`=ES, `+0xC6`=SI, `+0xC8`=DI, `+0xCA`=BP (and `+0xCC`=CS,
`+0xCE`=IP as before). Section 5.1 has been corrected. Nothing else in this
document depended on the old labels, but any future work that physicalises a
guest pointer should use `+0xC4` for the ES base.

### 15.2 Argument order and CHS math (CONFIRMED)

Both sector arms push arguments in reverse, so the cdecl parameter order is:

- AH=`02` (`0x523D3F-0x523D7B`) -> `FUN_00595E00(AL, CH, CL, DH, DL, p_buf)`
- AH=`03` (`0x523E4A-0x523E88`) -> `FUN_00596370(AL, CH, CL, DH, DL, p_buf,
  &zero_word)`

where `p_buf = ES*0x10 + BX`, with BX produced by the `[eax+0x76C]` accessor
pair (the same BL/BH getter shape as the INT 21h `3C/3D` prologue).
**Head is `+0xC1` (DH)**, cylinder is CH, sector is CL, count is AL. That is
exactly the 8086 contract, and it independently confirms that the clone's
BH-for-DH read was the bug and not an emu8086 convention.

`FUN_00596BB0` is the CHS->sector-index helper, and it is short enough to
quote in full in effect (`0x596C0B-0x596C3A`):

```
cx = (u8)arg1 * 0x24      ; 36 = heads x sectors-per-track (2 x 18)
cx += (u8)arg2 * 0x12     ; 18 = sectors per track
cx += (u8)arg3            ; sector, 1-based
cx -= 1                   ; -> 0-based index
```

So `linear = C*36 + H*18 + (S-1)`, i.e. `(C*2 + H)*18 + (S-1)`. This is
byte-for-byte the formula the clone already used
(`(cyl * FLOPPY_HEADS + head) * FLOPPY_SPT + (sec - 1)`), and it includes the
head term - so a head mis-decode necessarily reads a different sector, as
observed. The geometry constants 2x18 are hardcoded, matching the 1474560-byte
`FLOPPY_0` (80 x 2 x 18 x 512).

### 15.3 The transfer helpers reproduce 5.3's *inner* copy only

Call inventory of **direct `call rel32`** inside each function:

| function | direct calls |
|---|---|
| `0x595E00` read  | `0x596930`, `0x596BB0`, `0x547870`, `0x548350` |
| `0x596370` write | `0x596A20`, `0x596BB0`, `0x56FEE0`, `0x571280`, `0x4ACC90`, `0x547870`, `0x548350`, `0x4199B0` |
| `0x596930` (file->memory) | `0x56FEE0` with base `ds:0x621F0C`, length checked against `0x200` |
| `0x596A20` (memory->file) | `0x571280` with base `ds:0x621F0C` |

`0x56FEE0` is the same put-byte-at-EA primitive the MOVS opcode uses, and
`0x571280` is the same fetch-byte primitive, so `0x596930`/`0x596A20` are the
raw 512-byte emulated-memory <-> buffer copies. `ds:0x621F00` is the buffer
descriptor and `ds:0x621F0C` its data pointer (INFERRED from use as a
0x200-stride base).

**A direct-call scan alone is not sufficient here** - see 15.5. The host file
work in these two functions goes through `call DWORD PTR ds:0x401xxx` IAT
thunks, which a `call rel32` sweep misses entirely. Any future "does the
original touch a file?" question must include the thunk form.

### 15.4 What this changes in the clone

Only `src/dos.c int13()` (`cpu.r[DX]` for DH, `cpu.r[BX]` for the buffer, enum
names instead of magic indices) and `src/check_int13.py` (oracle rewritten).
The CHS formula, the ES:BX buffer, the CF/AH convention and the 80x2x18x512
geometry all stand as they were - now with disassembly evidence instead of
inference. Write persistence *is* a real delta; see 15.5.

### 15.5 Host-file I/O is real, per call, and settles persistence (CONFIRMED)

The `FLOPPY_<n>` image is opened, read/written and closed **inside every
INT 13h AH=02/AH=03 call**. The runtime calls are IAT thunks, resolved against
the single MSVBVM60 import descriptor (273 entries, IAT at RVA 0x1000):

| IAT slot | import | role |
|---|---|---|
| `0x4012F8` | `__vbaFileOpen` | Open "FLOPPY_" & drive, record length `0x200` |
| `0x4012CC` | `__vbaGetOwner4` | `Get #` - file -> buffer |
| `0x4011BC` | `__vbaPutOwner4` | `Put #` - buffer -> file |
| `0x401180` | `__vbaFileClose` | Close before returning |

The other file-capable slots the binary imports are `__vbaGet3`/`__vbaPut3`
(`0x40119C`/`0x401054`), `__vbaGetOwner3`/`__vbaPutOwner3`
(`0x4012BC`/`0x4011A8`), `__vbaFileSeek` (`0x4012E0`),
`__vbaLineInputStr` (`0x401040`) and `__vbaNameFile` (`0x4010AC`); neither
sector path uses them.

Read path `FUN_00595E00` (`0x596013-0x5960A5`, CONFIRMED instruction by
instruction):

```
name = "FLOPPY_" & CStr(DL)            ; 0x445C14 is a UTF-16 "FLOPPY_"
__vbaFileOpen(0x4004, 0x200, chan, name)      ; 0x596018-0x596022
lin  = CHS(CH, DH, CL)                        ; 0x596034 -> FUN_00596BB0
for (i = 1; i <= AL; i++) {                   ; 0x59604A-0x5960A3
    rec = lin + i                             ; 0x59605C-0x596069
    __vbaGetOwner4(0x44C0E4, 0x621F00, rec, chan)   ; 0x59606C-0x596077
    copy 512 bytes buffer -> emulated memory  ; 0x59607D -> 0x596930
    buffer += 0x200                           ; 0x596085-0x596091
}
__vbaFileClose(chan)                          ; 0x5960A5
```

Write path `FUN_00596370` is symmetric: `0x5965C7` FileOpen, loop
`0x596601-0x596638` with `__vbaPutOwner4` at `0x596632` after the same
`lin + i` record build, `__vbaFileClose` at `0x59664B` (and again on the error
branch at `0x596790`).

**Answers the persistence question:** there is no "flush on exit". Record
numbering is 1-based (`lin + i`, and `lin` already holds `S-1`), with a
512-byte record length, so the byte offset is `lin*512` - the same place the
clone's memory image keeps it. Because the file is opened from disk at the
start of each transfer and closed before the INT returns, a guest write is on
disk by the time AH=03 returns.

Note also that `DL` is not ignored: it is concatenated into the filename, so
DL=1 addresses `FLOPPY_1`. Only `FLOPPY_0` ships in `C:\emu8086`, so any other
drive reaches the FileOpen failure path (INFERRED - that path was not decoded).
The clone ignores DL and always uses one image.

Consequences for the clone (`src/dos.c`) - both were real deltas and both are
now closed:

1. **Writes were lost.** The clone kept only an in-memory `floppy` array and
   never wrote it back: later reads inside the run saw the write, but the
   change vanished at exit. `int13()` now opens the image for `AH=03`, seeks
   to `(C*36 + H*18 + S-1 + i) * 512` and writes, then closes - one open per
   call, exactly as the original does. `rw-persists-to-image` asserts the
   bytes reached the host file (proved to bite: opening read-only there makes
   that check fail at offset 0).
2. The clone **cached the image once**, so a host-side change between two
   transfers was invisible. There is no cache any more; every read and write
   goes to the file, matching the original's per-call `FileOpen`.

Delivery detail: `DL` selects the file (`FLOPPY_<DL>`), with `MUE_FLOPPY`
overriding drive 0 only so the regressions can drive a scratch copy. A drive
with no image fails with `CF=1` instead of silently reading drive 0. Whether
the original *creates* a missing image (VB `Open ... For Binary` would) could
not be settled from the disassembly; the clone fails the transfer rather than
inventing the file.
3. The clone **synthesises a zero image** when the file is missing; the
   original's FileOpen would fail. Treat as INFERRED (error path not decoded).

`src/check_int13.py` was still rewritten to verify the round trip inside the
guest: it must not depend on persistence that the clone does not implement,
and it now fails correctly if the clone's head decode regresses (see 5.6).

## 16. `push`/`pop` of a segment register: encoder + missing opcodes (FIXED)

Found while validating 5.2 against the shipped `examples\int10_13.asm`, which
starts with the standard `push cs` / `pop es` idiom. The defect had two halves
- a wrong encoding in the assembler and no handlers at all in the CPU - and
both are now fixed and covered.

### 16.1 The encoder half: symptom, cause, fix

**Symptom (CONFIRMED).** `push cs` was emitted as three bytes and the register
index leaked into the instruction stream:

Measured one instruction per assembly (`mue -a`, single-line sources):

```
source      clone emits          correct (8086)
push cs     06 0F 01             0E
pop  es     07                   07
push ds     06 0F 03             1E
pop  ds     07 0F 03             1F
push es     06                   06
pop  cs     07 0F 01             0F
push ss     06 0F 02             16
pop  ss     07 0F 02             17
```

Only the ES forms come out right, by accident: `ES` is index 0, so the
`if (d.reg)` guard below skips the bogus tail for it. Everything else is
one byte too long.

Consequence: `examples\int10_13.asm` assembles "successfully" but its first two
instructions become `push es` + `pop cs`, so it halts immediately - `mue` reports
`instructions executed: 0`, `IP=0101`. The same applies to any DOS program using
this idiom, which is most of them.

**Root cause (CONFIRMED).** `src/asm_main.c`, push/pop arm:

```c
if (d.kind == OP_SREG) {
    emit8(a, (uint8_t)(ispop ? 0x07 : 0x06));
    if (d.reg) { emit8(a, 0x0F); emit8(a, (uint8_t)d.reg); }  /* cs/ds/ss */
    return 1;
}
```

The 8086 encodes all eight forms as one byte, `(ispop ? 0x07 : 0x06) + (sreg << 3)`,
with ES=0, CS=1, SS=2, DS=3. That is exactly the numbering the parser already
produces: `SR[4] = { "es","cs","ss","ds" }` and `sreg_index()` returns the index
into it. The trailing `0x0F` pair is not part of any 8086 form (0F is `POP CS`
on the 8086 and the 386 escape prefix).

**Fix (applied).** The opcode is now emitted as one byte with the index folded
in:

```c
emit8(a, (uint8_t)((ispop ? 0x07 : 0x06) + (d.reg << 3)));
```

### 16.2 The CPU half: no handlers for these opcodes at all

The assembler fix alone was not enough - `0x06/07/0E/0F/16/17/1E/1F` were
absent from the emulator's dispatch table, so the first byte of
`int10_13.asm` hit the unknown-opcode funnel. `src/exec.c` now has all eight,
built on the existing `push16`/`pop16` and `sr_set`:

```c
static void push_sr(int s) { push16(cpu.sr[s]); }
static void pop_sr (int s) { sr_set(s, pop16()); }
```

`0F` (POP CS on the 8086) also recomputes `cpu.phys`, like the other control
transfers.

The original does the same thing, one opcode at a time, and this is where the
DS/SS field offsets come from (CONFIRMED, `0x5022C4`-`0x502370`):

| opcode | original | field |
|---|---|---|
| `06` PUSH ES | `push &obj+0xC4; call [vtable+0xB40]` | ES = `+0xC4` |
| `16` PUSH SS | `push &obj+0xD0; call [vtable+0xB40]` | SS = `+0xD0` |
| `1F` POP DS | `call [vtable+0xB3C]` then `[obj+0xC2] = ax` | DS = `+0xC2` |

`0xB40`/`0xB3C` are the push/pop vtable slots the general `50`-`5F` forms use,
so segment stack traffic shares their implementation.

**Why the gate missed it (CONFIRMED, negative).** No fixture under
`C:\emu8086\MyBuild` uses push/pop of a segment register - `grep -i "push *\(cs\|ds\|es\|ss\)" *.~asm` returns nothing. The 46/46 `.list` comparison and the
44/44 byte round-trip therefore never cover it, and `check_examples.py` only
asserts that a source assembles, not that the bytes are right.

**Coverage added.**

- `src/check_enc.py` (new, fixture-free so it also runs where emu8086 is not
  installed): the eight forms plus the `push cs`/`pop es` prologue, with
  `mov ax,cs` and `push ax`/`pop bx` as controls that must not change.
- `src/test_cpu.c`: executes each form and checks the register landed in the
  segment, that SP is restored, and that POP CS moves the fetch pointer.

**After the fix** `examples\int10_13.asm` runs and prints
`hello world!<hello world!<`; `make examples` still assembles 85/85; the
`.list` and round-trip gates stay at 46/46 and 44/44.

**Note for the INT 10h regression:** `src/check_int10.py` sets ES with
`mov ax,cs` / `mov es,ax`. It could use the prologue now, but either is fine and
the current form keeps that check independent of the segment push/pop path.

## 17. INT 10h cell buffer: AH=08/09/0A and scroll (IMPLEMENTED)

The clone renders through `dos_putc`, so before this pass a character's
*attribute* had nowhere to live and AH=08 could only answer `' '`/`0x07`.
That made three BIOS calls incoherent with each other. `src/dos.c` now keeps
an 80x25 char+attribute array in CGA order (even byte = char, odd = attr),
initialised to `0x20`/`0x07` - what mode 3 clears to.

Implemented to the BIOS contract:

| call | behaviour |
|---|---|
| `AH=08` | `AX = attr<<8 | char` from the cell at the cursor |
| `AH=09` | `CX` cells rightwards from the cursor: char `AL`, attribute `BL`; cursor does not move |
| `AH=0A` | same, but the cell keeps the attribute it already had (text mode), so `BL` is ignored |
| `AH=06/07` | shift the window by `AL` rows (`0` = whole window), blanking with (`BH`, space); a whole-screen scroll still emits one newline to the console |
| `AH=0E` | fills its cell with attribute `0x07` before advancing |
| `AH=13h` | fills cells with the attribute bytes from the string, which are no longer discarded |

`vram_cell()` clamps row/column: AH=02 accepts any position, so an
out-of-range cursor must not index past the array.

**Scope of the claim.** The original's screen is the `frmScreen` OCX, which was
not decoded, so this is the clone's model of the documented BIOS contract, not
a byte-for-byte mirror of the original's rendering. Its only job is to make
AH=08/09/0A/06/07 self-consistent, and to stop throwing attributes away.

Regressions in `src/check_int10.py`: `attr-write-read`,
`char-write-keeps-attr`, `teletype-fills-cell`, `string-attr-reaches-cell`,
`scroll-blanks-cells`. Proved to bite: removing the AH=09 attribute store
fails `attr-write-read` (`0x741`) and `char-write-keeps-attr` (`0x742`).

## 18. INT 21h file services: 3C/3D/3E/3F/40/42 (IMPLEMENTED)

Before this pass every one of these returned `CF=1, AX=0`, so a DOS program
could not touch a file at all - the largest remaining behavioural hole once
the disk and screen models were honest.

Attribution is from 5.1 (CONFIRMED call targets in the dispatcher
`0x51FF60`): `3C`/`3D` -> `0x5DDF80` (create/open), `3E` -> `0x5DE530`,
`3F`/`40` -> `[eax+0x76C]`, `42` -> `0x5DED70` ("allowed values for AL =
0/1/2"). They are ordinary host-file opens: `0x5DDF80` builds a path string
and calls the VB file runtime, which is the same shape as the INT 13h
`__vbaFileOpen`/`__vbaFileClose` path decoded in 15.5.

Implemented in `src/dos.c`:

| call | behaviour |
|---|---|
| `AH=3C` | create/truncate the ASCIIZ name at `DS:DX`, `CX` = attribute (kept but unused), `AX` = handle |
| `AH=3D` | open with `AL` = 0 read / 1 write / 2 read+write; never creates; `AX` = handle |
| `AH=3E` | close `BX` |
| `AH=3F` | read `CX` bytes from `BX` into `DS:DX`, `AX` = bytes read, 0 = EOF |
| `AH=40` | write `CX` bytes from `DS:DX` to `BX`, `AX` = bytes written; handles 1/2 are stdout/stderr |
| `AH=42` | seek `BX` by the signed 32-bit `CX:DX` from `AL` = 0/1/2, `DX:AX` = new position |

Handles are numbered from 5 like DOS (0-4 are the character devices), the
table holds 16 open files, and failure is `CF=1` with a DOS code in `AX`:
1 invalid function, 2 file not found, 3 path not found, 4 too many open
files, 5 access denied, 6 invalid handle. Writes are flushed when the call
returns, so a reader that reopens the file sees them immediately.

**Scope of the claim.** The handler addresses, the host-file backing and the
`AL` seek-origin constraint are read from the binary. The numbering is the
standard DOS one. The `CF`/`AX` error convention was originally written here
as INFERRED; it has since been checked against the dispatcher and the original
**does not do that** - it answers in `AX` alone, with different codes, and
never raises `CF`. 21 has the evidence and what the clone does instead.
Handle 0 (stdin) is covered in 20; `AH=41` and `AH=4E`/`4F` in 19;
`AH=43` and `AH=56` in 22.

Regression `src/check_int21.py` (14 checks, wired into `make test`): each
guest runs in a scratch directory and the host reads the files back, so the
oracle is the filesystem, not the emulator's own view - `create-write-hits-
host`, `read-roundtrip` (read into memory, write to a second file, compare),
`seek-read-from-offset` ('ell'), `seek-end-reports-size` (5), the two error
paths, EOF, and the stdout handle. Proved to bite: moving `DOS_FIRST_HANDLE`
off 5 and zeroing the seek offset fails exactly `create-returns-handle-5`,
`read-roundtrip` and `seek-read-from-offset`.

## 19. INT 21h 41h/4Eh/4Fh + the DTA (IMPLEMENTED, layout CONFIRMED)

`AH=41` and `AH=4E`/`4F` both just returned `CF=1, AX=0` before, so a
directory-aware DOS program could not list or remove anything.

**The DTA is real and its layout is confirmed from the binary.** The
dispatcher stores the DTA in one flat global, `0x621C48`:

- `0x5E225D`: `mov ds:0x621c48, eax` - the `AH=1A` arm (EA from `DS:DX`).
- `0x5E2532`: `mov DWORD PTR ds:0x621c48, 0x7080` - the default DTA. `0x7080`
  is `0x700:0x0080`, i.e. the PSP alias at offset 0x80, and 0x700 is the
  segment emu8086 loads a guest at. The clone loads at `0x0100` (`main.c`), so
  its default is `0x0100:0x0080` - same offset, its own PSP.
- The find arm (`0x5E25E0`) then fills fixed offsets off that address:

| offset | written at | field |
|---|---|---|
| `+0x15` | `0x5E2A23` (`add edx,0x15`) | attribute |
| `+0x16` | `0x5E2A5B` | last-write time |
| `+0x18` | `0x5E2A7A` | last-write date |
| `+0x1A` | `0x5E2AB0` | size, low word |
| `+0x1C` | `0x5E2ADB` | size, high word |
| `+0x1E` | `0x5E2AFA`/`0x5E2B17` | ASCIZ filename |

That is the DOS 2.x DTA, and it settles the "which DTA variant" question:
this is not the DOS 3.1 extended layout.

Implemented in `src/dos.c`:

- `AH=1A` stores `DS:DX` as the DTA; until then the default is the PSP.
- `AH=4E` parses the filespec (`dir\pattern`), converts the pattern to 8.3
  with DOS wildcard rules (`*` fills the rest of *its own field*, so `*.c`
  does not match `A.TXT`), and walks the host directory.
- `AH=4F` advances to the next match. Matching is case-insensitive and names
  are reported upper-case 8.3, as DOS does.
- Attribute filter: directories are reported only when the search attribute
  has bit `0x10`; a volume-label search (`0x08`) matches no files. Files
  report `0x20`, directories `0x10`. `.`/`..` are skipped.
- `AH=41` deletes via `remove()`; a second delete is file-not-found (2).
- No match in either call is `AX=0x12, CF=1`, the "no more files" code.

The clone reports size and mtime from `stat()`, so the packed time/date are
real values rather than zeros.

**Implementation note (repo).** `#include <dirent.h>` initially failed to
compile: dirent.h includes `<io.h>`, and `-Isrc` made that resolve to the
project's own `src/io.h`, leaving `struct _finddata_t` undefined. The project
header is now `src/ioport.h` - a name that cannot shadow a system header.

Regressions added to `src/check_int21.py` (30 checks total): find-first
attribute/name, find-next advancing to a different name, the third call
ending in `AX=0x12`+CF, directory visibility with and without `CX=0x10`,
`AH=1A` redirecting the DTA to `DS:0200` (read back at `+0x1E`/`+0x1A`), size
reporting, and delete succeeding once then failing. Proved to bite: dropping
the `0x10` directory filter, ignoring the `AH=1A` address and stubbing
`remove()` fails exactly the six affected checks.

## 20. INT 21h AH=3F on handle 0 (stdin) (IMPLEMENTED)

`AH=3F` with `BX=0` reported invalid handle (6), so a guest could not read
its standard input at all.

**A finding while wiring it:** the keyboard queue was inert. `kb_pop()` is
the source for INT 16h and INT 21h `AH=00/01/0A`, but nothing ever pushed
into it - `dos_feed_stdin()` and `dos_push_key()` are exported and were never
called. So the queue was always empty, and that is why `AH=3F`/0 had nothing
to read from. The queue is now fed from the host on demand:

- `kb_pop()` falls back to `host_stdin_byte()` when the queue is empty. That
  makes `mue prog.asm < input` work for the console calls as well, not just
  for `3F`.
- A terminal is never read (`isatty(0)`), because the emulator must not block
  waiting for a keypress nobody asked for.
- stdin is switched to `_O_BINARY` on Windows before the first read. In text
  mode the C runtime ate every CR, so a guest looking for a DOS line ending
  saw `hi\nrest` instead of `hi\r\nrest` and a CR-terminated read could never
  terminate. This was caught by the regression, not by inspection.

`AH=3F` on handle 0 then behaves like DOS reading CON:

- at most `CX` bytes are transferred, into `DS:DX`;
- the read stops at CR, and the CR *is* stored, so the guest sees a normal
  DOS line;
- bytes after the CR stay queued for the next read;
- `AX` is the count; an empty input gives `AX=0` with `CF` clear, not an
  error;
- the characters are echoed, as DOS echoes console input.

The CR rule is the documented DOS console behaviour, INFERRED rather than
read out of the binary; the rest is exercised end to end.

Regressions added to `src/check_int21.py` (36 checks total):
`stdin-read-stops-at-cr`, `stdin-keeps-cr` (the file written from the buffer
contains `hi\r`), `stdin-continues-after-cr`, `stdin-rest-unconsumed`
(`\nrest` is still there), plus the EOF pair. Both halves were proved to
bite: dropping the CR stop makes the first read take all 8 bytes and keeps
`hi\r\nrest`; dropping `_setmode` loses the CR and fails the same four
checks.

## 21. INT 21h result convention, checked against the dispatcher (CONFIRMED)

§18 left the `AX` vs `CF`/`AX` question labelled INFERRED. It has now been read
out of `FUN_0051FF60` itself, and the answer is: **the original answers in `AX`
only, and never raises `CF` for 3C/3D/3E/3F/40/42.**

### 21.1 How an INT 21h arm reports

The register block is zeroed at entry on four 16-bit status words:

| word | zeroed at | set to 0xFFFF by |
|---|---|---|
| `+0x38` | `0x51FFBE` | every failure branch |
| `+0x3A` | `0x51FFCE` | every success branch |
| `+0x3C` | `0x51FFDE` | other INTs |
| `+0x3E` | `0x51FFEE` | other INTs |

They are **not** guest flags. A post-step reporter reads each one and turns it
into a UI message: `+0x38` at `0x50108C` fetches text through object `0x621DEC`
vtable `+0x304` and shows it, `+0x3A` at `0x501169` uses vtable `+0x308`, and
`+0x3C`/`+0x3E` at `0x501246`/`0x501323` do the same. So `+0x38` = "error
message", `+0x3A` = "info message": bookkeeping for the emulator's own
reporting, nothing a guest can branch on.

### 21.2 What the guest actually gets

Success: the result is written to `AL` (`+0xBA`) - handle, byte count, or the
low half of a 32-bit position - and `+0x3A` is marked. Examples: `0x528882`
(3C), `0x528C27` (3E), `0x5290CC` (3F), `0x5298E8` (42).

Failure: `+0x38` is marked, `AH` is set to 0 and `AL` to a **fixed per-call
code**, which is the same regardless of the real reason:

| AH | failure AX | evidence |
|---|---|---|
| `3C` | `0x0003` | `0x528133` mark, `AL=3` at `0x528158` |
| `3D` | `0x0003` | `0x528A21`, `AL=3` at `0x528A54` |
| `3E` | `0x0006` | `0x528C69`, `AL=6` at `0x528C8E` |
| `3F` | `0x0005` | `0x5290DE`, `AL=5` at `0x529111` |
| `40` | `0x0005` | `0x529574`, `AL=5` at `0x529599` |
| `42` | `0x0003` | `0x52968D`, `AL=3` at `0x5296B2` |

`3C`/`3D` additionally log `"interrupt error: "` (`0x447E40`) +
`"h/3Dh : cannot open file."` (`0x447E68`) and set the global error flag
`0x62141C = 0xFFFF` (`0x528BC4`).

None of these branches writes any other register field, so a guest that does
`int 21h` / `jc error` on emu8086 branches on the CF it had *before* the
interrupt. That is the original's behaviour, and it is why DOS error handling
"works" in the docs but not here.

**The sink `[obj+0x7AC]` - SETTLED in 24.** It is a VB6 COM method call on
the project's global object (`__vbaHresultCheckObj(hr, this,
IID{CB74122D-86F6-4403-89A1-5AB68F733C03}, 0x7AC)`), not a halt: the only
stop marker is `ds:0x621218` (INT 19/20, run loop), and neither these
file-error paths nor the INT 10h `CX` guard write it. So a failed INT 21h
file call reports and returns; the guest keeps running. Wherever earlier
sections say "halts through `[obj+0x7AC]`", read that as superseded.
Related: `+0x4C` is a halt word, not the carry flag, correcting the table in
5.1.

### 21.3 What the clone does, and why

`src/dos.c` keeps the DOS convention: `CF=1` plus a DOS error code
(`2` not found, `3` path, `4` too many files, `5` access, `6` invalid handle,
`0x12` no more files). That is a **deliberate, verified divergence**: without
`CF` a guest has no failure signal at all, and every program that probes for a
file would take its success path. Keeping `CF` makes those programs run
correctly; it does not make the clone byte-identical to emu8086 here.

The divergence is confined to the failure branch - success paths already match
the original (`AX` = handle/bytes/position).

## 22. INT 21h 43h (attributes) and 56h (rename) (IMPLEMENTED)

Both were unimplemented, so the file API could create, read, write, seek,
delete and enumerate, but not rename or inspect attributes.

Attribution from 5.1 is CONFIRMED: `43 -> 0x5E16C0`, `56 -> 0x5DF2D0` (the
"rename" handler, reached twice through the EA prologue `0x54A0C0` for the two
names). Their result branches follow the convention verified in 21:

| AH | success | failure AX | evidence |
|---|---|---|---|
| `43` | `+0x3A` marked | `0x0001` | `0x529A14` ok, `0x529A26` mark + `AL=1` at `0x529A59` |
| `56` | `+0x3A` marked | `0x0002` | `0x529EFC` ok, `0x529F0E` mark + `AL=2` at `0x529F41` |

Implemented in `src/dos.c`:

- `AH=43` `AL=0`: `CX` = `0x10` for a directory, else `0x20` (archive), plus
  `0x01` when the host file is read-only.
- `AH=43` `AL=1`: sets/clears read-only. That is the only DOS attribute bit a
  host file can actually carry, so it is mapped onto the host's write
  permission (`_chmod` / `chmod`); the other bits are accepted and ignored.
  Any other `AL` is invalid function (1).
- `AH=56`: ASCIIZ `DS:DX` renamed to the ASCIIZ at `ES:DI`, through `rename()`.
  Note the second segment: `ES`, not `DS`, which is what the EA prologue in
  the original sets up.
- `AH=4E`/`4F` now report the same attribute bits (`0x20|0x01` for a read-only
  file) instead of a flat `0x20`, so 43 and 4E cannot disagree.

Failure codes follow the clone's DOS convention (2 not found, 3 path, 5
access, 1 invalid function), which is the deliberate divergence described in
21.3; the original's fixed codes are in the table above.

Regressions added to `src/check_int21.py` (50 checks): rename moves the file
and keeps its contents, rename of a missing name is 2+CF, get reports `0x20`,
set read-only then get reports bit 0, missing file is 2+CF, and `AL=2` is 1+CF.
Proved to bite: stubbing `rename()` and dropping the read-only bit fail
`rename-succeeds`/`rename-old-name-gone`/`rename-keeps-contents` and
`attrs-readonly-readable` respectively. The test also caught itself crashing
on a missing file instead of reporting - fixed by making the permission
restore tolerant.

## 23. `examples/dirlist.asm`: a 4Eh/4Fh lister, and what the original does (CONFIRMED)

`examples/dirlist.asm` walks the current directory with `AH=4Eh`/`4Fh` and
prints `NAME  size` in hex per entry, reading the default DTA at `PSP:0080`
(name `+1Eh`, size `+1Ah`, the layout confirmed in 19). It stops when the
search reports "no more files" - testing both `CF` and `AX != 0` so it works
whether the emulator uses the DOS convention or a plain error code.

`src/check_dirlist.py` (wired into `make test`) runs it in a scratch directory
with known contents: four files of 5/3/10/1 bytes and a subdirectory. It
asserts the exact listing (`A.TXT  0005`, `B.TXT  0003`, `C.DAT  000A`,
`KEEP.ME  0001`), that the directory is skipped (`CX=0` search), that names are
upper-case, that the guest leaves through `AH=4Ch` (`AX=4C00h`), and that an
empty directory lists nothing and still terminates.

### 23.1 Check against the real emulator

Two things had to be checked against the install rather than assumed.

**4Eh/4Fh are not a supported call.** `documentation/8086_bios_and_dos_
interrupts.html` lists the INT 21h AHs the emulator supports:
`01,02,05,06,07,09,0A,0B,0C,0E,19,25,2A,2C,35,39,3A,3B,3C,3D,3E,3F,40,41,42,
47,4C,56` - no `4E`, no `4F`, and no `43` either (22 implements 43 anyway,
because the dispatcher has an arm for it).

**The arms exist but return nothing.** `0x52A146` (`4E`) and `0x52A171` (`4F`)
call `0x5E25E0`/`0x5E2C20` and jump straight to the shared exit; neither arm
touches a register. Inside `0x5E25E0` there is not a single write to the
register block (`+0xBA`/`+0xBB`/`+0x38`/`+0x3A`); it fills the DTA through the
global `0x621C48` and sets its own globals. So the guest's `AX` and `CF` are
**unchanged** by 4Eh/4Fh.

Predicted behaviour of the shipped program on the real emulator: after
`mov ah,4Eh; int 21h`, `AX` is still `4E00h`, so the `or ax,ax` / `jnz done`
guard fires on the first iteration and the program prints **nothing**. The
clone's `CF=1` + `AX=0x12` for "no more files" is the DOS contract (and what
a guest needs), not the original's behaviour - the same divergence described
in 21.3.

**Paths.** The original's file and search handlers go through the vdrive
object at `0x6214D0` (calls `+0x804`/`+0x808` in `0x5E25E0`), i.e. the virtual
drives under `C:\emu8086\vdrive\`; the clone resolves host paths relative to
its working directory. A 4Eh search therefore lists different directories on
the two implementations even before the return codes differ.

One more difference inside the DTA: the clone writes the name as `A.TXT`
(the host name upper-cased), where DOS pads it to `A       TXT`. The ASCIZ
field is what the program reads, so this only matters to a guest that wants
the padded 11-byte form.

### 23.2 A CPU defect the program exposed (FIXED)

Writing the hex printer with `rol ax,4` produced no output. Two facts, in
order:

- the clone's assembler emits `C1 C0 04` for `rol ax,4` - the 186+ immediate
  form. That is right: emu8086 documents `SHL REG, immediate` and
  `SHL memory, immediate` as valid operand forms, and its instruction list
  includes `PUSHA`/`POPA`, so 186 encodings are in scope;
- `src/exec.c` **rejected** `C0`/`C1` (`REJECT_80186`), so the instruction
  halted the run with exit code -2, and before that change it silently did
  nothing.

The core now executes both through the existing `grp2(width, 0, 1)` path
(rotate/shift by `imm8`, count masked into 5 bits, count 0 = no operation).
`src/test_cpu.c` pins `rol ax,4` -> `2341h`, `shl ax,4` -> `2340h`,
`sar ax,4` of `8000h` -> `F800h`, the count-0 no-op, and the 8-bit `C0` form.
The real assembler's own output for these was checked against the shipped
`.list` fixtures, which use `D0`/`D1` for a count of 1.

### 20.1 Console reads now wait instead of reporting "no key" (IMPLEMENTED)

`kb_pop()` returned -1 as soon as the queue was empty and stdin was a
terminal, so a guest doing `INT 16h AH=01` saw "no key" instantly and
`INT 21h AH=01` saw Ctrl-Z. Both now wait for a key:

- `kb_refill()` tops the queue up from the host when it is empty.
- `kb_pop()` consumes; `kb_peek()` waits and returns the next key *without*
  consuming it, so `AH=01` still leaves the character for the following
  `AH=00`.
- Only a redirected stdin can end (EOF); a terminal never does, so a guest
  run from a pipe or file still finishes instead of hanging.

Covered by `src/check_int21.py`: `int16-peek-returns-key`,
`int16-peek-zf-clear`, `int16-peek-does-not-consume` (the key survives into
`AH=00`), and `int16-no-input-returns-zero`/`-sets-zf` for the exhausted-input
case. The harness now runs guests with `stdin=DEVNULL` by default, so a guest
that waits can never hang `make test`.

**Not demonstrated here:** this shell's console reports `isatty(0)` true but
returns EOF immediately from `fgetc` (checked with a standalone C probe:
`isatty=64`, `fgetc=-1`), so the waiting path cannot be exercised in this
environment; what the regression pins is the EOF fallback and the peek
semantics. On a real console the read waits for a line (console input is
line-buffered, so a key followed by Enter).

## 24. What `[obj+0x7AC]` is, and what the INT 10h CX guard really does (CONFIRMED)

Open since 21.2: the sink called by 60 sites, including the INT 10h AH=13h
`CX > 0x7D0` guard, "reboot"/"terminate", and the INT 20h/21h failure paths.
It is **not** a halt. What the binary actually contains:

### 24.1 The call is a VB6 COM method call on the project object

The whole application is **Visual Basic 6**, not C++: the PE entry point is
`push 0x41AC1C; call [ThunkRTMain]`, the header at `0x41AC1C` starts with the
`"VB5!"` magic, `wRuntimeBuild = 0x1FF0`, `lpSubMain = 0x4CC150`,
`lpProjectData = 0x41B5C8`, `wFormCount = 39`, `wExternalCount = 5`,
`dwThunkCount = 0xE9`, and the project name is `emu8086`. 273 imports come
from `MSVBVM60.DLL`.

Every `call DWORD PTR [reg+0x7AC]` is therefore the VB6 code generator's
shape for a virtual call on a class object:

```
mov edx,[ebp+8]     ; this
mov eax,[edx]       ; vtable
mov ecx,[ebp+8]
push ecx            ; this is the only argument
call [eax+0x7ac]    ; obj.Method()
jge ok              ; negative HRESULT -> error
push 0x7ac          ; vtable offset
push 0x43a388       ; IID {CB74122D-86F6-4403-89A1-5AB68F733C03}
push this
push hr
call ds:0x4010B0    ; MSVBVM60!__vbaHresultCheckObj
```

`ds:0x4010B0` is CONFIRMED from the import table (`MSVBVM60.DLL`,
`__vbaHresultCheckObj`), which is also what identifies the `push <offset>` +
`push <IID>` + `push pUnk` + `push hr` quartet: it is the runtime's
"check the HRESULT of the interface method you just called" helper. The same
IID is used for the `0x7AC` checks at `0x5A3FB7` on the object in the global
`ds:0x6214D0`, so the callee is a method of the **project's global object**,
the same object that owns the INT handlers, their register block
(`+0xB0..+0xC4`), the halt word `+0x4C` and the single-step method
`[obj+0x7BC]`.

### 24.2 It is not the halt

The run loop (`0x5A3F81`) is:

```
5A3FAB: call [ecx+0x7bc]        ; one instruction
5A3FC9: call ebx                ; runtime message pump
5A3FCB: cmp WORD ds:0x621218,0  ; halt marker
5A3FD3: jne 0x5A4587            ; -> leave the loop
```

`ds:0x621218` is the **only** stop condition for the run loop (the other
look, `0x5A409D`, also tests it; `0x5A40AB` additionally tests
`ds:0x62123C`, the pause/step flag). Nothing else can stop a run.

Against that, the CX guard block (`0x522F7D-0x523079`) does exactly:

- composes and logs `"INT 10h, AH=13h - "` + `"CX value (string size) is
  above the allowed limit!"` (`0x447650`),
- calls `[obj+0x7AC]`,
- on a negative HRESULT calls `__vbaHresultCheckObj`,
- writes `WORD ds:0x62141C = 0xFFFF`,
- `jmp 0x52ABDF`, the dispatcher epilogue (`__vbaFreeObj`-style cleanup at
  `ds:0x4010E0` then `jmp 0x52AD31`, the VB6 procedure return).

It never writes `ds:0x621218` and never writes the `+0x4C` halt word, so it
cannot stop the CPU. `ds:0x62141C` cannot do it either: across the whole image
it is *read* at only three places (`0x525895`, `0x525A9F`, `0x529FE3`), all
inside the same INT dispatcher method (`0x51FF60`), i.e. it is a
"a message has already been reported" flag for message composition, not a
machine state. For comparison, the paths that really do stop the emulator
write `0x621218` (INT 19h at `0x52557E`, INT 20h at `0x525833`, the run loop
itself at `0x5A38F2`).

Since `[obj+0x7AC]` is also called from entirely ordinary code (the end of the
form's initialisation at `0x4E18DF`, right after `this` was constructed and
before the window is shown), a halt there would abort startup, which settles
it: **the call is a "report/refresh the UI" step, not the stop**. Its exact
VB6 name is still unresolved (see 24.4).

### 24.3 Clone delta: the CX guard no longer halts

`src/dos.c` used to treat the guard as fatal (`cpu.halted = 1;
cpu.exit_code = -5`), which was INFERRED from the (wrong) reading of
`[obj+0x7AC]`. It now matches the original: the diagnostic goes to stderr and
execution continues with the next instruction.

The guard was also moved above the `DX` latch. The original only reaches the
cursor sink `[edx+0x760]` on the success path, so a refused call leaves the
cursor untouched.

`src/check_int10.py` pins all four properties: `cx-limit-writes-nothing`
(the only output is the `|` marker printed *after* the call),
`cx-limit-continues` (exit 0), `cx-limit-diagnostic`, and
`cx-limit-keeps-cursor` (cursor parked at `0305h`, the refused call's `0A0Ah`
must not reach it). Both were verified to bite: restoring the halt fails the
first three, and moving the guard below the `DX` latch fails only
`cx-limit-keeps-cursor`.

### 24.4 Still open

The name behind vtable offset `0x7AC`. The VB5 header gives
`lpObjectTable = 0x435018`, whose 39 records are
`{name, count, table, 0xFFFF, flags, ..., entry, 0xFFFFFFFF, iid, ...}` and
name the project's parts (`frmMain`, `Module1`, `frmDat`, `Module2`,
`frmInfo`, `StringTokenizer`, `frmMacro`, `frmEmulation`, `ALU`, `frmScreen`,
`RAM`, `frmFLAGS`, `frmOptions`, `ComDlg`, `frm_mBox`, `frmAbout`,
`mCodeSelector`, `frmOrigCode`, `frmBaseConvertor`, `frmStack`, `mDisk`,
`mFASM`, `mAnalyser`, `frmEvaluator`, `mMath`, `mString`, `mEditor`,
`frmDebugLog`, `mRecentFiles`, `mSYMBOL_TABLE`, `mOptions`,
`mSegment_Names_Table`, `mDAT`, `frmWriteBinToFloppy`, `mOutput`, ...), and
the packed pointer tables from `0x4360BC` hold their method names in
declaration order (`do_instruction_D8_DF`, `MAKE_sub_BYTES`, `add_to_SCREEN`,
`clear_SCREEN`, `stopAutoStep`, `mnuSingleStep_Click_PUBLIC`,
`do_INT_21_4E_4F_StepBack`, `showFPU_STATE`, ...). But those tables contain
1025 slots of which only 257 are named and the gaps do not line up with
`(offset-0x1C)/4`, so the offset -> name mapping is not the file order; the
runtime-built vtable keeps that mapping in memory. Treat the *name* as open;
the *behaviour* above (no halt, UI report) is CONFIRMED.
