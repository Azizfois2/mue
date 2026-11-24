# emu8086 virtual devices - reverse engineering notes

All measurements below come from fresh live captures of emu8086 4.08 on this
machine, not from the GIFs in `re/ui_ref/`. The captures were taken with
`re/uicap.py --virtual` so the coordinates are the device's own logical
pixels; the FLTK rebuild uses the same coordinates.

## 1. How emu8086 starts a device

`DEVICES\_READ_ME.txt` states the rule plainly:

* a device starts when its **file name appears anywhere in the source**,
  comments and string buffers included (the shipped examples all contain
  `#start=<name>.exe#`);
* every `*.exe` dropped into `DEVICES\` is added to the `virtual devices`
  menu at emulator start-up;
* the devices exchange bytes through a shared 64 KiB file at the root of C:,
  `C:\emu8086.io` (ports 0..65535 are bytes 0..65535 of that file) and
  `C:\emu8086.hw`.

This session runs at medium integrity, so creating/writing those two root
files is denied. Live captures of the stock `.exe`s therefore show their
form's initial state; the **protocols** below are taken from the seven
shipped examples (`examples\*.asm`) and `DEVICES\Printer.txt`, which agree
with the device code.

`mue` keeps the same port contract but drives the windows in-process:
`src/io.c` calls the hook installed by `devices_boot()` on every `IN`/`OUT`,
and a 200 ms FLTK timer advances the thermometer and robot. No external
process and no writable C: root file are needed. Devices named in the source
are opened on every assemble/load, after all device state is reset.

## 2. Port contracts

| device | ports | protocol |
|---|---|---|
| `LED_Display.exe` | 199 word | OUT 199 = low byte, OUT 200 = high byte; five digits of a signed decimal value in green seven-segment on black |
| `Traffic_Lights.exe` | 4 word | OUT 4 = low byte, OUT 5 = high byte; four groups of red/yellow/green at bit bases 0, 3, 6, 9 (`bit % 3`: 0 red, 1 yellow, 2 green); upper four bits unused/grey |
| `Stepper_motor.exe` | 7 byte | IN bit 7 = ready; OUT low nibble = coil phase; the example uses 0110, 0100, 0011, 0010 (4 half-steps) |
| `Thermometer.exe` | 125 IN, 127 OUT | IN 125 = signed temperature in C; OUT 127 != 0 switches the heater on; room temperature is 0 C |
| `Robot.exe` | 9 OUT, 10 IN, 11 IN | OUT 9 command: 1 forward, 2 left, 3 right, 4 examine, 5 lamp on, 6 lamp off. IN 10 data (0 nothing, 255 wall, 7 lamp on). IN 11 status: bit 0 new data, bit 1 busy |
| `Printer.exe` | 130 byte | OUT prints one character; >31 printed directly, 07 bell, 08 backspace, 10 line feed, 13 carriage return; port is cleared to 0 after printing |
| `Simple.exe` | 110 byte, 112 word | OUT 110/112 shows the received byte/word; IN 110/112 reads the user-typed (hex) fields; input ports refresh every 100 ms in the original |
| `VGA_STATE.exe` | none | indicator window; no port traffic |

## 3. Measured windows

Each row lists the measured **client** rectangle. A fresh live capture of the
printer form confirms the whole client is painted (no black margin around the
paper), so the rebuild fills every device client instead of leaving unused
black space. The LED display is the one intentional exception: its screen is
black because that is the simulated hardware.

| window title | client | painted panel | notes |
|---|---|---|---|
| `LED display` | 206x90 | (whole, black) | 5 digits 12x17 at x=38+24i, y=1; label `port 199 (2 bytes)` at x=36, baseline 46 |
| `Traffic Lights` | 281x320 | x 6..216, y 0..245 | road picture x=8..217/y=2..174; four green corners; register at x=20..203, y=194..231; caption baseline 245 |
| `stepper-motor on port 7` | 288x237 | 231x184 | motor plate x=70..158/y=4..92; three coil squares at x=82/109/136, y=98; `ready` at x=11/baseline 106; bits x=84, labels y=153, values y=167 |
| `thermometer & heater - io device` | 411x666 | 328x526 | tube x=20..72/y=66..344, bulb at (46,397); 120 C at y=77, 0 C at y=281; data box 192,66..317,95; control box 190,375..315,400 |
| `robot on port 9         (3 bytes)` | 372x333 | 297x261 | 9x6 grid, 32px cells, x=4+32c/y=32r; map frame ends y=186; legend icons at y=232; registers at x=183, baselines 204/225/237/249 |
| `printer emulation` | 333x425 | 265x332 | top strip y=0..31 with `new page` at x=2..83; white page x=0..263/y=36..330; text at x=6, y=51+16r |
| `simple io test` | 337x206 | 269x158 | write fields x=193..260 (y=17 and 49); read fields x=176..260 (y=105 and 133); label baselines 32/64/92/120/148 |
| `VGA STATE` | 133x16 | 5px grey strip | the caption is clipped by the tiny form; black below |

Bit registers use MSB-first labels (`FEDCBA9876543210` or `76543210`), grey
names, red `1` and blue `0`, with a form-specific pitch (8px traffic/robot/
stepper, 9px thermometer).

## 4. Robot map

`DEVICES\robot_map.dat` is 54 bytes: nine columns by six rows. Values seen in
the shipped file: `04` robot, `0A` lamp on, `07` lamp off, `0B` wall, `00`
empty. The robot example moves the robot, switches lamps and writes the map
back through the device. The rebuild reads the same file at start-up and
saves changes back to `<home>\DEVICES\robot_map.dat`.
