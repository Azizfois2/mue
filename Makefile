# mue — 8086 assembler + emulator
#
#   make            build mue.exe
#   make test       build, then run every check (the regression gate)
#   make clean      remove build output

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-unused-parameter
CPPFLAGS += -Isrc
LDFLAGS ?=

BIN     := src/mue.exe
TBIN    := src/test_cpu.exe

# core emulator: no assembler needed
CORE    := src/exec.c src/mem.c src/alu.c src/io.c src/dos.c
ASM     := src/asm_main.c src/asm_sup.c src/asm_ops.c src/exe.c

HEADERS := $(wildcard src/*.h)

.PHONY: all test clean

all: $(BIN)

$(BIN): $(CORE) $(ASM) src/main.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE) $(ASM) src/main.c

$(TBIN): $(CORE) src/test_cpu.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE) src/test_cpu.c

# The regression gate.  Each step must exit non-zero on failure, so a broken
# check stops the run instead of being buried in the output.
test: $(BIN) $(TBIN)
	@echo "== CPU self-test =="
	@cd src && ./test_cpu.exe
	@echo
	@echo "== assembler vs emu8086 .list listings =="
	@cd src && python3 check_asm.py
	@echo
	@echo "== byte round-trip vs emu8086's own binaries =="
	@cd src && python3 check_roundtrip.py
	@echo
	@echo "all checks passed"

clean:
	rm -f $(BIN) $(TBIN)

