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

# ---- the FLTK front end ------------------------------------------------
#
# The UI is a separate binary that links the same core sources as the CLI,
# compiled as C and wrapped in extern "C" (exec.c uses C compound literals
# that g++ rejects). It is not part of `all` or `test`: FLTK is a large
# optional dependency and the verified CLI must build without it.
UIBIN   := src/mueui$(EXE)
UISRC   := src/mue_ui.c src/mue_icons.c src/mue_highlight.c \
           src/mue_source.cpp src/mue_tools.c
CORE_C  := src/exec.c src/mem.c src/alu.c src/io.c src/dos.c \
           src/asm_main.c src/asm_sup.c src/asm_ops.c src/exe.c src/screen.c \
           src/mue_core.c
CXX     ?= g++
UICXXFLAGS ?= $(shell fltk-config --cxxflags 2>/dev/null)
# fltk-config --ldflags omits the COM libraries FLTK's drag-and-drop needs on
# Windows, so they are listed explicitly.
UILDFLAGS  ?= -L/ucrt64/lib -lfltk -lole32 -luuid -lcomdlg32 -lgdi32 -luser32              -lshell32 -ladvapi32 -lws2_32 -lcomctl32 -lgdiplus -lwinspool

.PHONY: all test examples clean ui

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
	@echo "== assembler encodings (no fixtures needed) =="
	@cd src && "$(PYTHON)" check_enc.py
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
	@echo "== runtime behaviour regressions =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_runtime.py
	@echo
	@echo "== every shipped example runs to completion =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_run.py
	@echo
	@echo "== INT 10h write-string services =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int10.py
	@echo
	@echo "== INT 16h keyboard buffer and scan codes =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int16.py
	@echo
	@echo "== INT 21h date and time follow the host clock =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int21time.py
	@echo
	@echo "== INT 21h/INT 10h gap regressions (re/GAPS.md) =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_gaps.py
	@echo
	@echo "== assembler diagnostics: rejected vs. accepted forms =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_asmdiag.py
	@echo
	@echo "== INT 10h subfunctions checked by side effect =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int10sem.py
	@echo
	@echo "== INT 15h AH=86h: the wait happens, and it is capped =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int15.py
	@echo
	@echo "== INT 33h mouse: per-register contract =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int33.py
	@echo
	@echo "== INT 1Ah AH=00: tick count comes from the host clock =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int1a.py
	@echo
	@echo "== INT 21h file services =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_int21.py
	@echo
	@echo "== examples/dirlist.asm (4Eh/4Fh walk) =="
	@cd src && $(SKIPOK) "$(PYTHON)" check_dirlist.py
	@echo
	@echo "all checks passed (any SKIP above means emu8086 fixtures were not found)"

# Breadth check: does every shipped example still assemble?  Kept out of
# `make test` because it reports a known non-zero failure set rather than
# asserting exact correctness; it fails only if the count drops.
examples: $(BIN)
	@cd src && $(SKIPOK) "$(PYTHON)" check_examples.py -v

# Static FLTK link, so the binary runs without the toolchain's DLLs present.
ui: $(UIBIN)

# The core is compiled as C by $(CC), because exec.c uses C compound
# literals that g++ rejects; only the UI sources go through $(CXX).
$(UIBIN): $(UISRC) $(CORE_C) $(HEADERS)
	@mkdir -p src/.uiobj
	@for f in $(CORE_C); do \
	    $(CC) -c $(CFLAGS) $(CPPFLAGS) -o src/.uiobj/`basename $$f .c`.o $$f || exit 1; \
	done
	$(CXX) -static -O2 -Wall -Wno-unused-parameter $(CPPFLAGS) \
	    -o $@ $(UISRC) src/.uiobj/*.o $(UICXXFLAGS) $(UILDFLAGS) -mwindows

clean:
	rm -f $(BIN) $(TBIN) $(UIBIN)
	rm -rf src/.uiobj

