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
 * Returns 1 on success; on failure `err` holds the assembler diagnostic. */
int core_load(const char *src, char *err, int errsz);

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

/* Memory dump for the memory panel: `rows` entries starting at `phys`.
 * Each row is written as "AAAAA: HH DDD C" style text by the caller's
 * formatter, so this only reports the bytes. */
uint8_t core_peek8(uint32_t phys);
uint16_t core_peek16(uint32_t phys);

/* Text screen contents, 80x25, attributes dropped. */
void core_screen_text(char *out, int outsz);

/* The last real-mode segment:offset the CPU fetched from (CS:IP). */
void core_cs_ip(unsigned short *cs, unsigned short *ip);

#ifdef __cplusplus
}
#endif

#endif
