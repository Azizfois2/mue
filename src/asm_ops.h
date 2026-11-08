#ifndef MUE_ASM_OPS_H
#define MUE_ASM_OPS_H

#include "asm.h"

enum { OP_NONE = 0, OP_REG8, OP_REG16, OP_SREG, OP_MEM, OP_IMM };

typedef struct {
    int          kind;
    int          reg;        /* register index */
    int          size;       /* 8, 16, 32, 64 or 0 unknown */
    int          seg;        /* segment override index, -1 = none */
    int          base;       /* base register index or -1 */
    int          index;      /* index register index or -1 */
    int          disp;
    int          has_disp;
    int          moffs;      /* direct address form */
    int          imm;
    const char  *text;
} operand_t;

int reg8_index (const char *s);
int reg16_index(const char *s);
int sreg_index (const char *s);

int parse_operand(asm_t *a, const char *text, operand_t *op);
int build_modrm (asm_t *a, const operand_t *op, int regf, int *out, int *outn);

#endif
