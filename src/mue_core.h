/* mue_core.h - drive the verified emulator core in-process.
 *
 * The UI used to shell out to the mue.exe CLI for every run and scrape its
 * register dump. That can only ever show the FINAL state of a program, so
 * "single step" and the live memory/disassembly views were impossible. The
 * core already exposes cpu_step(), MEM[] and disasm_line(), so the UI now
 * links it directly and owns the machine state between steps.
 *
 * This keeps the verified CLI untouched: the same source files are compiled
 * into both binaries, so the tests still exercise the same code.
 */
#ifndef MUE_CORE_H
#define MUE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble `src` and load it as a .COM at 0100h, resetting the CPU.
 * `src_path` is the file the source came from: its stem names the MyBuild\
 * artefacts, and it may be empty for an unsaved buffer.
 * Returns 1 on success; on failure `err` holds the assembler diagnostic. */
int core_load(const char *src, const char *src_path, char *err, int errsz);

/* Load an already-built .com/.bin from `path` straight into memory at 0100h.
 * This backs the emulator's "file/load executable..." menu item. */
int core_load_binary(const char *path, char *err, int errsz);

/* Assemble and load exactly like core_load, but write nothing: Compile shows
 * the assembler status window before emu8086's Save-As box asks where the
 * image goes, and Emulate never writes files. */
int core_assemble(const char *src, const char *src_path, char *err, int errsz);

/* Write the last assembled image and its four sidecars to `path` (the file
 * the user chose in Compile's Save-As box). Returns 0 if nothing is assembled. */
int core_save_image(const char *path);

/* 1 when the source has anything other than blank/comment lines after its
 * `end` directive - emu8086's "any code after 'END' directive is ignored." */
int core_code_after_end(const char *src);

/* Execute exactly one instruction. Returns 1 if an instruction ran, 0 when
 * the guest has halted or cannot continue. */
int core_step(void);

/* Run until halt, or until `max` instructions have executed. */
int core_run(int max);

/* Machine state, for the register and flag panels. */
void core_regs(unsigned short *r8, unsigned short *sr4,
               unsigned short *ip, unsigned short *flags, long *icount);
int  core_halted(void);
int  core_loaded(void);

/* One line of disassembly at the current CS:IP. `len` receives the number of
 * bytes the instruction occupies. Returns 1 if there was anything to show. */
int core_disasm_here(char *out, int *len);
int core_disasm_at(char *out, int *len);

/* Disassemble `rows` instructions starting at CS:IP. */
void core_disasm_list(char *out, int outsz, int rows);

/* One instruction of disassembly at an arbitrary address. */
int  core_disasm_seg(unsigned short seg, unsigned short off, char *out,
                     int *len);
int  core_insn_len_at(unsigned short seg, unsigned short off);
void core_disasm_rows(unsigned short seg, unsigned short first_off, int rows,
                      unsigned short *addrs, int *lens, char *text,
                      int tstride);

/* ---- step-back history -------------------------------------------------
 * Every single step records the CPU state plus the bytes the instruction
 * overwrote, so rewinding restores registers AND the memory the instruction
 * wrote - which is what emu8086's `step back` does. A run is not journalled
 * (a million instructions is not a history), so running clears it. */
int  core_step_back(void);
int  core_back_depth(void);
void core_hist_clear(void);

/* ---- writing state (register / memory editing) ------------------------- */
/* idx 0..7 = AX CX DX BX SP BP SI DI, 8..11 = ES CS SS DS,
 * 12 = IP, 13 = FLAGS. */
void core_write_reg(int idx, unsigned short v);
void core_write_flags(unsigned short f);
void core_write8(uint32_t phys, unsigned char v);
void core_write16(uint32_t phys, unsigned short v);

/* Text screen with attributes, for the screen window: 80x25 cells. */
void core_screen_cells(unsigned char *chars, unsigned char *attrs);
void core_screen_cursor(int *row, int *col);

uint8_t core_peek8(uint32_t phys);
uint16_t core_peek16(uint32_t phys);

/* Text screen contents, 80x25, attributes dropped. */
void core_screen_text(char *out, int outsz);

/* The last real-mode segment:offset the CPU fetched from (CS:IP). */
void core_cs_ip(unsigned short *cs, unsigned short *ip);

/* Extension of the image the last build wrote: "com", "exe" or "bin". */
const char *core_image_ext(void);

/* Bytes in the image the last build wrote. */
int core_last_size(void);

/* Name (no directory) of the image the last build wrote, e.g. "hello.com". */
const char *core_last_image_name(void);

#ifdef __cplusplus
}
#endif

#endif
