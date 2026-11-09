# mue — 8086 assembler + emulator
#
#   make            build mue.exe
#   make test       build, then run every check (the regression gate)
#   make clean      remove build output

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-unused-parameter
CPPFLAGS += -Isrc
LDFLAGS ?=

# Windows names the binaries .exe; elsewhere they have no suffix.
ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

BIN     := src/mue$(EXE)
TBIN    := src/test_cpu$(EXE)

# Python is `python3` almost everywhere but plain `python` on some runners.
PYTHON  ?= $(shell command -v python3 >/dev/null 2>&1 && echo python3 || echo python)

# core emulator: no assembler needed
CORE    := src/exec.c src/mem.c src/alu.c src/io.c src/dos.c
ASM     := src/asm_main.c src/asm_sup.c src/asm_ops.c src/exe.c

HEADERS := $(wildcard src/*.h)

# Run a fixture-dependent check, treating exit 77 ("no fixtures") as a skip
# rather than a failure, so `make test` still works where emu8086 is absent.
SKIPOK = $(PYTHON) -c "import subprocess,sys; r=subprocess.run([sys.argv[1]]+sys.argv[2:]); sys.exit(0 if r.returncode==77 else r.returncode)"

.PHONY: all test examples clean

all: $(BIN)

$(BIN): $(CORE) $(ASM) src/main.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE) $(ASM) src/main.c

$(TBIN): $(CORE) src/test_cpu.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE) src/test_cpu.c

# The regression gate.  Each step must exit non-zero on failure, so a broken
# check stops the run instead of being buried in the output.
test: $(BIN) $(TBIN)
	@echo "== CPU self-test =="
	@cd src && ./test_cpu$(EXE)
	@echo
	@echo "== assembler vs emu8086 .list listings =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_asm.py
	@echo
	@echo "== byte round-trip vs emu8086's own binaries =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_roundtrip.py
	@echo
	@echo "== INT 13h floppy services =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int13.py
	@echo
	@echo "== INT 10h write-string services =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int10.py
	@echo
	@echo "all checks passed (any SKIP above means emu8086 fixtures were not found)"

# Breadth check: does every shipped example still assemble?  Kept out of
# `make test` because it reports a known non-zero failure set rather than
# asserting exact correctness; it fails only if the count drops.
examples: $(BIN)
	@cd src && $(SKIPOK) "$(PYTHON)" check_examples.py -v

clean:
	rm -f $(BIN) $(TBIN)

