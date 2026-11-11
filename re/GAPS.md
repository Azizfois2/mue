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
35 39 3A 3B 3C 3D 3E 3F 40 41 42 47 4C 56) are implemented. The clone has 61
arms - more than the original's 47 - so several are supersets the original
does not dispatch (`00 31 44 4B` and others). Those are harmless but are
DESIGN, not CONFIRMED.

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

---

## 5. Not yet checked at all

Honest list of surfaces with no comparison done:

1. ~~INT 1Ah AH=02~~ - **withdrawn**: the original does not dispatch AH=02,
   so there is no arm to match. Recorded here so it is not re-opened.
2. **INT 16h shift flags** at `0040:0017` and Ctrl-Break / AH=12h. The key
   *buffer* now matches (sec. 40) but the modifier byte is not modelled, so a
   guest reading shift state sees a stale zero.
3. **INT 21h AH=31h TSR** is present in the clone but the original does not
   dispatch it - unverified which is right.
4. **INT 21h AH=4Bh EXEC**. Both have an arm; I have not compared whether
   either can actually load a child.
5. **INT 20h / 4Ch exit codes**: the clone returns AL directly. Unverified
   whether the original propagates it or always reports 0.
6. **INT 10h AH=09/0A with BH != 0**: page routing is implemented but the
   attribute handling in modes other than 3 is untested.
7. **The assembler's error recovery.** All 85 examples and 46 `.list` files
   are byte-identical, but a *malformed* source (bad operand, undefined
   symbol) has never been compared against the original's diagnostics.

---

## Suggested order

1. `2Eh`/`54h` - smallest possible, matched pair, removes a spurious report.
2. `INT 10h AH=11h` - one line, turns a wrong diagnostic into a silent no-op.
3. `INT 10h AH=0C/0D` gate - decide from a run against the original.
4. `INT 21h AH=57h` - the only meaty one.
5. `17h` rename, then `0Ch`/`0Dh`, `23h`/`33h`.
6. Sec. 5 items, starting with exit codes (cheapest to verify, and the current
   `check_run.py` treats any non-zero exit as failure, so a wrong answer there
   could mask or invent a failure).
