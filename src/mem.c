/* mue — memory, register access and the step-back journal
 * mem.c
 */
#include <string.h>
#include <stdlib.h>
#include "cpu.h"

cpu_t   cpu;
uint8_t MEM[1 << 20];

/* ---------------------------------------------------------------------
 * Step-back journal: every architectural byte write is recorded so a step
 * can be undone.  Mirrors the original's
 *   keep_BYTE_CHANGE_FOR_CURRENT_BACKSTEP_RECORD
 * design (FINDINGS sec. 6).
 * ------------------------------------------------------------------- */
typedef struct { uint32_t phys; uint8_t old; uint8_t is_reg; uint16_t regno; } delta_t;

#define MAX_DELTA 65536
static delta_t *journal;
static int journal_n;
static int journal_on;

void sb_begin(void)
{
    if (!journal) journal = (delta_t *)malloc(sizeof(delta_t) * MAX_DELTA);
    journal_n = 0;
    journal_on = 1;
}

void sb_end(void) { journal_on = 0; }

int sb_undo(void)
{
    int i;
    if (!journal || journal_n == 0) return 0;
    for (i = journal_n - 1; i >= 0; i--) {
        delta_t *t = &journal[i];
        if (t->is_reg) {
            if (t->regno < 8) cpu.r[t->regno] = t->old;
        } else {
            MEM[t->phys & 0xFFFFF] = t->old;
        }
    }
    journal_n = 0;
    return 1;
}

static void journal_push(uint32_t phys, uint8_t old)
{
    if (journal_on && journal && journal_n < MAX_DELTA) {
        journal[journal_n].phys = phys;
        journal[journal_n].old  = old;
        journal[journal_n].is_reg = 0;
        journal_n++;
    }
}

/* --------------------------------------------------------------------- */
uint32_t seg_off_to_phys(uint16_t seg, uint16_t off)
{
    return (((uint32_t)seg << 4) + off) & 0xFFFFF;
}

uint8_t peek8(uint32_t phys)
{
    return MEM[phys & 0xFFFFF];
}

/* 8086 word reads wrap inside a 64 KiB segment, but for a flat physical
 * pointer the wrap is at the 1 MiB boundary. */
uint16_t peek16(uint32_t phys)
{
    uint32_t a = phys & 0xFFFFF;
    return (uint16_t)(MEM[a] | (MEM[(a + 1) & 0xFFFFF] << 8));
}

uint16_t peek16s(uint32_t phys)
{
    uint32_t a = phys & 0xFFFFF;
    return (uint16_t)(MEM[a] | (MEM[(a + 1) & 0xFFFFF] << 8));
}

void poke8(uint32_t phys, uint8_t v)
{
    uint32_t a = phys & 0xFFFFF;
    journal_push(a, MEM[a]);
    MEM[a] = v;
}

void poke16(uint32_t phys, uint16_t v)
{
    poke8(phys, (uint8_t)(v & 0xFF));
    poke8(phys + 1, (uint8_t)(v >> 8));
}

/* --------------------------------------------------------------------- */
uint16_t reg_get16(int idx)
{
    return cpu.r[idx & 7];
}

void reg_set16(int idx, uint16_t v)
{
    cpu.r[idx & 7] = v;
    /* SP/BP affect the address cursor only via recompute, so no action */
}

void sr_set(int idx, uint16_t v)
{
    cpu.sr[idx & 3] = v;
}

void cpu_reset(void)
{
    memset(&cpu, 0, sizeof(cpu));
    cpu.flags = F_P1 | F_IF;
    cpu.seg_override = -1;
    cpu.sr[CS] = 0x0100;
    cpu.ip     = 0x0000;
    cpu.phys   = seg_off_to_phys(cpu.sr[CS], cpu.ip);
}
