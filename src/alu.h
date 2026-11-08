#ifndef MUE_ALU_H
#define MUE_ALU_H

#include <stdint.h>

enum {
    ALU_ADD = 0, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP,
    ALU_TEST = 100
};

/* shift/rotate operation codes, ordered by ModRM reg field (/0../7) */
enum {
    SH_ROL = 0, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL_MARKER, SH_SAR
};

int      parity_even(unsigned v);
uint16_t alu_op(int op, uint16_t a, uint16_t b, int width);
uint16_t alu_incdec(uint16_t a, int width, int dec);
uint16_t alu_neg(uint16_t a, int width);
void     alu_mul(uint16_t a, uint16_t b, int width);
void     alu_imul(uint16_t a, uint16_t b, int width);
void     alu_div(uint16_t a_hi, uint16_t a_lo, uint16_t d, int width, int sign);
uint16_t alu_shift(int op, uint16_t v, unsigned count, int width);

void     sb_begin(void);
void     sb_end(void);
int      sb_undo(void);

#endif
