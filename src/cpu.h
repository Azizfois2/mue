/* mue — clean-room 8086 assembler + emulator
 * cpu.h : architectural state and the public CPU interface
 */
#ifndef MUE_CPU_H
#define MUE_CPU_H

#include <stdint.h>
#include <stdio.h>

/* ---- flags ---------------------------------------------------------- */
enum {
    F_CF = 0x0001,
    F_P1 = 0x0002,          /* always 1 on real 8086 */
    F_PF = 0x0004,
    F_AF = 0x0010,
    F_ZF = 0x0040,
    F_SF = 0x0080,
    F_TF = 0x0100,
    F_IF = 0x0200,
    F_DF = 0x0400,
    F_OF = 0x0800
};

/* ---- register codes ------------------------------------------------- */
enum { AX = 0, CX, DX, BX, SP, BP, SI, DI };
enum { ES = 0, CS, SS, DS };

typedef struct {
    uint16_t r[8];          /* AX CX DX BX SP BP SI DI */
    uint16_t sr[4];         /* ES CS SS DS */
    uint16_t ip;
    uint16_t flags;

    /* fetch cursor: phys == (seg<<4)+off, kept as a running pointer */
    uint32_t phys;
    uint32_t cur_seg;       /* segment register in effect for the fetch */
    int      seg_override;  /* -1, or ES..DS */

    /* execution control */
    int      halted;
    int      exit_code;
    long     icount;

    /* pending prefix state (carried across steps for REP) */
    int      rep_kind;      /* 0 none, 1 REP/REPE, 2 REPNE */
    int      lock;

    /* last step, for tracing / step-back */
    uint16_t last_ip, last_cs;
    int      last_len;
    uint8_t  last_op;
} cpu_t;

extern cpu_t cpu;
extern uint8_t MEM[1 << 20];

/* ---- memory access --------------------------------------------------
 * All architectural writes MUST go through poke8/poke16 so that the
 * step-back journal can revert a step (see FINDINGS sec. 6).
 */
uint8_t  peek8 (uint32_t phys);
uint16_t peek16(uint32_t phys);
uint16_t peek16s(uint32_t phys);          /* 8086 wrap at 64K: off+1 wraps  */
void     poke8 (uint32_t phys, uint8_t v);
void     poke16(uint32_t phys, uint16_t v);

/* ---- register helpers ----------------------------------------------- */
void     reg_set16(int idx, uint16_t v);  /* keeps phys cursor in sync */
uint16_t reg_get16(int idx);
void     sr_set(int idx, uint16_t v);

/* ---- cpu ------------------------------------------------------------ */
void cpu_reset(void);
int  cpu_step(void);                      /* 1 = ran one instruction */
int  cpu_run(int max_steps);              /* <0 = until halt */

uint32_t seg_off_to_phys(uint16_t seg, uint16_t off);

/* ---- disassembler / trace ------------------------------------------- */
void disasm_line(uint32_t phys, char *out, int *len);

#endif
