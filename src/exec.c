/* mue — 8086 fetch / decode / execute loop
 * exec.c
 *
 * Dispatch is a 256-entry handler table (CPU_SPEC sec. 3.2).  The original
 * uses a linear compare chain; behaviour, not data structure, is matched.
 */
#include <string.h>
#include <stdio.h>
#include "cpu.h"
#include "alu.h"
#include "ioport.h"
#include "dos.h"

/* ---- fetch ---------------------------------------------------------- */
static uint8_t fetch8(void)
{
    uint8_t v = peek8(cpu.phys);
    cpu.phys++;
    cpu.ip++;
    return v;
}

static uint16_t fetch16(void)
{
    uint16_t lo = fetch8();
    uint16_t hi = fetch8();
    return (uint16_t)(lo | (hi << 8));
}

static int8_t fetch_s8(void) { return (int8_t)fetch8(); }

/* ---- operand plumbing ----------------------------------------------- */
typedef struct {
    int      is_reg;
    int      reg;        /* 0..7 */
    uint32_t phys;       /* memory operand physical address */
    int      width;      /* 8 or 16 */
} op_t;

static int rm_decode(int mod, int rm, uint32_t *out)
{
    static const int8_t basetab[8] = { BX, BX, BP, BP, SI, DI, BP, BX };
    static const int8_t idxtab [8] = { SI, DI, SI, DI, -1, -1, -1, -1 };
    uint16_t off;
    int uses_bp;

    if (mod == 3) return -1;            /* caller handles register form */

    off = 0;
    if (mod == 0 && rm == 6) {
        /* bare [disp16]: direct address, defaults to DS (NOT SS) */
        off = fetch16();
        uses_bp = 0;
    } else {
        if (basetab[rm] >= 0) off = (uint16_t)(off + cpu.r[basetab[rm]]);
        if (idxtab [rm] >= 0) off = (uint16_t)(off + cpu.r[idxtab [rm]]);
        uses_bp = (basetab[rm] == BP);

        if (mod == 1)      off = (uint16_t)(off + (int8_t)fetch8());
        else if (mod == 2) off = (uint16_t)(off + fetch16());
    }

    if (cpu.seg_override >= 0)
        *out = seg_off_to_phys(cpu.sr[cpu.seg_override], off);
    else
        *out = seg_off_to_phys(cpu.sr[uses_bp ? SS : DS], off);
    return 0;
}

/* Decode a ModRM byte and resolve the r/m operand. reg = 0..7. */
static void modrm(int width, op_t *rm, int *reg)
{
    uint8_t b = fetch8();
    int mod = b >> 6, r = (b >> 3) & 7, m = b & 7;

    *reg = r;
    rm->width = width;
    if (mod == 3) {
        rm->is_reg = 1;
        rm->reg = m;
    } else {
        rm->is_reg = 0;
        rm_decode(mod, m, &rm->phys);
    }
}

static uint16_t rd(const op_t *o)
{
    if (o->is_reg) {
        if (o->width == 8)
            /* 0..3 = AL,CL,DL,BL ; 4..7 = AH,CH,DH,BH */
            return (o->reg & 4) ? (cpu.r[o->reg & 3] >> 8)
                                : (cpu.r[o->reg & 3] & 0xFF);
        return cpu.r[o->reg];
    }
    return (o->width == 8) ? peek8(o->phys) : peek16(o->phys);
}

static void wr(const op_t *o, uint16_t v)
{
    if (o->is_reg) {
        if (o->width == 8) {
            int i = o->reg & 3;             /* AL,CL,DL,BL / AH,CH,DH,BH */
            if (o->reg & 4) cpu.r[i] = (uint16_t)((cpu.r[i] & 0x00FF) | ((v & 0xFF) << 8));
            else            cpu.r[i] = (uint16_t)((cpu.r[i] & 0xFF00) | (v & 0xFF));
        } else {
            cpu.r[o->reg] = v;
        }
        return;
    }
    if (o->width == 8) poke8(o->phys, (uint8_t)v);
    else               poke16(o->phys, v);
}

static op_t mkreg(int reg, int width)
{
    op_t o;
    o.is_reg = 1; o.reg = reg; o.width = width; o.phys = 0;
    return o;
}

/* ---- stack ---------------------------------------------------------- */
static void push16(uint16_t v)
{
    cpu.r[SP] = (uint16_t)(cpu.r[SP] - 2);
    poke16(seg_off_to_phys(cpu.sr[SS], cpu.r[SP]), v);
}

static uint16_t pop16(void)
{
    uint16_t v = peek16(seg_off_to_phys(cpu.sr[SS], cpu.r[SP]));
    cpu.r[SP] = (uint16_t)(cpu.r[SP] + 2);
    return v;
}

/* ---- control transfer ------------------------------------------------ */
static void set_cs_ip(uint16_t cs, uint16_t ip)
{
    cpu.sr[CS] = cs;
    cpu.ip     = ip;
    cpu.phys   = seg_off_to_phys(cs, ip);
}

static int cond(int cc)
{
    uint16_t f = cpu.flags;
    switch (cc) {
    case 0x0: return  (f & F_OF);                                   /* O  */
    case 0x1: return !(f & F_OF);                                   /* NO */
    case 0x2: return  (f & F_CF);                                   /* B  */
    case 0x3: return !(f & F_CF);                                   /* AE */
    case 0x4: return  (f & F_ZF);                                   /* E  */
    case 0x5: return !(f & F_ZF);                                   /* NE */
    case 0x6: return  (f & F_CF) || (f & F_ZF);                     /* BE */
    case 0x7: return !(f & F_CF) && !(f & F_ZF);                    /* A  */
    case 0x8: return  (f & F_SF);                                   /* S  */
    case 0x9: return !(f & F_SF);                                   /* NS */
    case 0xA: return  (f & F_PF);                                   /* P  */
    case 0xB: return !(f & F_PF);                                   /* NP */
    case 0xC: return ((f & F_SF) != 0) != ((f & F_OF) != 0);        /* L  */
    case 0xD: return ((f & F_SF) != 0) == ((f & F_OF) != 0);        /* GE */
    case 0xE: return  (f & F_ZF) ||
                     (((f & F_SF) != 0) != ((f & F_OF) != 0));      /* LE */
    default:  return !(f & F_ZF) &&
                     (((f & F_SF) != 0) == ((f & F_OF) != 0));      /* G  */
    }
}

/* ---- interrupt ------------------------------------------------------ */
void cpu_int(int n)
{
    uint16_t f = cpu.flags;

    /* services the emulator provides; unrouted vectors fall through to the
     * guest's own interrupt vector table */
    if (dos_interrupt(n)) {
        if (cpu.halted) return;
        return;
    }

    push16(f);
    push16(cpu.sr[CS]);
    push16(cpu.ip);
    cpu.flags &= ~(F_IF | F_TF);
    {
        uint32_t v = (uint32_t)(n & 0xFF) * 4;
        set_cs_ip(peek16(v + 2), peek16(v));
    }
}

/* ---- string ops ----------------------------------------------------- */
enum { S_MOVSB, S_MOVSW, S_CMPSB, S_CMPSW, S_STOSB, S_STOSW,
       S_LODSB, S_LODSW, S_SCASB, S_SCASW };

static void str_op(int kind)
{
    int w    = (kind == S_MOVSW || kind == S_CMPSW || kind == S_STOSW ||
                kind == S_LODSW || kind == S_SCASW) ? 16 : 8;
    int df   = (cpu.flags & F_DF) ? -1 : 1;
    uint32_t src = seg_off_to_phys(
        cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS],
        cpu.r[SI]);
    uint32_t dst = seg_off_to_phys(cpu.sr[ES], cpu.r[DI]);
    int step = (w / 8) * df;

    switch (kind) {
    case S_MOVSB: case S_MOVSW:
        if (w == 8) poke8(dst, peek8(src));
        else        poke16(dst, peek16(src));
        cpu.r[SI] = (uint16_t)(cpu.r[SI] + step);
        cpu.r[DI] = (uint16_t)(cpu.r[DI] + step);
        break;
    case S_CMPSB: case S_CMPSW:
        alu_op(ALU_CMP, w == 8 ? peek8(src) : peek16(src),
               w == 8 ? peek8(dst) : peek16(dst), w);
        cpu.r[SI] = (uint16_t)(cpu.r[SI] + step);
        cpu.r[DI] = (uint16_t)(cpu.r[DI] + step);
        break;
    case S_STOSB: case S_STOSW:
        if (w == 8) poke8(dst, (uint8_t)cpu.r[AX]);
        else        poke16(dst, cpu.r[AX]);
        cpu.r[DI] = (uint16_t)(cpu.r[DI] + step);
        break;
    case S_LODSB: case S_LODSW:
        cpu.r[AX] = (w == 8) ? (uint16_t)((cpu.r[AX] & 0xFF00) | peek8(src))
                             : peek16(src);
        cpu.r[SI] = (uint16_t)(cpu.r[SI] + step);
        break;
    case S_SCASB: case S_SCASW:
        alu_op(ALU_CMP, (uint16_t)(w == 8 ? (cpu.r[AX] & 0xFF) : cpu.r[AX]),
               w == 8 ? peek8(dst) : peek16(dst), w);
        cpu.r[DI] = (uint16_t)(cpu.r[DI] + step);
        break;
    }
}

/* =====================================================================
 * Opcode handlers
 * ===================================================================*/

/* ---- helpers for the common shapes ---- */
static void do_alu_rm_r(int op, int width)
{
    op_t rm; int reg; uint16_t a, b;
    modrm(width, &rm, &reg);
    a = rd(&rm);
    b = rd(&(op_t){ .is_reg = 1, .reg = reg, .width = width });
    a = alu_op(op, a, b, width);
    if (op == ALU_CMP || op == ALU_TEST) return;   /* flags only */
    wr(&rm, a);
}

static void do_alu_r_rm(int op, int width)
{
    op_t rm; int reg; uint16_t a, b;
    modrm(width, &rm, &reg);
    b = rd(&rm);
    a = rd(&(op_t){ .is_reg = 1, .reg = reg, .width = width });
    a = alu_op(op, a, b, width);
    if (op == ALU_CMP || op == ALU_TEST) return;   /* flags only */
    {
        op_t dst = mkreg(reg, width);
        wr(&dst, a);
    }
}

static void do_alu_acc_imm(int op, int width)
{
    uint16_t imm = (width == 8) ? fetch8() : fetch16();
    uint16_t a   = (width == 8) ? (cpu.r[AX] & 0xFF) : cpu.r[AX];
    uint16_t r   = alu_op(op, a, imm, width);
    if (op == ALU_CMP || op == ALU_TEST) return;   /* flags only */
    if (width == 8) cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | (r & 0xFF));
    else            cpu.r[AX] = r;
}

/* group 1 : 80/81/82/83 -- ALU rm,imm */
static void grp1(int width, int signext)
{
    op_t rm; int reg; uint16_t imm;
    modrm(width, &rm, &reg);
    if (width == 16)
        imm = signext ? (uint16_t)(int16_t)fetch_s8() : fetch16();
    else
        imm = fetch8();
    imm = alu_op(reg, rd(&rm), imm, width);
    if (reg == ALU_CMP) return;                    /* /7 CMP: flags only */
    wr(&rm, imm);
}

/* group 2 : shifts/rotates */
static void grp2(int width, int bycl, int imm8)
{
    op_t rm; int reg; unsigned count;
    modrm(width, &rm, &reg);
    if (imm8)       count = fetch8();
    else if (bycl)  count = (unsigned)(cpu.r[CX] & 0xFF);
    else            count = 1;
    wr(&rm, alu_shift(reg, rd(&rm), count, width));
}

/* group 3 : F6/F7 */
static void grp3(int width)
{
    op_t rm; int reg;
    modrm(width, &rm, &reg);
    switch (reg) {
    case 0: {   /* TEST */
        uint16_t imm = (width == 8) ? fetch8() : fetch16();
        alu_op(ALU_TEST, rd(&rm), imm, width);
        break; }
    case 1: alu_op(ALU_TEST, rd(&rm), rd(&rm), width); break;  /* not defined */
    case 2: wr(&rm, (uint16_t)~rd(&rm)); break;                /* NOT */
    case 3: wr(&rm, alu_neg(rd(&rm), width)); break;           /* NEG */
    case 4: alu_mul(cpu.r[AX], rd(&rm), width); break;         /* MUL */
    case 5: alu_imul(cpu.r[AX], rd(&rm), width); break;        /* IMUL */
    case 6: alu_div(cpu.r[DX], cpu.r[AX], rd(&rm), width, 0); break;
    case 7: alu_div(cpu.r[DX], cpu.r[AX], rd(&rm), width, 1); break;
    }
}

/* group 4 : INC/DEC rm8 */
static void grp4(void)
{
    op_t rm; int reg;
    modrm(8, &rm, &reg);
    wr(&rm, alu_incdec(rd(&rm), 8, reg >= 1));
}

/* group 5 : INC/DEC rm16, CALL, JMP, PUSH */
static void grp5(void)
{
    op_t rm; int reg;
    modrm(16, &rm, &reg);
    switch (reg) {
    case 0: wr(&rm, alu_incdec(rd(&rm), 16, 0)); break;
    case 1: wr(&rm, alu_incdec(rd(&rm), 16, 1)); break;
    case 2: {                                   /* CALL r/m16 near */
        uint16_t t = rd(&rm);
        push16(cpu.ip);
        cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t);
        break; }
    case 3: {                                   /* CALL far m16:16 */
        uint32_t p = rm.is_reg ? 0 : rm.phys;
        uint16_t off, seg;
        if (rm.is_reg) { off = cpu.r[rm.reg]; seg = cpu.sr[CS]; }
        else { off = peek16(p); seg = peek16(p + 2); }
        push16(cpu.sr[CS]);
        push16(cpu.ip);
        set_cs_ip(seg, off);
        break; }
    case 4: {                                   /* JMP r/m16 */
        uint16_t t = rd(&rm);
        cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t);
        break; }
    case 5: {                                   /* JMP far */
        uint32_t p = rm.is_reg ? 0 : rm.phys;
        uint16_t off, seg;
        if (rm.is_reg) { off = cpu.r[rm.reg]; seg = cpu.sr[CS]; }
        else { off = peek16(p); seg = peek16(p + 2); }
        set_cs_ip(seg, off);
        break; }
    case 6: push16(rd(&rm)); break;
    default: break;
    }
}

/* ---- the table ------------------------------------------------------ */
typedef void (*h_t)(void);
static h_t OP[256];

#define H(n, ...) static void h_##n(void) { __VA_ARGS__; }
#define EMPTY(n)   static void h_##n(void) { cpu.halted = 1; cpu.exit_code = -2; }

/* 00-3F : the eight ALU operations in their six standard shapes */
#define ALU_BLOCK(base, op) \
    H(a##base##0, do_alu_rm_r(op, 8)); \
    H(a##base##1, do_alu_rm_r(op, 16)); \
    H(a##base##2, do_alu_r_rm(op, 8)); \
    H(a##base##3, do_alu_r_rm(op, 16)); \
    H(a##base##4, do_alu_acc_imm(op, 8)); \
    H(a##base##5, do_alu_acc_imm(op, 16));

ALU_BLOCK(00, ALU_ADD)
ALU_BLOCK(08, ALU_OR)
ALU_BLOCK(10, ALU_ADC)
ALU_BLOCK(18, ALU_SBB)
ALU_BLOCK(20, ALU_AND)
ALU_BLOCK(28, ALU_SUB)
ALU_BLOCK(30, ALU_XOR)
ALU_BLOCK(38, ALU_CMP)

/* 40-4F INC/DEC r16  -- range-addressed in the original, so handlers are
 * generated per register but wired as contiguous blocks in the table. */

#define INCDEC(v)    alu_incdec((v), 16, 0)
#define DECREMENT(v) alu_incdec((v), 16, 1)

/* 40-4F INC r16 */
H(r400, cpu.r[AX] = INCDEC(cpu.r[AX]));
H(r401, cpu.r[CX] = INCDEC(cpu.r[CX]));
H(r402, cpu.r[DX] = INCDEC(cpu.r[DX]));
H(r403, cpu.r[BX] = INCDEC(cpu.r[BX]));
H(r404, cpu.r[SP] = INCDEC(cpu.r[SP]));
H(r405, cpu.r[BP] = INCDEC(cpu.r[BP]));
H(r406, cpu.r[SI] = INCDEC(cpu.r[SI]));
H(r407, cpu.r[DI] = INCDEC(cpu.r[DI]));

/* 48-4F DEC r16 */
H(r500, cpu.r[AX] = DECREMENT(cpu.r[AX]));
H(r501, cpu.r[CX] = DECREMENT(cpu.r[CX]));
H(r502, cpu.r[DX] = DECREMENT(cpu.r[DX]));
H(r503, cpu.r[BX] = DECREMENT(cpu.r[BX]));
H(r504, cpu.r[SP] = DECREMENT(cpu.r[SP]));
H(r505, cpu.r[BP] = DECREMENT(cpu.r[BP]));
H(r506, cpu.r[SI] = DECREMENT(cpu.r[SI]));
H(r507, cpu.r[DI] = DECREMENT(cpu.r[DI]));

/* 50-5F PUSH/POP r16 */
static void push_r16(int r) { push16(cpu.r[r]); }
static void pop_r16(int r)  { cpu.r[r] = pop16(); }

H(ps0, push_r16(AX)); H(ps1, push_r16(CX)); H(ps2, push_r16(DX)); H(ps3, push_r16(BX));
H(ps4, push_r16(SP)); H(ps5, push_r16(BP)); H(ps6, push_r16(SI)); H(ps7, push_r16(DI));
H(po0, pop_r16(AX));  H(po1, pop_r16(CX));  H(po2, pop_r16(DX));  H(po3, pop_r16(BX));
H(po4, pop_r16(SP));  H(po5, pop_r16(BP));  H(po6, pop_r16(SI));  H(po7, pop_r16(DI));

/* 06/07/0E/16/17/1E/1F PUSH/POP segment register.  The original tests these
 * as individual opcodes (0x5022C4 = 06, 0x5022FD = 16, 0x502337 = 1F) and
 * routes them through the same push16/pop16 slots as the general forms
 * (FINDINGS 16). */
static void push_sr(int s) { push16(cpu.sr[s]); }
static void pop_sr (int s) { sr_set(s, pop16()); }
H(s06, push_sr(ES));
H(s07, pop_sr(ES));
H(s0e, push_sr(CS));
H(s16, push_sr(SS));
H(s17, pop_sr(SS));
H(s1e, push_sr(DS));
H(s1f, pop_sr(DS));
/* 0F is POP CS on the 8086 (it is not a prefix there), so the fetch pointer
 * has to follow the new CS, like every other control transfer. */
H(s0f, { sr_set(CS, pop16()); cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });

/* 60-6F : 80186+ forms, rejected to match the original */
#define REJECT_80186(n) static void h_##n(void) { cpu.halted = 1; cpu.exit_code = -2; }
REJECT_80186(60) REJECT_80186(61) REJECT_80186(62) REJECT_80186(63)
REJECT_80186(64) REJECT_80186(65) REJECT_80186(66) REJECT_80186(67)
REJECT_80186(68) REJECT_80186(69) REJECT_80186(6a) REJECT_80186(6b)
REJECT_80186(6c) REJECT_80186(6d) REJECT_80186(6e) REJECT_80186(6f)

/* 70-7F : short conditional jumps */
#define JCC(n, cc) static void h_j##n(void) { \
    int8_t d = fetch_s8(); \
    uint16_t t = (uint16_t)(cpu.ip + d); \
    if (cond(cc)) { cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); } }

JCC(70,0x0) JCC(71,0x1) JCC(72,0x2) JCC(73,0x3)
JCC(74,0x4) JCC(75,0x5) JCC(76,0x6) JCC(77,0x7)
JCC(78,0x8) JCC(79,0x9) JCC(7a,0xA) JCC(7b,0xB)
JCC(7c,0xC) JCC(7d,0xD) JCC(7e,0xE) JCC(7f,0xF)

/* 80-83 group 1 */
H(g80, grp1(8, 0));
H(g81, grp1(16, 0));
H(g82, grp1(8, 0));
H(g83, grp1(16, 1));

/* 84/85 TEST */
H(t84, do_alu_rm_r(ALU_TEST, 8));
H(t85, do_alu_rm_r(ALU_TEST, 16));

/* 86/87 XCHG */
H(x86, { op_t rm; int reg; uint16_t a,b; modrm(8,&rm,&reg);
         a = rd(&rm); b = (uint16_t)((reg & 1) ? (cpu.r[reg>>1]>>8) : (cpu.r[reg>>1]&0xFF));
         wr(&rm, b); { op_t ro = mkreg(reg,8); wr(&ro, a); } });
H(x87, { op_t rm; int reg; uint16_t a,b; modrm(16,&rm,&reg);
         a = rd(&rm); b = cpu.r[reg];
         wr(&rm, b); cpu.r[reg] = a; });

/* 88-8B MOV */
H(m88, { op_t rm; int reg; modrm(8,&rm,&reg);
         wr(&rm, rd(&(op_t){.is_reg=1,.reg=reg,.width=8})); });
H(m89, { op_t rm; int reg; modrm(16,&rm,&reg);
         wr(&rm, cpu.r[reg]); });
H(m8a, { op_t rm; int reg; modrm(8,&rm,&reg);
         { op_t d = mkreg(reg,8); wr(&d, rd(&rm)); } });
H(m8b, { op_t rm; int reg; modrm(16,&rm,&reg);
         cpu.r[reg] = rd(&rm); });

/* 8C MOV rm16,sreg   /   8E MOV sreg,rm16 */
H(m8c, { op_t rm; int reg; modrm(16,&rm,&reg);
         wr(&rm, cpu.sr[reg & 3]); });
H(m8e, { op_t rm; int reg; modrm(16,&rm,&reg);
         uint16_t v = rd(&rm);
         if ((reg & 3) == CS) { cpu.halted = 1; cpu.exit_code = -2; return; }
         cpu.sr[reg & 3] = v;
         if ((reg & 3) == SS || (reg & 3) == DS || (reg & 3) == ES) {}
       });

/* 8D LEA */
H(m8d, { uint8_t b = fetch8(); int mod = b>>6, reg = (b>>3)&7, m = b&7;
         if (mod == 3) { cpu.halted = 1; cpu.exit_code = -2; return; }
         {
           static const int8_t basetab[8] = { BX,BX,BP,BP,SI,DI,BP,BX };
           static const int8_t idxtab [8] = { SI,DI,SI,DI,-1,-1,-1,-1 };
           uint16_t off = 0;
           if (basetab[m] >= 0) off = (uint16_t)(off + cpu.r[basetab[m]]);
           if (idxtab [m] >= 0) off = (uint16_t)(off + cpu.r[idxtab [m]]);
           if (mod == 0) { if (m == 6) off = fetch16(); }
           else if (mod == 1) off = (uint16_t)(off + fetch_s8());
           else off = (uint16_t)(off + fetch16());
           cpu.r[reg] = off;
         } });

/* 8F POP rm16 */
H(m8f, { op_t rm; int reg; modrm(16,&rm,&reg); (void)reg; wr(&rm, pop16()); });

/* 90-9F */
H(n90, (void)0);                                     /* NOP */
H(n91, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[CX]; cpu.r[CX] = t; });
H(n92, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[DX]; cpu.r[DX] = t; });
H(n93, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[BX]; cpu.r[BX] = t; });
H(n94, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[SP]; cpu.r[SP] = t; });
H(n95, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[BP]; cpu.r[BP] = t; });
H(n96, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[SI]; cpu.r[SI] = t; });
H(n97, { uint16_t t = cpu.r[AX]; cpu.r[AX] = cpu.r[DI]; cpu.r[DI] = t; });
H(n98, cpu.r[AX] = (uint16_t)(int16_t)(int8_t)(cpu.r[AX] & 0xFF));  /* CBW */
H(n99, cpu.r[DX] = (cpu.r[AX] & 0x8000) ? 0xFFFF : 0x0000);         /* CWD */
H(n9a, {                                           /* CALL far ptr16:16 */
        uint16_t off = fetch16(), seg = fetch16();
        push16(cpu.sr[CS]); push16(cpu.ip);
        set_cs_ip(seg, off); });
H(n9b, (void)0);                                   /* WAIT */
H(n9c, push16(cpu.flags));                          /* PUSHF */
H(n9d, { uint16_t v = pop16();                      /* POPF */
         cpu.flags = (uint16_t)((v & ~F_P1) | F_P1); });
H(n9e, { uint8_t ah = (uint8_t)(cpu.r[AX] >> 8);    /* SAHF */
         cpu.flags = (uint16_t)((cpu.flags & 0xFF00) | (ah & 0xD5) | F_P1); });
H(n9f, { cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0x00FF) |  /* LAHF */
         ((cpu.flags & 0xD5) | F_P1) << 8); });

/* A0-A3 moffs moves */
H(nA0, { uint16_t off = fetch16();
         uint32_t p = seg_off_to_phys(cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS], off);
         cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | peek8(p)); });
H(nA1, { uint16_t off = fetch16();
         uint32_t p = seg_off_to_phys(cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS], off);
         cpu.r[AX] = peek16(p); });
H(nA2, { uint16_t off = fetch16();
         uint32_t p = seg_off_to_phys(cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS], off);
         poke8(p, (uint8_t)cpu.r[AX]); });
H(nA3, { uint16_t off = fetch16();
         uint32_t p = seg_off_to_phys(cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS], off);
         poke16(p, cpu.r[AX]); });

/* A4-AF string ops */
H(nA4, str_op(S_MOVSB));
H(nA5, str_op(S_MOVSW));
H(nA6, str_op(S_CMPSB));
H(nA7, str_op(S_CMPSW));
H(nA8, alu_op(ALU_TEST, cpu.r[AX] & 0xFF, fetch8(), 8));    /* TEST AL,ib */
H(nA9, alu_op(ALU_TEST, cpu.r[AX], fetch16(), 16));        /* TEST AX,iw */
H(nAA, str_op(S_STOSB));
H(nAB, str_op(S_STOSW));
H(nAC, str_op(S_LODSB));
H(nAD, str_op(S_LODSW));
H(nAE, str_op(S_SCASB));
H(nAF, str_op(S_SCASW));

/* B0-BF MOV reg,imm  (B0..B7 = AL,CL,DL,BL,AH,CH,DH,BH) */
H(b0, cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | fetch8()));   /* AL */
H(b1, cpu.r[CX] = (uint16_t)((cpu.r[CX] & 0xFF00) | fetch8()));   /* CL */
H(b2, cpu.r[DX] = (uint16_t)((cpu.r[DX] & 0xFF00) | fetch8()));   /* DL */
H(b3, cpu.r[BX] = (uint16_t)((cpu.r[BX] & 0xFF00) | fetch8()));   /* BL */
H(b4, cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0x00FF) | (fetch8() << 8))); /* AH */
H(b5, cpu.r[CX] = (uint16_t)((cpu.r[CX] & 0x00FF) | (fetch8() << 8))); /* CH */
H(b6, cpu.r[DX] = (uint16_t)((cpu.r[DX] & 0x00FF) | (fetch8() << 8))); /* DH */
H(b7, cpu.r[BX] = (uint16_t)((cpu.r[BX] & 0x00FF) | (fetch8() << 8))); /* BH */
H(b8_, cpu.r[AX] = fetch16());
H(b9_, cpu.r[CX] = fetch16());
H(ba_, cpu.r[DX] = fetch16());
H(bb_, cpu.r[BX] = fetch16());
H(bc_, cpu.r[SP] = fetch16());
H(bd_, cpu.r[BP] = fetch16());
H(be_, cpu.r[SI] = fetch16());
H(bf_, cpu.r[DI] = fetch16());

/* C0-C1 80186 shift-with-immediate: rejected (FINDINGS sec. 5) */
REJECT_80186(c0)
REJECT_80186(c1)

/* C2/C3 RET */
H(nC2, { uint16_t n = fetch16(); cpu.ip = pop16(); cpu.r[SP] = (uint16_t)(cpu.r[SP] + n);
         /* a return to CS:0000 ends a .COM program, as in emu8086 */
         if (cpu.ip == 0) { cpu.halted = 1; return; }
         cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });
H(nC3, { cpu.ip = pop16();
         if (cpu.ip == 0) { cpu.halted = 1; return; }
         cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });

/* C4/C5 LES/LDS */
H(nC4, { op_t rm; int reg; modrm(16,&rm,&reg);
         if (rm.is_reg) { cpu.halted=1; cpu.exit_code=-2; return; }
         cpu.r[reg] = peek16(rm.phys);
         cpu.sr[ES] = peek16(rm.phys + 2);
         { uint16_t cs = cpu.sr[CS], ip = cpu.ip; (void)cs; (void)ip; } });
H(nC5, { op_t rm; int reg; modrm(16,&rm,&reg);
         if (rm.is_reg) { cpu.halted=1; cpu.exit_code=-2; return; }
         cpu.r[reg] = peek16(rm.phys);
         cpu.sr[DS] = peek16(rm.phys + 2); });

/* C6/C7 MOV rm,imm -- the original has dedicated length checks for these */
H(nC6, { op_t rm; int reg; modrm(8,&rm,&reg);
         wr(&rm, fetch8()); });
H(nC7, { op_t rm; int reg; modrm(16,&rm,&reg);
         wr(&rm, fetch16()); });

/* C8/C9 80186 ENTER/LEAVE: rejected */
REJECT_80186(c8)
REJECT_80186(c9)

/* CA/CB RETF */
H(nCA, { uint16_t n = fetch16(); cpu.ip = pop16(); cpu.sr[CS] = pop16();
         cpu.r[SP] = (uint16_t)(cpu.r[SP] + n);
         cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });
H(nCB, { cpu.ip = pop16(); cpu.sr[CS] = pop16();
         cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });

/* CC-CD INT */
H(nCC, cpu_int(3));
H(nCD, cpu_int(fetch8()));

/* CE INTO */
H(nCE, { if (cpu.flags & F_OF) cpu_int(4); });

/* CF IRET */
H(nCF, { cpu.ip = pop16(); cpu.sr[CS] = pop16(); cpu.flags = (uint16_t)((pop16() & ~F_P1) | F_P1);
         cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip); });

/* D0-D3 shifts */
H(nD0, grp2(8, 0, 0));
H(nD1, grp2(16, 0, 0));
H(nD2, grp2(8, 1, 0));
H(nD3, grp2(16, 1, 0));

/* D4/D5 AAM/AAD */
H(nD4, { unsigned b = fetch8(); if (b == 0) { cpu.halted=1; cpu.exit_code=-3; return; }
         uint8_t al = (uint8_t)(cpu.r[AX] & 0xFF);
         cpu.r[AX] = (uint16_t)(((al / b) << 8) | (al % b));
         cpu.flags = (uint16_t)((cpu.flags & ~(F_SF|F_ZF|F_PF)) |
                     ((cpu.r[AX] & 0xFF) == 0 ? F_ZF : 0) |
                     (parity_even(cpu.r[AX] & 0xFF) ? F_PF : 0) |
                     ((cpu.r[AX] & 0x80) ? F_SF : 0)); });
H(nD5, { unsigned b = fetch8();
         uint8_t al = (uint8_t)(cpu.r[AX] & 0xFF);
         uint8_t ah = (uint8_t)(cpu.r[AX] >> 8);
         cpu.r[AX] = (uint16_t)((al * b) + ah); });

/* D6 SALC (undocumented), D7 XLAT */
H(nD6, cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | ((cpu.flags & F_CF) ? 0xFF : 0x00)));
H(nD7, { uint32_t p = seg_off_to_phys(cpu.seg_override >= 0 ? cpu.sr[cpu.seg_override] : cpu.sr[DS],
                                      (uint16_t)(cpu.r[BX] + (cpu.r[AX] & 0xFF)));
         cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | peek8(p)); });

/* D8-DF FPU: the original refuses these */
H(nD8, (cpu.halted = 1, cpu.exit_code = -4));
H(nD9, (cpu.halted = 1, cpu.exit_code = -4));
H(nDA, (cpu.halted = 1, cpu.exit_code = -4));
H(nDB, (cpu.halted = 1, cpu.exit_code = -4));
H(nDC, (cpu.halted = 1, cpu.exit_code = -4));
H(nDD, (cpu.halted = 1, cpu.exit_code = -4));
H(nDE, (cpu.halted = 1, cpu.exit_code = -4));
H(nDF, (cpu.halted = 1, cpu.exit_code = -4));

/* E0-E3 loops */
H(nE0, { int8_t d = fetch_s8(); uint16_t t = (uint16_t)(cpu.ip + d);
         cpu.r[CX] = (uint16_t)(cpu.r[CX] - 1);
         if (cpu.r[CX] != 0) { cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); } });
H(nE1, { int8_t d = fetch_s8(); uint16_t t = (uint16_t)(cpu.ip + d);
         cpu.r[CX] = (uint16_t)(cpu.r[CX] - 1);
         if (cpu.r[CX] != 0 && (cpu.flags & F_ZF)) { cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); } });
H(nE2, { int8_t d = fetch_s8(); uint16_t t = (uint16_t)(cpu.ip + d);
         cpu.r[CX] = (uint16_t)(cpu.r[CX] - 1);
         if (cpu.r[CX] != 0) { cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); } });
H(nE3, { int8_t d = fetch_s8(); uint16_t t = (uint16_t)(cpu.ip + d);
         if (cpu.r[CX] == 0) { cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); } });

/* E4-E7 IN/OUT immediate port */
H(nE4, { uint16_t p = fetch8(); cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | io_in8(p)); });
H(nE5, { uint16_t p = fetch8(); uint8_t lo = io_in8(p); uint8_t hi = io_in8((uint16_t)(p + 1));
         cpu.r[AX] = (uint16_t)(lo | (hi << 8)); });
H(nE6, { uint16_t p = fetch8(); io_out8(p, (uint8_t)cpu.r[AX]); });
H(nE7, { uint16_t p = fetch8(); io_out16(p, cpu.r[AX]); });

/* E8/E9/EA/EB */
H(nE8, { uint16_t d = fetch16(); uint16_t t = (uint16_t)(cpu.ip + d);
         push16(cpu.ip); cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); });
H(nE9, { uint16_t d = fetch16(); uint16_t t = (uint16_t)(cpu.ip + d);
         cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); });
H(nEA, { uint16_t off = fetch16(), seg = fetch16(); set_cs_ip(seg, off); });
H(nEB, { int8_t d = fetch_s8(); uint16_t t = (uint16_t)(cpu.ip + d);
         cpu.ip = t; cpu.phys = seg_off_to_phys(cpu.sr[CS], t); });

/* EC-EF IN/OUT DX */
H(nEC, cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | io_in8(cpu.r[DX])));
H(nED, { uint8_t lo = io_in8(cpu.r[DX]); uint8_t hi = io_in8((uint16_t)(cpu.r[DX] + 1));
         cpu.r[AX] = (uint16_t)(lo | (hi << 8)); });
H(nEE, io_out8(cpu.r[DX], (uint8_t)cpu.r[AX]));
H(nEF, io_out16(cpu.r[DX], cpu.r[AX]));

/* F0-F3 prefixes: consumed by the prefix loop, never reached */
H(nF0, (void)0);
H(nF1, (void)0);
H(nF2, (void)0);
H(nF3, (void)0);

/* F4 HLT */
H(nF4, cpu.halted = 1);

/* F5 CMC */
H(nF5, cpu.flags ^= F_CF);

/* F6/F7 group 3 */
H(nF6, grp3(8));
H(nF7, grp3(16));

/* F8-FD flag ops */
H(nF8, cpu.flags &= ~F_CF);
H(nF9, cpu.flags |=  F_CF);
H(nFA, cpu.flags &= ~F_IF);
H(nFB, cpu.flags |=  F_IF);
H(nFC, cpu.flags &= ~F_DF);
H(nFD, cpu.flags |=  F_DF);

/* FE/FF groups */
H(nFE, grp4());
H(nFF, grp5());

/* =====================================================================
 * Table wiring
 * ===================================================================*/
static int table_ready;

static void op_table_init(void)
{
    if (table_ready) return;
    table_ready = 1;

/* undefined by default -> the original's single funnel */
#define SET(op, fn) OP[op] = h_##fn

    SET(0x00, a000); SET(0x01, a001); SET(0x02, a002); SET(0x03, a003); SET(0x04, a004); SET(0x05, a005);
    SET(0x08, a080); SET(0x09, a081); SET(0x0a, a082); SET(0x0b, a083); SET(0x0c, a084); SET(0x0d, a085);
    SET(0x10, a100); SET(0x11, a101); SET(0x12, a102); SET(0x13, a103); SET(0x14, a104); SET(0x15, a105);
    SET(0x18, a180); SET(0x19, a181); SET(0x1a, a182); SET(0x1b, a183); SET(0x1c, a184); SET(0x1d, a185);
    SET(0x20, a200); SET(0x21, a201); SET(0x22, a202); SET(0x23, a203); SET(0x24, a204); SET(0x25, a205);
    SET(0x28, a280); SET(0x29, a281); SET(0x2a, a282); SET(0x2b, a283); SET(0x2c, a284); SET(0x2d, a285);
    SET(0x30, a300); SET(0x31, a301); SET(0x32, a302); SET(0x33, a303); SET(0x34, a304); SET(0x35, a305);
    SET(0x38, a380); SET(0x39, a381); SET(0x3a, a382); SET(0x3b, a383); SET(0x3c, a384); SET(0x3d, a385);

    SET(0x40, r400); SET(0x41, r401); SET(0x42, r402); SET(0x43, r403);
    SET(0x44, r404); SET(0x45, r405); SET(0x46, r406); SET(0x47, r407);
    SET(0x48, r500); SET(0x49, r501); SET(0x4a, r502); SET(0x4b, r503);
    SET(0x4c, r504); SET(0x4d, r505); SET(0x4e, r506); SET(0x4f, r507);

    SET(0x50, ps0); SET(0x51, ps1); SET(0x52, ps2); SET(0x53, ps3);
    SET(0x54, ps4); SET(0x55, ps5); SET(0x56, ps6); SET(0x57, ps7);
    SET(0x58, po0); SET(0x59, po1); SET(0x5a, po2); SET(0x5b, po3);
    SET(0x5c, po4); SET(0x5d, po5); SET(0x5e, po6); SET(0x5f, po7);

    SET(0x06, s06); SET(0x07, s07); SET(0x0e, s0e); SET(0x0f, s0f);
    SET(0x16, s16); SET(0x17, s17); SET(0x1e, s1e); SET(0x1f, s1f);

    { int i;
      for (i = 0x60; i <= 0x6f; i++) OP[i] = h_60 + (i - 0x60); }
    OP[0x70]=h_j70; OP[0x71]=h_j71; OP[0x72]=h_j72; OP[0x73]=h_j73;
    OP[0x74]=h_j74; OP[0x75]=h_j75; OP[0x76]=h_j76; OP[0x77]=h_j77;
    OP[0x78]=h_j78; OP[0x79]=h_j79; OP[0x7a]=h_j7a; OP[0x7b]=h_j7b;
    OP[0x7c]=h_j7c; OP[0x7d]=h_j7d; OP[0x7e]=h_j7e; OP[0x7f]=h_j7f;

    SET(0x80, g80); SET(0x81, g81); SET(0x82, g82); SET(0x83, g83);
    SET(0x84, t84); SET(0x85, t85); SET(0x86, x86); SET(0x87, x87);
    SET(0x88, m88); SET(0x89, m89); SET(0x8a, m8a); SET(0x8b, m8b);
    SET(0x8c, m8c); SET(0x8d, m8d); SET(0x8e, m8e); SET(0x8f, m8f);

    SET(0x90, n90); SET(0x91, n91); SET(0x92, n92); SET(0x93, n93);
    SET(0x94, n94); SET(0x95, n95); SET(0x96, n96); SET(0x97, n97);
    SET(0x98, n98); SET(0x99, n99); SET(0x9a, n9a); SET(0x9b, n9b);
    SET(0x9c, n9c); SET(0x9d, n9d); SET(0x9e, n9e); SET(0x9f, n9f);

    SET(0xa0, nA0); SET(0xa1, nA1); SET(0xa2, nA2); SET(0xa3, nA3);
    SET(0xa4, nA4); SET(0xa5, nA5); SET(0xa6, nA6); SET(0xa7, nA7);
    SET(0xa8, nA8); SET(0xa9, nA9); SET(0xaa, nAA); SET(0xab, nAB);
    SET(0xac, nAC); SET(0xad, nAD); SET(0xae, nAE); SET(0xaf, nAF);

    SET(0xb0, b0); SET(0xb1, b1); SET(0xb2, b2); SET(0xb3, b3);
    SET(0xb4, b4); SET(0xb5, b5); SET(0xb6, b6); SET(0xb7, b7);
    SET(0xb8, b8_); SET(0xb9, b9_); SET(0xba, ba_); SET(0xbb, bb_);
    SET(0xbc, bc_); SET(0xbd, bd_); SET(0xbe, be_); SET(0xbf, bf_);

    SET(0xc0, c0); SET(0xc1, c1); SET(0xc2, nC2); SET(0xc3, nC3);
    SET(0xc4, nC4); SET(0xc5, nC5); SET(0xc6, nC6); SET(0xc7, nC7);
    SET(0xc8, c8); SET(0xc9, c9); SET(0xca, nCA); SET(0xcb, nCB);
    SET(0xcc, nCC); SET(0xcd, nCD); SET(0xce, nCE); SET(0xcf, nCF);

    SET(0xd0, nD0); SET(0xd1, nD1); SET(0xd2, nD2); SET(0xd3, nD3);
    SET(0xd4, nD4); SET(0xd5, nD5); SET(0xd6, nD6); SET(0xd7, nD7);
    SET(0xd8, nD8); SET(0xd9, nD9); SET(0xda, nDA); SET(0xdb, nDB);
    SET(0xdc, nDC); SET(0xdd, nDD); SET(0xde, nDE); SET(0xdf, nDF);

    SET(0xe0, nE0); SET(0xe1, nE1); SET(0xe2, nE2); SET(0xe3, nE3);
    SET(0xe4, nE4); SET(0xe5, nE5); SET(0xe6, nE6); SET(0xe7, nE7);
    SET(0xe8, nE8); SET(0xe9, nE9); SET(0xea, nEA); SET(0xeb, nEB);
    SET(0xec, nEC); SET(0xed, nED); SET(0xee, nEE); SET(0xef, nEF);

    SET(0xf0, nF0); SET(0xf1, nF1); SET(0xf2, nF2); SET(0xf3, nF3);
    SET(0xf4, nF4); SET(0xf5, nF5); SET(0xf6, nF6); SET(0xf7, nF7);
    SET(0xf8, nF8); SET(0xf9, nF9); SET(0xfa, nFA); SET(0xfb, nFB);
    SET(0xfc, nFC); SET(0xfd, nFD); SET(0xfe, nFE); SET(0xff, nFF);
#undef SET
}

/* =====================================================================
 * The step loop -- fetch / prefix / decode / execute
 * ===================================================================*/
int cpu_step(void)
{
    uint32_t start_phys = cpu.phys;
    uint16_t start_ip   = cpu.ip;
    uint8_t  op;
    int      saw_rep = 0;

    op_table_init();

    if (cpu.halted) return 0;

    cpu.last_cs = cpu.sr[CS];
    cpu.last_ip = cpu.ip;
    cpu.seg_override = -1;
    cpu.lock = 0;
    cpu.rep_kind = 0;

    /* --- prefix loop --------------------------------------------------- */
    for (;;) {
        op = fetch8();
        switch (op) {
        case 0x26: cpu.seg_override = ES; continue;
        case 0x2E: cpu.seg_override = CS; continue;
        case 0x36: cpu.seg_override = SS; continue;
        case 0x3E: cpu.seg_override = DS; continue;
        case 0xF0: cpu.lock = 1;         continue;
        case 0xF2: cpu.rep_kind = 2;     saw_rep = 1; continue;
        case 0xF3: cpu.rep_kind = 1;     saw_rep = 1; continue;
        default: break;
        }
        break;
    }

    cpu.last_op = op;

    /* --- execute ------------------------------------------------------ */
    if (OP[op]) {
        /* REP on a string op: repeat CX times, honouring the ZF gate */
        if (saw_rep && (op == 0xA4 || op == 0xA5 || op == 0xA6 || op == 0xA7 ||
                        op == 0xAA || op == 0xAB || op == 0xAC || op == 0xAD ||
                        op == 0xAE || op == 0xAF)) {
            if (cpu.r[CX] == 0) {
                /* nothing to do; length is just the two prefix+op bytes */
            } else {
                for (;;) {
                    uint32_t before = cpu.phys;
                    OP[op]();
                    (void)before;
                    cpu.r[CX] = (uint16_t)(cpu.r[CX] - 1);
                    if (cpu.r[CX] == 0) break;
                    if ((op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF)) {
                        int zf = (cpu.flags & F_ZF) ? 1 : 0;
                        if (cpu.rep_kind == 1 && !zf) break;   /* REPE  */
                        if (cpu.rep_kind == 2 &&  zf) break;   /* REPNE */
                    }
                    if (cpu.halted) break;
                }
            }
        } else {
            OP[op]();
        }
    } else {
        /* the original's single funnel for unassigned bytes */
        cpu.halted = 1;
        cpu.exit_code = -2;
        return 0;
    }

    cpu.icount++;
    cpu.last_len = (int)(cpu.phys - start_phys);
    if (cpu.last_len == 0) cpu.last_len = (int)(cpu.ip - start_ip);
    return 1;
}

int cpu_run(int max_steps)
{
    int n = 0;
    while (!cpu.halted) {
        if (max_steps > 0 && n >= max_steps) return n;
        if (!cpu_step()) break;
        n++;
    }
    return n;
}
