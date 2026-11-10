/* mue — ALU, flags and the shared arithmetic path
 * alu.c
 *
 * All of ADD ADC SUB SBB CMP AND OR XOR TEST funnel through alu_op().
 * The original exposes this same funnel as returnALU_STATE/setALU_STATE
 * (FINDINGS sec. 4.3 / CPU_SPEC sec. 4.3).
 */
#include "cpu.h"
#include "alu.h"

/* parity of the low byte: 1 when the count of set bits is even */
int parity_even(unsigned v)
{
    v &= 0xFF;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

static void set_szp(uint16_t res, int width)
{
    cpu.flags &= ~(F_ZF | F_SF | F_PF);
    if (res == 0)                    cpu.flags |= F_ZF;
    if (width == 8) {
        if (res & 0x80)              cpu.flags |= F_SF;
    } else {
        if (res & 0x8000)            cpu.flags |= F_SF;
    }
    if (parity_even(res))            cpu.flags |= F_PF;
}

uint16_t alu_op(int op, uint16_t a, uint16_t b, int width)
{
    uint32_t mask = (width == 8) ? 0xFFu : 0xFFFFu;
    uint32_t sign = (width == 8) ? 0x80u : 0x8000u;
    uint32_t carry_in = 0;
    uint32_t full;
    uint16_t res;

    if (op == ALU_ADC || op == ALU_SBB)
        carry_in = (cpu.flags & F_CF) ? 1 : 0;

    a &= mask;
    b &= mask;

    switch (op) {
    case ALU_ADD:
    case ALU_ADC:
        full = (uint32_t)a + b + carry_in;
        res = (uint16_t)(full & mask);
        if (full > mask)             cpu.flags |=  F_CF; else cpu.flags &= ~F_CF;
        if (((a ^ res) & (b ^ res) & sign))  cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
        if ((a ^ b ^ res) & 0x10)    cpu.flags |=  F_AF; else cpu.flags &= ~F_AF;
        break;

    case ALU_SUB:
    case ALU_SBB:
    case ALU_CMP:
        full = (uint32_t)a - b - carry_in;
        res = (uint16_t)(full & mask);
        if (a < b + carry_in)        cpu.flags |=  F_CF; else cpu.flags &= ~F_CF;
        if (((a ^ b) & (a ^ res) & sign))    cpu.flags |= F_OF; else cpu.flags &= ~F_OF;
        if ((a ^ b ^ res) & 0x10)    cpu.flags |=  F_AF; else cpu.flags &= ~F_AF;
        break;

    case ALU_AND:
    case ALU_TEST:
        res = (uint16_t)(a & b);
        cpu.flags &= ~(F_CF | F_OF | F_AF);
        break;

    case ALU_OR:
        res = (uint16_t)(a | b);
        cpu.flags &= ~(F_CF | F_OF | F_AF);
        break;

    case ALU_XOR:
        res = (uint16_t)(a ^ b);
        cpu.flags &= ~(F_CF | F_OF | F_AF);
        break;

    default:
        res = (uint16_t)(a + b);
        break;
    }

    set_szp(res, width);
    return (uint16_t)(res & mask);
}

/* INC/DEC keep CF, unlike ADD/SUB.  CONFIRMED semantics for 8086. */
uint16_t alu_incdec(uint16_t a, int width, int dec)
{
    uint32_t mask = (width == 8) ? 0xFFu : 0xFFFFu;
    uint32_t sign = (width == 8) ? 0x80u : 0x8000u;
    uint16_t cf   = (uint16_t)(cpu.flags & F_CF);
    uint16_t res  = (uint16_t)((dec ? a - 1 : a + 1) & mask);

    cpu.flags &= ~(F_AF | F_OF);
    if (dec) {
        if (((a ^ 1) & (a ^ res) & sign))    cpu.flags |= F_OF;
        if (((a ^ 1 ^ res) & 0x10))          cpu.flags |= F_AF;
    } else {
        if ((~(a ^ res) & (a ^ res) & sign) == 0 &&
            (((a ^ res ^ 1) & sign) && (a & sign) == 0) ) cpu.flags |= 0;
        /* standard: OF when sign flips from 0->1 or 1->0 on increment */
        if (((a ^ res) & sign) && !(a & sign) == 0 &&
            ((a & sign) && !(res & sign))) cpu.flags |= F_OF;
        if (((a ^ 1 ^ res) & 0x10))          cpu.flags |= F_AF;
    }
    set_szp(res, width);
    cpu.flags = (uint16_t)((cpu.flags & ~F_CF) | cf);
    return res;
}

/* NEG is 0 - operand */
uint16_t alu_neg(uint16_t a, int width)
{
    uint32_t mask = (width == 8) ? 0xFFu : 0xFFFFu;
    uint16_t r = alu_op(ALU_SUB, 0, a, width);
    cpu.flags = (uint16_t)(cpu.flags | ((r & mask) ? F_CF : 0));
    return r;
}

/* multiply / divide ---------------------------------------------------- */
void alu_mul(uint16_t a, uint16_t b, int width)
{
    if (width == 8) {
        uint16_t p = (uint16_t)((a & 0xFF) * (b & 0xFF));
        cpu.r[AX] = p;
        cpu.flags &= ~(F_CF | F_OF);
        if (p & 0xFF00) cpu.flags |= F_CF | F_OF;
        cpu.flags &= ~(F_ZF | F_SF | F_PF);
        cpu.flags |= (parity_even(p) ? F_PF : 0);
    } else {
        uint32_t p = (uint32_t)a * b;
        cpu.r[AX] = (uint16_t)(p & 0xFFFF);
        cpu.r[DX] = (uint16_t)(p >> 16);
        cpu.flags &= ~(F_CF | F_OF);
        if (p & 0xFFFF0000u) cpu.flags |= F_CF | F_OF;
    }
}

void alu_imul(uint16_t a, uint16_t b, int width)
{
    if (width == 8) {
        int16_t p = (int16_t)((int8_t)(a & 0xFF)) * (int16_t)(int8_t)(b & 0xFF);
        cpu.r[AX] = (uint16_t)p;
        cpu.flags &= ~(F_CF | F_OF);
        if (p < -128 || p > 127) cpu.flags |= F_CF | F_OF;
    } else {
        int32_t p = (int32_t)(int16_t)a * (int32_t)(int16_t)b;
        cpu.r[AX] = (uint16_t)(p & 0xFFFF);
        cpu.r[DX] = (uint16_t)((p >> 16) & 0xFFFF);
        cpu.flags &= ~(F_CF | F_OF);
        if (p < -32768 || p > 32767) cpu.flags |= F_CF | F_OF;
    }
}

void alu_div(uint16_t a_hi, uint16_t a_lo, uint16_t d, int width, int sign)
{
    if (d == 0) { cpu.halted = 1; cpu.exit_code = -3; return; }

    if (width == 8) {
        if (sign) {
            int16_t num = (int16_t)a_lo;
            int16_t den = (int8_t)(d & 0xFF);
            int16_t q, r;
            if (den == 0) { cpu.halted = 1; cpu.exit_code = -3; return; }
            q = num / den; r = num % den;
            if (q < -128 || q > 127) { cpu.halted = 1; cpu.exit_code = -3; return; }
            cpu.r[AX] = (uint16_t)(((uint8_t)r << 8) | (uint8_t)q);
        } else {
            uint16_t num = a_lo;
            uint16_t q = num / (d & 0xFF);
            uint16_t r = num % (d & 0xFF);
            if (q > 0xFF) { cpu.halted = 1; cpu.exit_code = -3; return; }
            cpu.r[AX] = (uint16_t)((r << 8) | q);
        }
    } else {
        uint32_t num = ((uint32_t)a_hi << 16) | a_lo;
        if (sign) {
            int32_t sn = (int32_t)num;
            int32_t sd = (int16_t)d;
            int32_t q, r;
            if (sd == 0) { cpu.halted = 1; cpu.exit_code = -3; return; }
            q = sn / sd; r = sn % sd;
            if (q < -32768 || q > 32767) { cpu.halted = 1; cpu.exit_code = -3; return; }
            cpu.r[AX] = (uint16_t)(q & 0xFFFF);
            cpu.r[DX] = (uint16_t)(r & 0xFFFF);
        } else {
            uint32_t q, r;
            if (d == 0) { cpu.halted = 1; cpu.exit_code = -3; return; }
            q = num / d; r = num % d;
            if (q > 0xFFFF) { cpu.halted = 1; cpu.exit_code = -3; return; }
            cpu.r[AX] = (uint16_t)(q & 0xFFFF);
            cpu.r[DX] = (uint16_t)(r & 0xFFFF);
        }
    }
}

/* shifts and rotates --------------------------------------------------- */
uint16_t alu_shift(int op, uint16_t v, unsigned count, int width)
{
    uint32_t mask = (width == 8) ? 0xFFu : 0xFFFFu;
    uint32_t top  = (width == 8) ? 0x80u : 0x8000u;
    uint32_t x = v & mask;
    unsigned i;

    if (count == 0) return (uint16_t)x;
    count &= 0x1F;

    switch (op) {
    case SH_SHL:            /* /4 and /6 are both SHL on 8086 */
        for (i = 0; i < count; i++) {
            cpu.flags = (cpu.flags & ~F_CF) | ((x & top) ? F_CF : 0);
            x = (x << 1) & mask;
        }
        cpu.flags &= ~F_OF;
        if ((x & top) ? !(cpu.flags & F_CF) : (cpu.flags & F_CF)) cpu.flags |= F_OF;
        set_szp((uint16_t)x, width);
        break;

    case SH_SHR:
        for (i = 0; i < count; i++) {
            cpu.flags = (cpu.flags & ~F_CF) | ((x & 1) ? F_CF : 0);
            x >>= 1;
        }
        cpu.flags &= ~F_OF;
        if (x & top) cpu.flags |= F_OF;     /* original sign preserved */
        set_szp((uint16_t)x, width);
        break;

    case SH_SAL_MARKER: case SH_SAR:
        for (i = 0; i < count; i++) {
            cpu.flags = (cpu.flags & ~F_CF) | ((x & 1) ? F_CF : 0);
            x = (x >> 1) | (x & top);
        }
        cpu.flags &= ~F_OF;
        set_szp((uint16_t)x, width);
        break;

    case SH_ROL:
        for (i = 0; i < count; i++) {
            uint32_t c = (x & top) ? 1 : 0;
            x = ((x << 1) | c) & mask;
            cpu.flags = (cpu.flags & ~F_CF) | (c ? F_CF : 0);
        }
        cpu.flags &= ~F_OF;
        if ((x & top) ? !(cpu.flags & F_CF) : (cpu.flags & F_CF)) cpu.flags |= F_OF;
        break;

    case SH_ROR:
        for (i = 0; i < count; i++) {
            uint32_t c = x & 1;
            x = (x >> 1) | (c ? top : 0);
            cpu.flags = (cpu.flags & ~F_CF) | (c ? F_CF : 0);
        }
        cpu.flags &= ~F_OF;
        if (((x ^ (x << 1)) & top)) cpu.flags |= F_OF;
        break;

    case SH_RCL:
        for (i = 0; i < count; i++) {
            uint32_t c = (x & top) ? 1 : 0;
            x = ((x << 1) | ((cpu.flags & F_CF) ? 1 : 0)) & mask;
            cpu.flags = (cpu.flags & ~F_CF) | (c ? F_CF : 0);
        }
        break;

    case SH_RCR:
        for (i = 0; i < count; i++) {
            uint32_t c = x & 1;
            x = (x >> 1) | ((cpu.flags & F_CF) ? top : 0);
            cpu.flags = (cpu.flags & ~F_CF) | (c ? F_CF : 0);
        }
        break;

    default:
        break;
    }

    return (uint16_t)(x & mask);
}
