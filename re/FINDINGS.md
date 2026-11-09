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
`+0xBF`=CH, `+0xC0`=DL, `+0xC1`=DH, `+0xC4`=SI / `+0xCA`=DI (x16 phys),
`+0x4C`=carry flag word. `0x621218` = global carry mirror,
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
| `15` | AH=`86` only (BIOS wait; `fmul [0x409828]` delay math) | fail template `0x4478E4` at `0x5245F4` |
| `16` | AH=`00,01` | kb-buffer OCX vtable sink `+0x750/0x754`; fail template `0x4479B4` at `0x524BDF` |
| `17` | - | direct `CALL 0x550B40` ("wrong parameter...") |
| `1A` | AH=`00` (time; `fmul` + `0x4011`-scaled `rtcHexVarFromVar`) | fail template `0x4479D4` at `0x5252EE` |
| `19` | - | reboot: `[ecx+0x4C]=0xFFFF; [0x621218]=0xFFFF; CALL [eax+0x7AC]` |
| `20` | - | terminate: same `0x4C/0x621218=0xFFFF` + `[eax+0x7AC]` halt |
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

Clone gaps this closes (DESIGN follow-ups, not behaviour changes): none -
`src/dos.c` already implements every documented AH above. AH=13h decoded
this pass (see 5.2); two deltas remain: INT 10h AH=`01/02/03/0C/0D` +
`08/09/0A` attribute bytes (clone writes chars but drops page/attribute;
original routes through `frmScreen` OCX + the `0x621C04`-guarded helpers),
and INT 13h AH=`02/03` real sector I/O (clone returns success without
touching `FLOPPY_0`).

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

Clone delta (`src/dos.c` `case 0x13`): original writes through the
`frmScreen` OCX with per-cell attributes (w1 doubles CX, attr byte follows
char) and cursor-advance control (bit0); the clone emits CX raw bytes and
ignores BL/attr/advance. Behaviour-identical only for the common
`AL=0/1, CX<=0x7D0` text-mode demo path; DESIGN follow-up, not changed.

### 5.3 INT 13h AH=00/02/03 floppy sector I/O  (CONFIRMED - objdump + file stat)

Entry `0x523C49` (INT byte test `0x13`, slot copy `AH->[ebp-0x278]` at
`0x523C50-0x523C59`). Three AH arms, all CONFIRMED:

- AH=`00` reset (`0x523C66-0x523C86`): `WORD [regblock+0x3A] = 0xFFFF`
  (drive-status word), then shared exit `0x5240EF -> jmp 0x52A934`.
  CONFIRMED cell: reset needs no disk access; clone can match exactly.
- AH=`02` read (`0x523C92-0x523D7B`): prologue copies BH/BL (`+0xBD/+0xBC`)
  into `[ebp-0x248]` via the `[eax+0x76C]` accessor pair (same EA-getter
  shape as INT 21h file `3C/3D` prologue `0x54A0C0+0x5DC410`), then
  `p_buf = SI*0x10 + [ebp-0x248]` (`SI=+0xC4`, `0x4AD050` + `imul` + add
  at `0x523D15-0x523D39`, stored `[ebp-0x24C]`) and
  `CALL 0x595E00(p_buf, BL, CL, CH, DL, DH)` -
  arg order pushed: `+0xBA(AL),+0xBF(CH),+0xBE(CL),+0xC1(DH),+0xC0(DL),buf`.
  `FUN_00595E00` is 1011 B / 36 blocks (Ghidra `procedure_info`).
- AH=`03` write (`0x523D80-0x523E88`): same shape, buffer physicalised as
  `p_buf = [ebp-0x250](=SI field via 0x4AD050 at 0x523DA5)*0x10 +
  [ebp-0x248]` (`0x523E20-0x523E44`), then
  `CALL 0x596370(p_buf, ..., w_flag)` with an extra leading
  `WORD [ebp-0x23C] = 0` argument slot (`0x523E20/0x523E4A`).
  `FUN_00596370` is 1091 B / 38 blocks.
- Fail path (`0x523E92-0x5240EF`, all other AHs): logs template
  `"INT 13h, AH=0"` (`0x447884`) + `"h   "` (`0x4478A4`) +
  `"- not supported yet."` (`0x4478B4`) + `"refer to the list of
  supported i..."` (`0x44782C`) composed through `0x5F2380` + `0x547EC0`
  (same log pipeline as the INT 10h fail paths), then forces the
  `+0x7AC` halt call and sets `WORD [0x62141C] = 0xFFFF` before the shared
  exit.

Geometry ground truth (CONFIRMED by host file, not by REA):
`C:\emu8086\FLOPPY_0` = 1474560 bytes = 80 tracks x 2 heads x 18 sectors
x 512 B (standard 1.44 MB floppy image; the file is the backing store the
read/write arms address). `0x595E00/0x596370` internals NOT decoded this
pass - only their call signatures (arg order/count) and sizes are claimed.

Clone delta (`src/dos.c`): `dos_interrupt` has NO `case 0x13` arm - INT 13h
falls through to `return 0` (forward to IVT). To close the gap per this
spec: add `case 0x13: int13(); return 1;` with `AH=00 -> clear CF + AH=00`
(success); `AH=02/03 -> CHS decode (C=CH, H=DH, S=CL&0x3F), linear =
(C*2+H)*18 + (S-1), file offset = linear*512 against the FLOPPY_0 image,
memcpy to/from ES:BX (p_buf per above), set CF/AH on bounds errors; else
CF=1, AH=1 (invalid function)` mirroring the fail path. DESIGN + behaviour
change; NOT implemented this pass (docs only), so `make test` stays green.

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
