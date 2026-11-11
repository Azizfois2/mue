# Gap list: mue vs. emu8086

Status of every surface I have compared, as of commit after the
"fix all gaps" pass. **Section 1 is now entirely closed** - see sec. 1.0.

**How this list was produced.** Not from memory or from a wish-list. The
authoritative list of what the original *dispatches* comes from the
`MOV ECX,imm / CALL __vbaUI1I2 / CMP [ebp-slot],AL` chains recorded in
`re/FINDINGS.md` sec. 5.1 - one per interrupt, keyed by the AH slot:

| INT | slot | AH values the original dispatches |
|---|---|---|
| 10h | `[ebp-0x270]` | 01 02 03 05 06 07 08 09 0A 0C 0D 0E 0F 11 13 (15) |
| 13h | `[ebp-0x278]` | 02 03 (2) |
| 15h | `[ebp-0x27C]` | 86 (1) |
| 1Ah | `[ebp-0x26C]` | 00 (1) |
| 21h | `[ebp-0x288]` | 47 values (see sec. 2) |

(Two slot numbers here were wrong in an earlier draft of this file and are
corrected above: INT 1Ah is on `-0x26C`, not `-0x284` - `-0x284` is the local
the AH=00 arm copies CL into at `0x524EA5`. And INT 1Ah has a single
dispatched value, not two; **AH=02 is not implemented by the original at
all**, so there is nothing to match.)

Each of those was then probed against `src/mue.exe` at runtime. "GAP" below
means the clone prints `... not supported yet.` where the original runs an
arm.

---

## 1. Real gaps, ordered by risk

### 1.0 All six closed

Every item in this section is now implemented and covered by
`src/check_gaps.py` (22 checks, wired into `make test`). Negative control:
reverting the 2Eh/54h arms makes `verify-flag-no-report` fail.

| gap | what was done |
|---|---|
| INT 21h AH=2Eh/54h verify flag | one byte, `dos_verify_flag`, round-trips |
| INT 21h AH=17h rename | real `rename()`; needed `DX`->`DI` too, not just `DS`->`ES` |
| INT 21h AH=57h file date | added `dos_fname[]` to remember each handle's host path, then `_stat` + DOS packed CX/DX |
| INT 21h AH=0Ch/0Dh | 0Ch clears the key buffer and re-dispatches on AH=AL (skipping 06/07/08/0A); 0Dh is a pure no-op |
| INT 21h AH=23h/33h | break flag constant 0; Ctrl-Break get/set round-trips |
| INT 10h AH=11h | silent no-op, matching the original's jump-to-epilogue |
| INT 10h AH=0Ch/0Dh text mode | no diagnostic outside graphics modes |

The items below are kept in their original form as the record of what the
gap *was*.

### 1.1 INT 21h AH=57h - get/set file date and time  `0x52A182`

The original calls `FUN_005e2e50` (821 bytes, 42 blocks - real work: it walks
the handle table at `ds:0x6214D0` and touches a host timestamp). The clone
reports unsupported.

**Why it matters:** a program that writes a file and then stamps it - or a
"copy preserving dates" utility - silently loses the date. This is the only
missing INT 21h arm whose original implementation is substantial.

### 1.2 INT 21h AH=2Eh - set verify flag / AH=54h - get verify flag

- `2Eh` (`0x527DB9`) -> `FUN_005e0860`: `mov cl,[eax]; mov ds:0x621CD0,cl`
  (15 bytes, one store).
- `54h` (`0x529E15`) -> `FUN_005e0870`: `mov al,ds:0x621CD0` (6 bytes, one
  load), then AL -> `+0xBA`.

**Why it matters:** trivially small, but `54h` is how a guest *reads* the
flag, and any DOS-compatibility probe calling it gets a spurious "unsupported"
report. Cheap to close and they are a matched pair.

### 1.3 INT 21h AH=0Ch / 0Dh - CLOSED, and my first reading was wrong

I originally described these as "clear buffer and flush, then invoke" and
implemented a `fflush()` of every write handle. Re-reading `0x5275BC`
properly shows neither is a file flush:

- **`0Ch`** zeroes the keyboard count at `ds:0x621270` (`0x5275DA`), calls a
  host console flush (`[edx+0x7B0]`, `0x527636`), then tests AL against
  `06 07 08 0A` (`0x5276A7`-`0x527706`) and - unless AL is one of those -
  sets `AH = AL` (`0x527715`) and **re-enters the INT 21h dispatcher**
  (`call [eax+0x9D4]`, `0x527749`). So it is "flush the console, then run
  the real function"; the four skipped values are the direct-console
  services, which would consume a key right after the buffer was cleared.
- **`0Dh`** is `jmp 0x52A52A` (`0x52776E`) - straight to the epilogue,
  nothing at all. The clone was *doing more* than the original.

### 1.4 INT 21h AH=17h - rename file  `0x5277C0`

Calls `FUN_005e33f0` and `FUN_005dc390`, stores AL at `+0xBA`.

**Why it matters:** moderate - renaming is ordinary DOS work. The clone has
no rename at all.

### 1.5 INT 21h AH=23h / AH=33h - get/set break flag, get/set Ctrl-Break

`23h` at `0x52A1AD` (`WORD PTR [eax+0x7C0]`), `33h` at `0x52A21E`
(`WORD PTR [edx+0xB4C]`, calls `FUN_0060a910`).

**Why it matters:** low, but `33h` collides with the *mouse* INT 33h in
casual reading; a guest checking Ctrl-Break state gets nothing back. Note the
clone already handles INT 33h (mouse reset) separately - do not confuse them.

### 1.6 INT 10h AH=11h - character generator  `0x522CD8`

**This one is NOT a real gap in behaviour.** The original's arm is
`cmp .. / jne / jmp 0x523B50` - it jumps to the epilogue and does *nothing*,
silently, with no report. The clone instead prints
`INT 10h, AH=11h - not supported yet.`

So the difference is a **spurious diagnostic**, not a missing effect. Fixing
it means turning the clone's arm into a silent no-op so stderr matches.

### 1.7 INT 10h AH=0Ch / 0Dh in text modes

The clone has these arms but `cga_pixel()` returns `-1` unless
`gfx_mode` is 1 or 2, so in the default mode 3 they report unsupported:

```
INT 10h, AH=0Ch - not supported yet.
```

The original has no such gate. Either the pixel is silently dropped or its
own mode table decides. Needs a run against the original to settle.

---

## 2. INT 21h: no documented function is missing

All 22 AH values in emu8086's own
`documentation/8086_bios_and_dos_interrupts.html` (0A 0B 0C 0E 19 25 2A 2C
35 39 3A 3B 3C 3D 3E 3F 40 41 42 47 4C 56) are implemented.

**Verified by calling each one, not by reading the dispatch table.** A probe
assembles a guest per AH, calls it with plausible arguments and asserts the
output carries no "not supported" report. All 22 answer. The four that could
have hidden a silent no-op behind an error return were then checked by their
side effect rather than their return value:

| AH | side effect checked |
|---|---|
| 39h mkdir | the directory really appears, then 3Ah removes it |
| 3Ah rmdir | the directory really disappears |
| 3Bh chdir | succeeds on a real directory (SI=0) |
| 47h getcwd | the buffer receives the actual current directory |

Two false alarms during the probe, both mine, both worth recording because
they are easy to repeat: a function that leaves AH untouched on success
(39h returns `AX=3900`) looks like an echoed failure if you read AX instead of
CF, and 47h looks broken if you leave DX pointing at something other than the
buffer.

The clone has 61 arms - more than the original's 47 - so several are supersets
the original does not dispatch (`00 31 44 4B` and others). Those are harmless
but are DESIGN, not CONFIRMED. 31h and 4Bh are documented in sections 1 and 4;
the 4Bh one is the only superset that is a *divergence* rather than an
extension.

---

## 3. Opcodes: nothing refused that should run

Bare-opcode probe of all 256 bytes against `src/mue.exe`: 11 refuse, and all
11 are correct to refuse or out of scope.

| opcodes | mnemonic | verdict |
|---|---|---|
| `26 2E 36 3E` | segment prefixes | prefixes, not instructions - handled by the prefix path |
| `60`-`69`, `6C`-`6F` | PUSHA/POPA/PUSH imm/IMUL imm/INS/OUTS | 80186. Original also skips them with a report - CONFIRMED identical |
| `6A`-`7F` | Jcc short, PUSH imm | handled via the `JCC` macro, not a `SET()` entry |
| `8D` | LEA | works with a real operand (`lea si,[bx+2]` -> `SI=0002`); the bare `8D C3` probe was an invalid ModRM |
| `C4 C5` | LES / LDS | work (`AX=0001` past them) |
| `D8`-`DF` | x87 FPU | **out of scope.** emu8086 is an 8086 emulator: FPU appears nowhere in `8086_instruction_set.html`, no shipped example uses it, and the original's assembler rejects `finit` |

No gap here. Worth stating because "11 opcodes refused" looks alarming until
classified.

---

## 4. Deliberate divergences (DESIGN, not gaps)

These are places where the clone knowingly does not reproduce the original.
Each should stay documented so nobody "fixes" it later:

- **`INT 15h AH=86h` really sleeps** (capped 1000 ms, `MUE_NOSLEEP`). The
  original computes `ticks/1000` ms and sleeps too, but the clone's cap is a
  safety choice.
- **Dispatch is a 256-entry table**, not the original's if/compare chain.
  Same observable behaviour, different structure.
- **`INT 13h` uses `FLOPPY_<DL>` on the host filesystem**; the original holds
  an internal image. Behaviour matches for AH=00/02/03; error codes for
  absent images are DESIGN.
- **`INT 21h` 48h/49h/4Ah implement a real memory arena.** The original
  dispatches these and touches no register (FINDINGS 29), so a guest cannot
  tell success from failure. Reproducing that would copy a broken service.
  **The clone allocates properly and that is the intended behaviour** -
  `MUE_INT21_MEM_NOOP=1` restores the original's silence for comparison.
  Do not "fix" this back to bug-compatibility.
- **`INT 21h` AH=4Bh EXEC really loads and runs a child.** The original has no
  4Bh arm at all (FINDINGS 44) and answers "not supported". Here the clone is
  the *less* faithful side, so this one is **off by default** (`MUE_EXEC=1`
  enables it).

Note the opposite polarities, which are deliberate: 48h/49h/4Ah are better by
default because the original is simply unimplemented there; 4Bh is opt-in
because the original's behaviour is defined (it reports), so matching it is
what "faithful" means.

### The rule

Everywhere a comparison is possible - every example's output, every `.list`
byte, every decoded AH arm - the clone must match exactly. A behaviour may
differ only where the original does nothing useful, and then it must be listed
here, with the flag that restores the original.

---

## 5. Not yet checked at all - now all closed

1. ~~INT 1Ah AH=02~~ - **withdrawn**: the original does not dispatch AH=02.
2. ~~INT 16h shift flags~~ - **not a gap.** emu8086 documents no shift-flag
   service and dispatches only AH=00/01/10/11 (the `[ebp-0x26C]` chain); the
   key *buffer* matches (FINDINGS 40) and there is no modifier byte to model.
3. ~~INT 21h AH=31h TSR~~ - **fixed.** No `31h` arm exists in the
   `[ebp-0x288]` chain, so it falls through to the generic report and the
   guest keeps running. The clone used to halt, silently ending a program the
   original would continue.
4. ~~INT 21h AH=4Bh EXEC~~ - **resolved: it is a divergence, now opt-in.**
   Decoded: there is exactly one `mov ecx,0x4b` in the whole image and it sits
   in an unrelated routine with no `cmp [ebp-0x288],al` after it, so the
   original has **no 4Bh arm**. emu8086's own interrupt reference does not
   document AH=4Bh either, and no shipped example calls it (`snake.asm`'s
   `4bh` is `left equ 4bh`). So under emu8086 a 4Bh call reports
   "not supported yet" and continues.

   The clone really does load and run a child, which is worth keeping but is
   **the clone's behaviour, not the original's**. It is therefore off by
   default (`MUE_EXEC=1` enables it), so the default matches emu8086.
5. ~~INT 20h / 4Ch exit codes~~ - **verified consistent.** `0x529F53` never
   stores AL; it writes the halt marker (`+0x4C` and `ds:0x621218` = `0xFFFF`)
   and returns. So the original does not propagate AL to the process status,
   and the clone's process exit is 0 in both cases. `dos_exit_code` is used
   only for the 4Bh child, which is a clone-only path.
6. ~~INT 10h AH=09/0A with BH != 0~~ - **fixed, and it was broken.** AH=08/09/
   0Ah take the page in **BH**, but the clone used the globally selected page,
   so a write with `BH=1` landed on whatever AH=05h last chose. Confirmed by
   negative control. (AH=0Eh is *not* affected: its documentation says
   "the printing is always done to current active page".)
7. ~~Assembler error recovery~~ - **fixed**, and it uncovered five genuine
   assembler gaps listed below.

### 5a. Assembler gaps found by fixing #7

Making a failed operand an error (instead of silently dropping the
instruction) exposed forms the clone had never really supported. Every one
was accepted before only because nothing checked:

| form | example | what it is |
|---|---|---|
| `10o` | `mov cl, 10o` | octal literal (= 8). `5_sample.asm` |
| `cs:sym` destination | `mov cs:make_minus, 0` | segment override with no brackets. `Calculator.asm`, `factorial.asm` |
| `b.`/`w.` prefixes | `mov bl, b.temp`, `mov w. week, al` | emu8086 symbol size prefixes (`HexConvertor.asm`, `datefile.asm`) |
| late `name = expr` | `size = ($ - buf) / 2` used above | the `=` handler computed an **empty** name when there was no `#`, defining nothing (`cmpsb.asm`) |
| `dw` symbol holding > 127 | `mov foo, 300` | an unsized symbol destination defaulted to 8-bit (`far_call-2.asm`) |
| `dd` truncation | `dd 12345678h` | high 16 bits were emitted as zero |

Now rejected with the original's wording ("undefined variable or over 16
bits", "overflow! - cannot be evaluated:"). `src/check_asmdiag.py` covers both
directions: 6 rejection checks and 7 acceptance checks that assert bytes, so
"assembles" cannot be satisfied by emitting nothing.
