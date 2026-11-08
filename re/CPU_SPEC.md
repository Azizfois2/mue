# Clean-Room Specification — 8086 CPU Execution Loop

Derived from the structural findings in `FINDINGS.md`. Where the original's
behaviour is `CONFIRMED`, the spec says so; where I am designing a clean
equivalent because the original is not observable, it says `DESIGN`.

---

## 1. State model

### 1.1 Registers

A flat 16-bit register file, byte-addressable in 8086 order:

```
union reg16 { uint16_t w; struct { uint8_t l, h; } b; };
reg16 R[8];    /* AX CX DX BX SP BP SI DI   -- index == ModRM reg/rm code */
uint16_t SR[4];/* ES CS SS DS              -- index == ModRM sreg code  */
uint16_t IP;
```

CONFIRMED: the emulator stores **CS at `[obj+0xCC]`**, **IP at `[obj+0xCE]`**,
and a separately cached **physical fetch pointer at `[obj+0xD4]`**
(see FINDINGS §3.2). The 8-name order `AX CX DX BX SP BP SI DI` is the
ModRM `r/m` code order and is CONFIRMED by the EA table in FINDINGS §4.3,
which lists `[BX+SI] [BX+DI] [BP+SI] [BP+DI] [SI] [DI] [BP] [BX]` — the
8086 standard.

### 1.2 Flags

One 16-bit word, low 9 bits meaningful:

```
bit 0 CF   1 (reserved, always 1)   2 PF   3 (0)   4 AF
bit 6 ZF   7 SF   8 TF   9 IF  10 DF  11 OF
```

CONFIRMED: the emulator has dedicated flag helpers —
`"set_parity_flag: "` (rva `0x4A2E0`), `"set_flags_for_RF_command: "`, and the
flag-analyzer vocabulary lists `CF ZF SF OF AF PF IF DF TF` with their
meanings (`"    above" / "(CF=0 and ZF=0)"`, `"    less" / "(SF<>OF)"`, …).

### 1.3 Memory

```
uint8_t MEM[1 << 20];   /* 1 MiB flat physical space */
```

CONFIRMED: the emulator's addressable window is documented in its own
messages — `"free memory area is between 0050:0000h and 0a000:0000h"`
and `"file will be loaded at 0100:0000h"`. Physically that is
`0x0`–`0xA0000` usable, with video memory at `0xB8000`.

Address formation (**CONFIRMED, exactly as the original does it**):

```
phys = ((uint32_t)segment << 4) + offset ;   /* 20-bit, wraps at 1 MiB */
```

The original caches this as a running cursor `[obj+0xD4]` and `inc`s it per
byte, rather than recomputing. A clone may keep the cursor; it must recompute
whenever a segment register or IP is loaded.

---

## 2. The loop

The original's loop is method #377, entered once per step. The step is:

```
do_step():
    saved_ip = IP
    /* --- FETCH --- */
    if (pending_rep && (prefix_state & REP_MASK) && CX == 0):
        goto rep_done                       /* zero-count REP short-circuits */

    /* --- PREFIX LOOP --- */
    for (;;) {
        op = fetch8()
        switch (op) {
        case 0x26: seg_override = ES; continue;
        case 0x2E: seg_override = CS; continue;
        case 0x36: seg_override = SS; continue;
        case 0x3E: seg_override = DS; continue;
        case 0xF0: lock_prefix  = 1;  continue;   /* LOCK (prefix) */
        case 0xF2: rep_prefix = REPNE; continue;
        case 0xF3: rep_prefix = REP;   continue;
        }
        break;
    }

    /* --- DECODE + EXECUTE  (linear compare chain, see §3) --- */
    dispatch(op);

    /* --- LENGTH ACCOUNTING --- */
    instruction_length = (phys_cursor - saved_phys)      /* CONFIRMED model */
    if (trace) record(saved_ip, instruction_length, op);

    if (halted) raise(HALTED)
```

The 1-MiB wrap and the `IP = (IP + n) & 0xFFFF` post-increment are the F1/M1
8086 behaviour. The original's use of a 20-bit `jo`-guarded cursor means it
does **not** wrap the *cursor*, it wraps the *segment:offset pair*.

---

## 3. Dispatch

### 3.1 Original mechanism (CONFIRMED)

**A linear `if/elseif` compare chain over the opcode byte.** No jump table.
Evidence: zero `jmp [reg*4+disp]` encodings in 161 KB; 54 ordered
`cmp dx, imm16` sites; the opcode error string only referenced from this method.

This is worth stating explicitly because it is the opposite of what one expects
from a CPU emulator, and it has observable consequences:

* **Cost is O(opcodes-tested) per instruction** — there is no constant-time
  dispatch, and instruction order in the source changes performance.
* **Unimplemented opcodes have a single funnel** — the `else` branch of the
  chain, which is why there is exactly one `"no such opcode in 8086 instruction
  set."` site pair.
* **The chain order is the source's `Select Case` order** (FINDINGS §3.3), and
  it is *not* opcode-numeric order.

### 3.2 Clone mechanism (DESIGN)

Use a **256-entry function-pointer table** — the standard, correct approach:

```
typedef void (*handler_t)(void);
handler_t OP[256];
```

Rationale: a clone should not reproduce the original's O(n) chain. The
*observable behaviour* (which opcode reaches which semantics, and which opcodes
are rejected) is what must match, not the dispatch data structure. The rejected
set is specified in §5.

---

## 4. Instruction execution rules

### 4.1 Operand fetch

```
uint8_t  fetch8(void)   { uint8_t v = MEM[phys++]; IP++; return v; }
uint16_t fetch16(void)  { uint16_t lo = fetch8(); uint16_t hi = fetch8();
                          return lo | (hi << 8); }   /* little-endian */
int8_t   fetch_s8(void) { return (int8_t)fetch8(); }
```

**Little-endian is CONFIRMED** by the original's own disassembler output in
`MyBuild/*.list`, e.g. `mov word ptr [1400h], 1` → `C7 06 00 14 01 00`
(displacement `00 14` = `0x1400`, immediate `01 00` = `1`).

### 4.2 ModRM decode  (CONFIRMED vocabulary — FINDINGS §4.3)

```
modrm = fetch8();
mod = modrm >> 6;  reg = (modrm >> 3) & 7;  rm = modrm & 7;

if (mod == 3) { operand is REGISTER rm; }

else {
  /* 16-bit EA: [BX+SI] [BX+DI] [BP+SI] [BP+DI] [SI] [DI] [disp16|BP] [BX] */
  static const struct { int8_t base, index; } EA[8] = {
    /* 0 */ { BX, SI },   /* 1 */ { BX, DI },
    /* 2 */ { BP, SI },   /* 3 */ { BP, DI },
    /* 4 */ { SI, NONE }, /* 5 */ { DI, NONE },
    /* 6 */ { BP, NONE }, /* 7 */ { BX, NONE },
  };
  uint16_t off = 0;
  if (EA[rm].base  >= 0) off += R[EA[rm].base ].w;
  if (EA[rm].index >= 0) off += R[EA[rm].index].w;

  switch (mod) {
  case 0: if (rm == 6) { off = fetch16(); }        /* [disp16], no BP */
          break;
  case 1: off += (int8_t)fetch8();  break;         /* [x] + d8  */
  case 2: off += fetch16();         break;         /* [x] + d16 */
  }
  /* default segment: SS if BP is involved, else DS */
  seg = (EA[rm].base == BP || EA[rm].index == BP) ? SS : DS;
  seg = seg_override_any ? seg_override : seg;
}
```

The `mod==0 && rm==6` case being a bare `disp16` (with **DS**, not SS) is the
one special case in the 8086 encoding and is CONFIRMED by the presence of
`[BX] + d16`, `[BP] + d16`, `[SI] + d16`, `[DI] + d16` as *distinct* entries
alongside `[BX + SI]` (no-displacement) entries.

### 4.3 ALU core  (the shared flags path)

All of `ADD ADC SUB SBB CMP AND OR XOR TEST` funnel through one routine. The
original exposes this as **`returnALU_STATE` / `setALU_STATE`** (strings
`"Error returnALU_STATE: "`, `"Error setALU_STATE: "`) — i.e. it *saves and
restores* the ALU flags state, which is how it implements AF for `DAA`/`DAS`.

```
alu(op, a, b, width):     /* width = 8 or 16 */

    /* --- arithmetic with carry-in --- */
    carry_in = (op == ADC || op == SBB) ? CF : 0;
    full = (uint32_t)a + (uint32_t)b + carry_in;          /* ADC/ADD */
        or (uint32_t)a - (uint32_t)b - carry_in;          /* SBB/SUB/CMP */

    result = full & mask(width);

    switch (op):
      ADD, ADC:  CF = (full >> width) & 1;
                 OF = ((a ^ result) & (b ^ result) & signbit) != 0;
                 AF = ((a ^ b ^ result) & 0x10) != 0;
      SUB, SBB, CMP:
                 CF = (full >> width) & 1;     /* borrow */
                 OF = ((a ^ b) & (a ^ result) & signbit) != 0;
                 AF = ((a ^ b ^ result) & 0x10) != 0;
      AND, OR, XOR, TEST:
                 CF = 0;  OF = 0;  AF = undefined(0);
    }

    ZF = (result == 0);
    SF = (result >> (width - 1)) & 1;
    PF = parity_even(result & 0xFF);          /* low byte only, 8086 rule */
    return result;
```

`parity_even` is CONFIRMED to be a dedicated step: the original has the
labelled helper `"set_parity_flag: "` (rva `0x4A2E0`).

### 4.4 The shift/rotate family  (0xC0 / 0xC1 / 0xD0–0xD3)

The original contains explicit length-fixups `"weird.... C0 tab6"` and
`"weird.... C1 tab6"` (rva `0x48DCC`, `0x48E00`) plus dedicated methods
`do_instruction_C0`, `do_instruction_C1`, `do_instruction_C8`…`0x6F`,
`do_instruction_D8_DF`. The `C0`/`C1` strings mean the **80186 shift forms
with an immediate count** are encoded in a 6-row table (`tab6`), and the
emulator flags them as edge cases. CONFIRMED: `0xC0` and `0xC1` are in the
`cmp dx,` chain at `0x516706` (`C0`) and `0x5167B5` (`C9`).

Clone rule: on 8086 only `0xD0–0xD3` (count 1 or CL) exist; `0xC0/C1` should
raise `"not 8086 instruction - not supported yet."` to match the original's
strict reading.

### 4.5 String ops and REP

The original implements `MOVSB/W MOVSW CMPSB/W SCASB/W LODSB/W STOSB/W` with
an explicit `DF` direction test and a `REP` prefix state that is *carried
across* steps (it is the only prefix that does not complete in one step —
hence the loop's `pending_rep` check in §2).

```
if (rep_prefix && CX > 0):
    do { step_one(); if (rep_is_compare && ZF != rep_cond) break; CX--; } while (CX);
```

`REPNE`/`REPNZ` on `CMPS`/`SCAS` is the only place `ZF` gates the loop.

### 4.6 Interrupts  (CONFIRMED surface set)

On `INT n` (and hardware IRQ injection):

```
push(FLAGS); push(CS); push(IP); IF = 0; TF = 0;
IP   = MEM[n*4]     | MEM[n*4+1] << 8;
CS   = MEM[n*4+2]   | MEM[n*4+3] << 8;
```

CONFIRMED interrupt numbers actually implemented, by their message strings:

| INT | AH | Function |
|---|---|---|
| `10h` | `0` | set video mode (`"INT 10h, AH=0"`, `"unsupported video mode:"`) |
| `10h` | `13h` | write string (`"INT 10h, AH=13h - "`) |
| `10h` | `1003h` | toggle blink (`"INT 10h, AX=1003h, BL=01  - "`) |
| `11h` | — | equipment list (`"todo: INT 10h, AH=011h"` shows 10h/11h is stubbed) |
| `13h` | `0` | reset disk (`"INT 13h, AH=0"`) |
| `15h` | `0` | (`"INT 15h, AH=0"`) |
| `16h` | `0` | read key (`"INT 16h, AH=0"`) |
| `17h` | — | printer (`do_INT_17H`, `"wrong parameter for INT 17h"`) |
| `1Ah` | `0` | get tick count (`"INT 1Ah, AH=0"`) |
| `19h` | — | reboot (`"program requested reboot. the emulator halted."`) |
| `20h` | — | terminate (`"PROGRAM HAS RETURNED CONTROL"`) |
| `21h` | `0Ch, 09h, 3Dh, 3Fh, 3Eh, 47h, 48h, 49h, 4Ah, 4Eh, 4Fh, 33h` | DOS |
| `33h` | `AX=0` | mouse (`"int 33h/AX=0"`, `"not supported yet..."`) |
| `3` / `4` | — | `"INT 3"`, `"INT 4 can be customized in interrupt vector table."` |

`INT 21h` file services CONFIRMED by name: `OPEN_VIRTUAL_FILE`,
`CLOSE_VIRTUAL_FILE`, `READ_VIRTUAL_FILE`, `WRITE_VIRTUAL_FILE`,
`SEEK_VIRTUAL_FILE`, `RENAME_VIRTUAL_FILE`, `DELETE_VIRTUAL_FILE`,
`CREATE_VIRTUAL_FILE`, `CREATE_VIRTUAL_SUBDIRECTORY`,
`REMOVE_VIRTUAL_SUBDIRECTORY`, `GET_VIRTUAL_SUBDIRECTORY`,
`CHANGE_VIRTUAL_SUBDIRECTORY`, `RECORD_VIRTUAL_FILE_HANDLE`,
`CLOSE_ALL_VIRTUAL_FILES`, `set_DTA_Address`, `do_Find_first_file`,
`do_Find_next_file`, `do_get_set_file_DATE_TIME`, `do_INT_21h_48h/49h/4Ah`.
The virtual file system is rooted at `VDRIVE_PATH` (`vdrive\`).

---

## 5. Opcodes the original refuses  (CONFIRMED, exact message per class)

| Class | Message |
|---|---|
| Unknown / unassigned byte | `"no such opcode in 8086 instruction set."` |
| Unknown, skip and continue | `"unknown opcode skipped:"` |
| `C0/C1` and 80186+ forms | `"not 8086 instruction - not supported yet."` |
| FPU (`D8`–`DF`) | `"the FPU function is not supported yet"` / `"FPU ERROR: "` |
| `IN`/`OUT` to absent port | `"port not available in 8086!"` |

A clone must reproduce the *accept/reject partition*, not just the mnemonics:
accepting `*ENTER`/`*BOUND`/`*CLTS`/`*LAR`/`*LSL`/`*LTR`/`*LGDT`/`*LIDT`/
`*LLDT`/`*LMSW`/`*INS`/`*ARPL` at runtime would be a behavioural divergence,
because the original's own form table marks them `*` and the core rejects them.

---

## 6. Step / step-back (behavioural contract)

CONFIRMED string surface: `mnuStepOver_Click`, `PROCESS_SINGLE_STEP`,
`"error on single step: "`, `"break point encountered at:"`,
`"run until"`, `"set CS:IP to selected position"`, `"clear break point"`.

The engine is **step-oriented, and rewind is a first-class feature**
(`ENABLE_STEP_BACK`, `MAXIMUM_STEPS_BACK`, `DO_STEP_BACK`,
`ResetStepBackRecording`, `keep_BYTE_CHANGE_FOR_CURRENT_BACKSTEP_RECORD`).
The step-back design is **journal-based**: before each step the engine records
the *byte deltas* it is about to make, and `DO_STEP_BACK` replays them in
reverse. CONFIRMED by:
`"memory write operation exceeded "`, `"when interrupt modifies more than "`,
`"cannot step-back: "`, `"set_StepBack_for_FPU"`, `"do_FPU_StepBack"`,
`"set_bSCREEN_SCROLL_IN_STEP_FLAG"`, and the explicit limitation notices
`"byte-by-byte file write operations cannot be undone, however all subsequent
write operations are reverted when file open is undone."` and
`"screen scroll operations cannot be undone."`.

**Design consequence for a clone:** make every architectural write go through
`poke(phys, value)` so a single undo journal can revert a step. Do not write
`MEM[]` directly from handlers.
