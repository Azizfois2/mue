/* mue — assembler: two-pass driver and instruction encoder
 * asm_main.c  (part 1: directives, data, and the encoder core)
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "asm.h"
#include "asm_ops.h"

/* define an "equ"-style constant symbol */
static void define_const(asm_t *a, const char *name, int v)
{
    label_t *l = find_label(a, name);
    if (!l) add_label(a, name, (uint16_t)v, -1, 0);
    else if (a->sizing) { l->value = (uint16_t)v; l->size = -1; }
}

static void lower_str(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* True if this line closes a macro body.  Accepts both the bare form
 * (`ENDM`) and the name-prefixed form emu8086.inc uses
 * (`DEFINE_SCAN_NUM ENDM`). */
static int is_endm(const char *s)
{
    const char *p = s;

    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "endm", 4) == 0) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        return !*p;
    }

    /* skip a leading identifier, then look for ENDM as the next word */
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p == ' ' || *p == '\t') p++;
    return strncasecmp(p, "endm", 4) == 0;
}

/* Strip a trailing comment, respecting quotes. */
static void strip_comment(char *s)
{
    int q = 0;
    char *p = s;
    while (*p) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '\'' || *p == '"') q = *p;
        else if (*p == ';') { *p = 0; return; }
        p++;
    }
}

static int is_ident(const char *s)
{
    if (!(isalpha((unsigned char)*s) || *s == '_' || *s == '.' || *s == '@' || *s == '?')) return 0;
    for (s++; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == '@' || *s == '?' || *s == '$'))
            return 0;
    return 1;
}

/* ---- operand-size inference ------------------------------------------ */
/* Size rules, matching emu8086: an explicit "byte/word ptr" wins; a register
 * operand fixes the width; a memory operand with an immediate and no other
 * hint is treated as byte-sized (the original emits 80 / C6 for these). */
static int effective_size(const operand_t *a, const operand_t *b)
{
    if (a->size) return a->size;
    if (b && b->size) return b->size;
    if (a->kind == OP_MEM && b && b->kind == OP_IMM) return 8;
    return 0;
}

static int is_mem(const operand_t *o) { return o->kind == OP_MEM; }
static int is_reg(const operand_t *o) { return o->kind == OP_REG8 || o->kind == OP_REG16; }

/* ---- segment-prefix emission ---------------------------------------- */
static void emit_seg_prefix(asm_t *a, const operand_t *op)
{
    static const uint8_t pre[4] = { 0x26, 0x2E, 0x36, 0x3E };
    if (op && op->kind == OP_MEM && op->seg >= 0) emit8(a, pre[op->seg]);
}

/* ---- ModRM + displacement ------------------------------------------- */

/* Does this operand text begin with a defined label name?
 *
 * Operands like "buf+2" or "str - 1" are not label names themselves, so a
 * plain find_label() on the whole string misses them.  Returns the label when
 * the leading identifier names one, NULL otherwise.
 */
static label_t *starts_with_label(asm_t *a, const char *text)
{
    char lead[256];
    size_t k = 0;
    if (!text) return NULL;
    while (isspace((unsigned char)*text)) text++;
    while (text[k] && (isalnum((unsigned char)text[k]) || text[k] == '_' ||
                       text[k] == '@' || text[k] == '$' || text[k] == '?') &&
           k < sizeof(lead) - 1)
        lead[k] = text[k], k++;
    lead[k] = 0;
    if (!k) return NULL;
    lower_str(lead);
    return find_label(a, lead);
}

static int emit_modrm(asm_t *a, const operand_t *rm, int regf, int w)
{
    int b[8], n = 0;
    if (build_modrm(a, rm, regf, b, &n) != 0) { asm_error(a, "bad memory operand"); return 0; }
    { int i; for (i = 0; i < n; i++) emit8(a, (uint8_t)b[i]); }
    (void)w;
    return 1;
}

/* =====================================================================
 * Instruction encoder
 * Returns bytes emitted (0 if the mnemonic was not recognised).
 * ===================================================================*/

static int emit_alu(asm_t *a, int opbase, operand_t *d, operand_t *s, int w)
{
    int wbit = (w == 16) ? 1 : 0;

    if (is_reg(d) && is_mem(s)) {
        emit_seg_prefix(a, s);
        emit8(a, (uint8_t)(opbase + 0x02 + wbit));        /* op r,rm */
        return emit_modrm(a, s, d->reg, w);
    }
    if (is_reg(d) && is_reg(s)) {
        /* emu8086 prefers the r,r/m direction (0x02/0x03) for reg,reg */
        emit8(a, (uint8_t)(opbase + 0x02 + wbit));
        return emit_modrm(a, s, d->reg, w);
    }
    if (is_mem(d) && is_reg(s)) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(opbase + 0x02 + wbit * 1));    /* op r,rm */
        return emit_modrm(a, d, s->reg, w);
    }
    if (is_reg(d) && s->kind == OP_IMM) {
        if (d->reg == 0 && !wbit) {                        /* AL,ib */
            emit8(a, (uint8_t)(opbase + 4));
            emit8(a, (uint8_t)s->imm);
        } else if (d->reg == 0) {                          /* AX,iw */
            emit8(a, (uint8_t)(opbase + 5));
            emit16(a, (uint16_t)s->imm);
        } else {
            if (wbit && s->imm >= -128 && s->imm <= 127) {
                emit8(a, 0x83);
                emit_modrm(a, d, opbase >> 3, w);
                emit8(a, (uint8_t)(s->imm & 0xFF));
            } else {
                emit8(a, (uint8_t)(wbit ? 0x81 : 0x80));
                emit_modrm(a, d, opbase >> 3, w);
                if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
            }
        }
        return 1;
    }
    if (is_mem(d) && s->kind == OP_IMM) {
        emit_seg_prefix(a, d);
        if (wbit && s->imm >= -128 && s->imm <= 127) {
            emit8(a, 0x83);                                /* sign-extended imm8 */
            emit_modrm(a, d, opbase >> 3, w);
            emit8(a, (uint8_t)(s->imm & 0xFF));
        } else {
            emit8(a, (uint8_t)(wbit ? 0x81 : 0x80));
            emit_modrm(a, d, opbase >> 3, w);
            if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
        }
        return 1;
    }
    return 0;
}

/* mov / test / xchg share the rm<->reg shapes */
static int emit_mov(asm_t *a, operand_t *d, operand_t *s)
{
    int w = effective_size(d, s);
    int wbit = (w == 16) ? 1 : 0;

    /* emu8086 uses the moffs forms (A0..A3) when one side is AL/AX and the
     * other is a plain absolute [constant] with no base or index register. */
    if (s->kind == OP_MEM && s->moffs && s->base < 0 && s->index < 0 &&
        d->kind != OP_MEM && !(d->kind == OP_SREG)) {
        int r8  = (d->kind == OP_REG8  && d->reg == 0);
        int r16 = (d->kind == OP_REG16 && d->reg == 0);
        if (r8 || r16) {
            emit_seg_prefix(a, s);
            emit8(a, (uint8_t)(0xA0 + (r16 ? 1 : 0)));   /* load AL/AX */
            emit16(a, (uint16_t)s->disp);
            return 1;
        }
    }
    if (d->kind == OP_MEM && d->moffs && d->base < 0 && d->index < 0 &&
        s->kind != OP_MEM) {
        int r8  = (s->kind == OP_REG8  && s->reg == 0);
        int r16 = (s->kind == OP_REG16 && s->reg == 0);
        if (r8 || r16) {
            emit_seg_prefix(a, d);
            emit8(a, (uint8_t)(0xA2 + (r16 ? 1 : 0)));   /* store AL/AX */
            emit16(a, (uint16_t)d->disp);
            return 1;
        }
    }

    if (is_reg(d) && is_reg(s)) {
        /* emu8086 emits the load direction (8A/8B) for reg,reg */
        emit8(a, (uint8_t)(0x8A + wbit));
        return emit_modrm(a, s, d->reg, w);
    }
    if (is_mem(d) && is_reg(s)) {                 /* store */
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(0x88 + wbit));
        return emit_modrm(a, d, s->reg, w);
    }
    if (is_reg(d) && is_mem(s)) {                 /* load */
        emit_seg_prefix(a, s);
        emit8(a, (uint8_t)(0x8A + wbit));
        return emit_modrm(a, s, d->reg, w);
    }
    if (is_reg(d) && s->kind == OP_IMM) {
        if (w == 8) { emit8(a, (uint8_t)(0xB0 + d->reg)); emit8(a, (uint8_t)s->imm); }
        else        { emit8(a, (uint8_t)(0xB8 + d->reg)); emit16(a, (uint16_t)s->imm); }
        return 1;
    }
    if (is_mem(d) && s->kind == OP_IMM) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(wbit ? 0xC7 : 0xC6));
        emit_modrm(a, d, 0, w);
        if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
        return 1;
    }
    if (d->kind == OP_SREG && (is_reg(s) || is_mem(s))) {   /* mov sreg,r/m16 */
        emit_seg_prefix(a, s);
        emit8(a, 0x8E);
        return emit_modrm(a, s, d->reg, 16);
    }
    if ((is_reg(d) || is_mem(d)) && s->kind == OP_SREG) {   /* mov r/m16,sreg */
        emit_seg_prefix(a, d);
        emit8(a, 0x8C);
        return emit_modrm(a, d, s->reg, 16);
    }
    return 0;
}

static int emit_test(asm_t *a, operand_t *d, operand_t *s)
{
    int w = effective_size(d, s);
    int wbit = (w == 16) ? 1 : 0;

    if (is_reg(d) && d->reg == 0 && s->kind == OP_IMM) {
        emit8(a, (uint8_t)(0xA8 + wbit));
        if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
        return 1;
    }
    if (is_mem(d) && s->kind == OP_IMM) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(wbit ? 0xF7 : 0xF6));
        emit_modrm(a, d, 0, w);
        if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
        return 1;
    }
    if (is_reg(d) && s->kind == OP_IMM) {          /* F6/F7 /0 r/m,imm */
        emit8(a, (uint8_t)(wbit ? 0xF7 : 0xF6));
        emit_modrm(a, d, 0, w);
        if (wbit) emit16(a, (uint16_t)s->imm); else emit8(a, (uint8_t)s->imm);
        return 1;
    }
    if (is_reg(d) && (is_reg(s) || is_mem(s))) {
        emit_seg_prefix(a, s);
        emit8(a, (uint8_t)(0x84 + wbit));
        return emit_modrm(a, s, d->reg, w);
    }
    if (is_mem(d) && is_reg(s)) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(0x84 + wbit));
        return emit_modrm(a, d, s->reg, w);
    }
    return 0;
}

static int emit_incdec(asm_t *a, int base16, int base8grp, operand_t *d)
{
    int w = d->size ? d->size : 16;
    if (is_reg(d) && w == 16) {
        emit8(a, (uint8_t)(base16 + d->reg));
        return 1;
    }
    emit_seg_prefix(a, d);
    emit8(a, (uint8_t)(w == 16 ? 0xFF : 0xFE));
    return emit_modrm(a, d, base8grp, w);
}

static int emit_shift(asm_t *a, int sub, operand_t *d, operand_t *s)
{
    int w = d->size ? d->size : 16;
    int wbit = (w == 16) ? 1 : 0;

    if (s->kind == OP_IMM && s->imm == 1) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(0xD0 + wbit));
        return emit_modrm(a, d, sub, w);
    }
    if (s->kind == OP_REG8 && s->reg == 1) {            /* CL */
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(0xD2 + wbit));
        return emit_modrm(a, d, sub, w);
    }
    if (s->kind == OP_IMM) {
        emit_seg_prefix(a, d);
        emit8(a, (uint8_t)(0xC0 + wbit));               /* 80186 form */
        emit_modrm(a, d, sub, w);
        emit8(a, (uint8_t)s->imm);
        return 1;
    }
    return 0;
}

static void emit_jmp_target(asm_t *a, int v, int shortok)
{
    int base = a->ip;                       /* before any byte is emitted */
    int d = v - (base + 2);
    if (shortok && d >= -128 && d <= 127) {
        emit8(a, 0xEB);
        emit8(a, (uint8_t)(d & 0xFF));
    } else {
        emit8(a, 0xE9);
        emit16(a, (uint16_t)(v - (base + 3)));
    }
}

/* ---- the mnemonic table --------------------------------------------- */
typedef struct { const char *m; int code; } mnem_t;

static const mnem_t MN[] = {
    {"add",0x00},{"or",0x08},{"adc",0x10},{"sbb",0x18},
    {"and",0x20},{"sub",0x28},{"xor",0x30},{"cmp",0x38},
    {"mov",-1},{"test",-2},{"xchg",-3},
    {"inc",-4},{"dec",-5},{"push",-6},{"pop",-7},
    {"not",6},{"neg",3},{"mul",4},{"imul",5},{"div",6},{"idiv",7},
    {"rol",0},{"ror",1},{"rcl",2},{"rcr",3},{"shl",4},{"sal",4},
    {"shr",5},{"sar",7},
    {"nop",0x90},{"cbw",0x98},{"cwd",0x99},{"hlt",0xF4},
    {"clc",0xF8},{"stc",0xF9},{"cli",0xFA},{"sti",0xFB},
    {"cld",0xFC},{"std",0xFD},{"cmc",0xF5},
    {"pushf",0x9C},{"popf",0x9D},{"sahf",0x9E},{"lahf",0x9F},
    {"ret",0xC3},{"retn",0xC3},{"retf",0xCB},{"iret",0xCF},
    {"int3",0xCC},{"into",0xCE},{"leave",0xC9},
    {"movsb",0xA4},{"movsw",0xA5},{"cmpsb",0xA6},{"cmpsw",0xA7},
    {"stosb",0xAA},{"stosw",0xAB},{"lodsb",0xAC},{"lodsw",0xAD},
    {"scasb",0xAE},{"scasw",0xAF},{"xlat",0xD7},{"aam",0xD4},{"aad",0xD5},
    {"pusha",0x60},{"popa",0x61},{"pushad",0x60},{"popad",0x61},
    {"aaa",0x37},{"aas",0x3F},{"daa",0x27},{"das",0x2F},
    {"xlatb",0xD7},{"xlat",0xD7},
    {NULL,0}
};

/* Jcc: name -> condition code */
typedef struct { const char *m; int cc; } jcc_t;
static const jcc_t JCC[] = {
    {"jo",0},{"jno",1},{"jb",2},{"jc",2},{"jnae",2},
    {"jae",3},{"jnb",3},{"jnc",3},{"je",4},{"jz",4},
    {"jne",5},{"jnz",5},{"jbe",6},{"jna",6},{"ja",7},{"jnbe",7},
    {"js",8},{"jns",9},{"jp",10},{"jpe",10},{"jnp",11},{"jpo",11},
    {"jl",12},{"jnge",12},{"jge",13},{"jnl",13},
    {"jle",14},{"jng",14},{"jg",15},{"jnle",15},
    {NULL,0}
};

static int find_jcc(const char *m)
{
    int i;
    for (i = 0; JCC[i].m; i++) if (strcmp(JCC[i].m, m) == 0) return JCC[i].cc;
    return -1;
}

static int find_mnem(const char *m, int *code)
{
    int i;
    for (i = 0; MN[i].m; i++)
        if (strcmp(MN[i].m, m) == 0) { *code = MN[i].code; return 1; }
    return 0;
}

/* ---- one instruction ------------------------------------------------ */
static int encode_insn(asm_t *a, const char *mnem_in, char **ops, int nops)
{
    operand_t d, s;
    char m[32];
    int code;

    snprintf(m, sizeof(m), "%s", mnem_in);

    /* prefixes */
    if (!strcmp(m, "rep") || !strcmp(m, "repe") || !strcmp(m, "repz")) {
        emit8(a, 0xF3); return 1;
    }
    if (!strcmp(m, "repne") || !strcmp(m, "repnz")) { emit8(a, 0xF2); return 1; }

    /* jcc */
    {
        int cc = find_jcc(m);
        if (cc >= 0) {
            int ok, v;
            if (nops != 1) { asm_error(a, "jcc needs one target"); return 0; }
            v = eval_expr(a, ops[0], &ok);
            if (!ok) {
                if (a->sizing) { emit8(a, (uint8_t)(0x70 + (cc ^ 1))); emit8(a, 3);
                                    emit8(a, 0xE9); emit16(a, 0); return 1; }
                asm_error(a, "bad jump target '%s'", ops[0]);
                return 0;
            }
            {
                int base = a->ip;
                int d16 = v - (base + 2);
                if (d16 >= -128 && d16 <= 127) {
                    emit8(a, (uint8_t)(0x70 + cc));
                    emit8(a, (uint8_t)(d16 & 0xFF));
                } else {                        /* expand to Jcc over JMP */
                    int far_d = v - (base + 5);
                    emit8(a, (uint8_t)(0x70 + (cc ^ 1)));
                    emit8(a, 0x03);
                    emit8(a, 0xE9);
                    emit16(a, (uint16_t)far_d);
                }
            }
            return 1;
        }
    }

    /* jmp / call */
    if (!strcmp(m, "jmp") || !strcmp(m, "call")) {
        int ok, v;
        if (nops != 1) { asm_error(a, "%s needs one target", m); return 0; }

        /* far jump/call: `jmp seg:off` / `call seg:off` -> EA/9A, offset then
         * segment.  A COLON is not an operator, so this must be recognised
         * before eval_expr() tries to evaluate the operand. */
        {
            const char *colon = strchr(ops[0], ':');
            if (colon) {
                char segpart[128];
                int okc, okd;
                size_t sl = (size_t)(colon - ops[0]);
                uint16_t segv, offv;
                if (sl >= sizeof(segpart)) sl = sizeof(segpart) - 1;
                memcpy(segpart, ops[0], sl);
                segpart[sl] = 0;
                segv = (uint16_t)eval_expr(a, segpart, &okc);
                offv = (uint16_t)eval_expr(a, colon + 1, &okd);
                if (!okc || !okd) {
                    if (a->sizing) {
                        emit8(a, (uint8_t)(m[0] == 'c' ? 0x9A : 0xEA));
                        emit16(a, 0); emit16(a, 0);
                        return 1;
                    }
                    asm_error(a, "bad far target '%s'", ops[0]);
                    return 0;
                }
                emit8(a, (uint8_t)(m[0] == 'c' ? 0x9A : 0xEA));
                emit16(a, offv);
                emit16(a, segv);
                return 1;
            }
        }

        v = eval_expr(a, ops[0], &ok);
        if (!ok) {
            operand_t t;
            if (!parse_operand(a, ops[0], &t)) { asm_error(a, "bad target"); return 0; }
            if (t.kind == OP_REG8 || t.kind == OP_REG16 || t.kind == OP_MEM) {
                if (!strcmp(m, "call")) { emit_seg_prefix(a, &t); emit8(a, 0xFF);
                    emit_modrm(a, &t, 2, 16); }
                else                    { emit_seg_prefix(a, &t); emit8(a, 0xFF);
                    emit_modrm(a, &t, 4, 16); }
                return 1;
            }
            /* forward label in pass 1: assume the wide form so offsets match */
            if (a->sizing) {
                emit8(a, (uint8_t)(m[0] == 'c' ? 0xE8 : 0xE9));
                emit16(a, 0);
                return 1;
            }
            asm_error(a, "bad target '%s'", ops[0]);
            return 0;
        }
        if (!strcmp(m, "call")) {
            int base = a->ip;                 /* before the opcode is emitted */
            emit8(a, 0xE8);
            emit16(a, (uint16_t)(v - (base + 3)));
        } else {
            emit_jmp_target(a, v, 1);
        }
        return 1;
    }

    if (!strcmp(m, "loop") || !strcmp(m, "loope") || !strcmp(m, "loopz") ||
        !strcmp(m, "loopne") || !strcmp(m, "loopnz") || !strcmp(m, "jcxz")) {
        int ok, v, d16;
        uint8_t oc = 0xE2;
        if (!strcmp(m, "loope") || !strcmp(m, "loopz")) oc = 0xE1;
        if (!strcmp(m, "loopne") || !strcmp(m, "loopnz")) oc = 0xE0;
        if (!strcmp(m, "jcxz")) oc = 0xE3;
        if (nops != 1) { asm_error(a, "%s needs a target", m); return 0; }
        v = eval_expr(a, ops[0], &ok);
        if (!ok) {
            if (a->sizing) { emit8(a, oc); emit8(a, 0); return 1; }
            asm_error(a, "bad target '%s'", ops[0]);
            return 0;
        }
        d16 = v - (a->ip + 2);
        if (d16 < -128 || d16 > 127) {
            /* The rel8 forms only reach +-127 bytes.  emu8086 documents the
             * rewrite it uses for an over-long JCXZ -- its own writebin.asm
             * says "this instruction is assembled into OR CX, CX and two
             * JMPs", and asm_tutorial_07.html gives the equivalence
             * "JCXZ = OR CX,CX and JNZ".  So emit:
             *
             *     OR  CX, CX          2 bytes, ZF = (CX == 0)
             *     JNZ short $+3       2 bytes, skip the jump when CX != 0
             *     JMP target          3 bytes, reached only when CX == 0
             *
             * That is exactly "jump to target when CX is zero" and is 8 bytes
             * wide, so it is stable across sizing passes.  The condition must
             * be JNZ: with JZ the jump would be taken on both paths.
             *
             * Only JCXZ has a documented rewrite; an over-long LOOP/
             * LOOPE/LOOPNE has no published expansion and no shipped listing
             * exercises one, so those keep reporting the problem rather than
             * receiving invented bytes. */
            if (oc == 0xE3) {                       /* jcxz */
                int jd;
                /* Fixed 8-byte rewrite: or cx,cx (2) / jnz +3 (2) / jmp (3).
                 * The width does not depend on the target, so the real
                 * displacement can be written immediately -- there is no
                 * separate emit pass, so a "sizing" placeholder would be
                 * what finally ships. */
                emit8(a, 0x0B); emit8(a, 0xC9);         /* or  cx,cx  (ZF=1 iff CX==0) */
                emit8(a, 0x75); emit8(a, 0x03);         /* jnz short +3: skip jmp */
                jd = v - (a->ip + 3);                   /* jmp is 3 bytes */
                emit8(a, 0xE9);
                emit16(a, (uint16_t)jd);
                return 1;
            }
            asm_error(a, "%s target too far (%d bytes, limit 127)", m, d16);
            return 0;
        }
        emit8(a, oc);
        emit8(a, (uint8_t)(d16 & 0xFF));
        return 1;
    }

    if (!strcmp(m, "int")) {
        int ok, v;
        if (nops != 1) { asm_error(a, "int needs a number"); return 0; }
        v = eval_expr(a, ops[0], &ok);
        if (!ok) { asm_error(a, "bad int number"); return 0; }
        if ((v & 0xFF) == 3) { emit8(a, 0xCC); return 1; }   /* INT 3 is a special opcode */
        emit8(a, 0xCD); emit8(a, (uint8_t)v);
        return 1;
    }

    if (!strcmp(m, "in") || !strcmp(m, "out")) {
        operand_t r, p;
        if (nops != 2) { asm_error(a, "in/out need 2 operands"); return 0; }
        if (!strcmp(m, "in")) {
            /* in  reg, port */
            if (!parse_operand(a, ops[0], &r) || !parse_operand(a, ops[1], &p)) return 0;
        } else {
            /* out port, reg */
            if (!parse_operand(a, ops[0], &p) || !parse_operand(a, ops[1], &r)) return 0;
        }
        {
            int wbit = (r.kind == OP_REG8) ? 0 : 1;
            int imm  = !(p.kind == OP_REG8 && p.reg == 2);   /* DX register */
            if (!strcmp(m, "in")) {
                emit8(a, (uint8_t)((imm ? 0xE4 : 0xEC) + wbit));
            } else {
                emit8(a, (uint8_t)((imm ? 0xE6 : 0xEE) + wbit));
            }
            if (imm) emit8(a, (uint8_t)(p.imm & 0xFF));
            return 1;
        }
    }

    /* lea / lds / les */
    if (!strcmp(m, "lea")) {
        if (nops != 2) { asm_error(a, "lea needs 2 operands"); return 0; }
        if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
        if (!is_reg(&d)) { asm_error(a, "lea needs a register destination"); return 0; }
        /* "lea reg, symbol" (or "symbol+d") uses the symbol's address;
         * emu8086 folds it into a plain immediate move.  The operand text is
         * not usually a bare label name, so match on its leading identifier. */
        if (s.kind == OP_MEM || starts_with_label(a, ops[1])) {
            int ok2, lv = eval_expr(a, ops[1], &ok2);
            if (ok2) {
                int val = lv;
                emit8(a, (uint8_t)((d.size == 8 ? 0xB0 : 0xB8) + d.reg));
                if (d.size == 8) emit8(a, (uint8_t)val); else emit16(a, (uint16_t)val);
                return 1;
            }
        }
        if (s.kind == OP_MEM && s.base < 0 && s.index < 0) {
            emit8(a, (uint8_t)((d.size == 8 ? 0xB0 : 0xB8) + d.reg));
            if (d.size == 8) emit8(a, (uint8_t)s.disp); else emit16(a, (uint16_t)s.disp);
            return 1;
        }
        emit_seg_prefix(a, &s);
        emit8(a, 0x8D);
        return emit_modrm(a, &s, d.reg, 16);
    }
    if (!strcmp(m, "lds") || !strcmp(m, "les")) {
        if (nops != 2) { asm_error(a, "lea needs 2 operands"); return 0; }
        if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
        if (!is_reg(&d)) { asm_error(a, "lea needs a register destination"); return 0; }
        emit_seg_prefix(a, &s);
        emit8(a, m[0] == 'l' && m[1] == 'e' ? 0x8D : (m[1] == 'd' ? 0xC5 : 0xC4));
        return emit_modrm(a, &s, d.reg, 16);
    }

    /* string ops with explicit operands are ignored; mnemonic is enough */
    {
        int c;
        if (find_mnem(m, &c) && c >= 0 && nops == 0) {
            emit8(a, (uint8_t)c);
            return 1;
        }
    }

    /* shift/rotate by name */
    {
        static const struct { const char *n; int sub; } SH[] = {
            {"rol",0},{"ror",1},{"rcl",2},{"rcr",3},{"shl",4},{"sal",4},
            {"shr",5},{"sar",7},{NULL,0}
        };
        int i;
        for (i = 0; SH[i].n; i++) {
            if (!strcmp(m, SH[i].n)) {
                if (nops < 1) { asm_error(a, "shift needs operands"); return 0; }
                if (!parse_operand(a, ops[0], &d)) return 0;
                if (nops == 1) { s.kind = OP_IMM; s.imm = 1; s.size = 0; s.text = "1"; }
                else if (!parse_operand(a, ops[1], &s)) return 0;
                return emit_shift(a, SH[i].sub, &d, &s);
            }
        }
    }

    /* not / neg / mul / imul / div / idiv */
    {
        static const struct { const char *n; int sub; } G3[] = {
            {"not",2},{"neg",3},{"mul",4},{"imul",5},{"div",6},{"idiv",7},{NULL,0}
        };
        int i;
        for (i = 0; G3[i].n; i++) {
            if (!strcmp(m, G3[i].n)) {
                int w;
                if (nops < 1) { asm_error(a, "%s needs an operand", m); return 0; }
                if (!parse_operand(a, ops[0], &d)) return 0;
                /* no explicit size on a memory operand means byte-sized */
                w = d.size ? d.size : (is_mem(&d) ? 8 : 16);
                /* imul with 2 operands is the 80186 3-operand form */
                if (!strcmp(m, "imul") && nops >= 2) { asm_error(a, "imul reg,imm is 80186"); return 0; }
                emit_seg_prefix(a, &d);
                emit8(a, (uint8_t)(w == 16 ? 0xF7 : 0xF6));
                return emit_modrm(a, &d, G3[i].sub, w);
            }
        }
    }

    /* inc / dec */
    if (!strcmp(m, "inc") || !strcmp(m, "dec")) {
        if (nops != 1) { asm_error(a, "%s needs one operand", m); return 0; }
        if (!parse_operand(a, ops[0], &d)) return 0;
        return emit_incdec(a, !strcmp(m, "inc") ? 0x40 : 0x48,
                           !strcmp(m, "inc") ? 0 : 1, &d);
    }

    /* push / pop */
    if (!strcmp(m, "push") || !strcmp(m, "pop")) {
        int ispop = !strcmp(m, "pop");
        if (nops != 1) { asm_error(a, "%s needs one operand", m); return 0; }
        if (!parse_operand(a, ops[0], &d)) return 0;
        if (is_reg(&d) && d.size == 16) {
            emit8(a, (uint8_t)((ispop ? 0x58 : 0x50) + d.reg));
            return 1;
        }
        if (d.kind == OP_SREG) {
            /* One byte for all eight forms: 06/07 ES, 0E/0F CS, 16/17 SS,
             * 1E/1F DS.  SR[] is already in that order, so the index folds
             * into the opcode (FINDINGS 16). */
            emit8(a, (uint8_t)((ispop ? 0x07 : 0x06) + (d.reg << 3)));
            return 1;
        }
        if (d.kind == OP_IMM) {
            emit8(a, 0x68);
            emit16(a, (uint16_t)d.imm);
            return 1;
        }
        emit_seg_prefix(a, &d);
        emit8(a, (uint8_t)(ispop ? 0x8F : 0xFF));
        return emit_modrm(a, &d, ispop ? 0 : 6, 16);
    }

    /* mov / test / xchg */
    if (!strcmp(m, "mov")) {
        if (nops != 2) { asm_error(a, "mov needs 2 operands"); return 0; }
        if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
        return emit_mov(a, &d, &s);
    }
    if (!strcmp(m, "test")) {
        if (nops != 2) { asm_error(a, "test needs 2 operands"); return 0; }
        if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
        return emit_test(a, &d, &s);
    }
    if (!strcmp(m, "xchg")) {
        int w;
        if (nops != 2) { asm_error(a, "xchg needs 2 operands"); return 0; }
        if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
        w = effective_size(&d, &s);
        if (is_reg(&d) && is_reg(&s) && w == 16) {
            if (d.reg == 0) { emit8(a, (uint8_t)(0x90 + s.reg)); return 1; }
            if (s.reg == 0) { emit8(a, (uint8_t)(0x90 + d.reg)); return 1; }
        }
        if (is_reg(&d) && is_mem(&s)) { emit_seg_prefix(a, &s);
            emit8(a, (uint8_t)(w == 16 ? 0x87 : 0x86));
            return emit_modrm(a, &s, d.reg, w); }
        if (is_mem(&d) && is_reg(&s)) { emit_seg_prefix(a, &d);
            emit8(a, (uint8_t)(w == 16 ? 0x87 : 0x86));
            return emit_modrm(a, &d, s.reg, w); }
        return 0;
    }

    /* the plain ALU ops */
    {
        int c;
        if (find_mnem(m, &c) && c >= 0 && c < 0x40 && (c & 7) == 0) {
            int w;
            if (nops != 2) { asm_error(a, "%s needs 2 operands", m); return 0; }
            if (!parse_operand(a, ops[0], &d) || !parse_operand(a, ops[1], &s)) return 0;
            w = effective_size(&d, &s);
            if (!w) w = 16;
            return emit_alu(a, c, &d, &s, w);
        }
    }
    /* zero-operand opcodes */
    {
        int c;
        if (find_mnem(m, &c) && c >= 0) {
            if (nops != 0) { asm_error(a, "%s takes no operands", m); return 0; }
            emit8(a, (uint8_t)c);
            return 1;
        }
    }

    asm_error(a, "unknown instruction '%s'", m);
    return 0;
}

/* =====================================================================
 * Data definition: db / dw / dd / label db ...
 * ===================================================================*/
static int emit_data(asm_t *a, const char *dir, char *rest)
{
    char *items[128];
    int n = split_operands(rest, items, 128), i;

    for (i = 0; i < n; i++) {
        char *it = items[i];
        int ok;

        /* string literal */
        if (it[0] == '\'' || it[0] == '"') {
            int q = it[0];
            char *p = it + 1;
            while (*p && *p != q) {
                if (!strcmp(dir, "db")) { emit8(a, (uint8_t)*p); p++; }
                else if (!strcmp(dir, "dw")) { emit16(a, (uint16_t)(unsigned char)*p); p++; }
                else { emit8(a, (uint8_t)*p); emit8(a, 0); emit8(a, 0); emit8(a, 0); p++; }
            }
            continue;
        }

        /* N DUP (value) */
        {
            char *dup = strstr(it, " dup");
            if (!dup) dup = strstr(it, " DUP");
            if (dup) {
                int count;
                char cnt[64];
                size_t cl = (size_t)(dup - it);
                char *open, *close;
                if (cl >= sizeof(cnt)) cl = sizeof(cnt) - 1;
                memcpy(cnt, it, cl); cnt[cl] = 0;
                count = eval_expr(a, cnt, &ok);
                open = strchr(dup, '(');
                close = strrchr(dup, ')');
                if (ok && open && close && close > open) {
                    char inner[256];
                    int k;
                    snprintf(inner, sizeof(inner), "%.*s", (int)(close - open - 1), open + 1);
                    for (k = 0; k < count; k++) {
                        emit_data(a, dir, inner);
                    }
                } else a->errcount++;
                continue;
            }
        }

        /* plain value */
        {
            int v;
            /* "db ?" reserves space without an initial value */
            if (it[0] == '?' && it[1] == 0) {
                if (!strcmp(dir, "db"))      emit8(a, 0);
                else if (!strcmp(dir, "dw")) emit16(a, 0);
                else { emit8(a, 0); emit8(a, 0); emit8(a, 0); emit8(a, 0); }
                continue;
            }
            v = eval_expr(a, it, &ok);
            if (!ok) { asm_line_error(a, a->errline, "bad data value '%s'", it); continue; }
            if (!strcmp(dir, "db"))      emit8(a, (uint8_t)v);
            else if (!strcmp(dir, "dw")) emit16(a, (uint16_t)v);
            else { emit8(a, (uint8_t)v); emit8(a, (uint8_t)(v >> 8));
                   emit8(a, 0); emit8(a, 0); }
        }
    }
    return 1;
}

/* =====================================================================
 * Line processing
 * ===================================================================*/
void process_line(asm_t *a, char *raw, int lineno);

/* case-insensitive "does s start with word kw at a word boundary" */
static int starts_ci(const char *s, const char *kw)
{
    size_t n = strlen(kw);
    if (strncasecmp(s, kw, n) != 0) return 0;
    return s[n] == 0 || isspace((unsigned char)s[n]);
}

/* substitute %1..%n / parameter names, then assemble each body line */
/* LOCAL label suffixes must be reproducible: the sizing passes and the
 * final emit pass each re-expand every macro, so the counter is reset at
 * the start of every pass (otherwise pass 2 references ??l0003 while the
 * label was defined as ??l0000, giving a bogus "bad target"). */
static unsigned long macro_serial;

static void asm_reset_macro_serial(void)
{
    macro_serial = 0;
}

static void expand_macro(asm_t *a, const macro_t *m, const char *args)
{
    char av[ASM_MAX_MPARAM][64];
    char lname[ASM_MAX_MPARAM][32];      /* LOCAL label -> unique name */
    char luniq[ASM_MAX_MPARAM][32];
    int  na = 0, nl = 0, i, j;
    const char *p = args;

    while (*p && na < ASM_MAX_MPARAM) {
        int k = 0;
        p += strspn(p, " \t,");
        if (!*p) break;
        while (*p && *p != ',' && k < 63) av[na][k++] = *p++;
        while (k > 0 && isspace((unsigned char)av[na][k - 1])) k--;
        av[na][k] = 0;
        na++;
    }

    /* "LOCAL a, b, c" makes each expansion's labels distinct.  A macro may
     * declare locals over several LOCAL lines (emu8086.inc's
     * DEFINE_SCAN_NUM uses four), so keep scanning rather than stopping at
     * the first one. */
    for (i = 0; i < m->nlines; i++) {
        const char *q = m->body[i];
        if (strncasecmp(q, "local", 5) != 0) continue;
        if (q[5] && !isspace((unsigned char)q[5])) continue;
        q = trim((char *)q + 5);
        while (*q && nl < ASM_MAX_MPARAM) {
            int k = 0;
            q += strspn(q, " \t,");
            if (!*q) break;
            while (*q && *q != ',' && !isspace((unsigned char)*q) && k < 31)
                lname[nl][k++] = *q++;
            while (k > 0 && isspace((unsigned char)lname[nl][k - 1])) k--;
            lname[nl][k] = 0;
            lower_str(lname[nl]);
            if (lname[nl][0]) {
                snprintf(luniq[nl], sizeof(luniq[nl]), "??l%04lu", macro_serial);
                macro_serial++;      /* each LOCAL name needs its own tag */
                nl++;
            }
        }
    }

    for (i = 0; i < m->nlines; i++) {
        char line[ASM_MAX_LINE];
        const char *src = m->body[i];
        int o = 0;

        /* a LOCAL declaration line emits nothing */
        if (nl && strncasecmp(src, "local", 5) == 0 &&
            (!src[5] || isspace((unsigned char)src[5]))) continue;

        while (*src && o < (int)sizeof(line) - 64) {
            int matched = 0;

            /* never rewrite inside a quoted string */
            if (*src == '\'' || *src == '"') {
                char q = *src;
                line[o++] = *src++;
                while (*src && *src != q && o < (int)sizeof(line) - 2)
                    line[o++] = *src++;
                if (*src == q) line[o++] = *src++;
                continue;
            }
            /* only substitute whole words: the previous byte must not be
             * part of an identifier (else "mov" would lose its "v") */
            if (o > 0 && (isalnum((unsigned char)line[o - 1]) || line[o - 1] == '_')) {
                line[o++] = *src++;
                continue;
            }
            /* LOCAL labels take precedence over parameters */
            for (j = 0; j < nl; j++) {
                size_t n = strlen(lname[j]);
                if (!strncmp(src, lname[j], n) &&
                    !(isalnum((unsigned char)src[n]) || src[n] == '_')) {
                    o += snprintf(line + o, sizeof(line) - (size_t)o, "%s", luniq[j]);
                    src += n;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            for (j = 0; j < m->nparam && j < na; j++) {
                size_t n = strlen(m->param[j]);
                if (!strncmp(src, m->param[j], n) &&
                    !(isalnum((unsigned char)src[n]) || src[n] == '_')) {
                    o += snprintf(line + o, sizeof(line) - (size_t)o, "%s", av[j]);
                    src += n;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            line[o++] = *src++;
        }
        line[o] = 0;
        process_line(a, line, a->errline);
    }
    /* the per-name increments above already advanced macro_serial */
}

void process_line(asm_t *a, char *raw, int lineno)
{
    extern int asm_saw_end;
    char *s = raw;

    if (asm_saw_end) return;
    char *label = NULL;
    char labelbuf[64] = {0};

    a->errline = lineno;

    strip_comment(s);
    s = trim(s);
    if (!*s) return;

    /* ---- macro collection / expansion ------------------------------- */
    if (a->in_macro) {
        /* ENDM may stand alone or carry the macro name in front of it
         * (`DEFINE_SCAN_NUM ENDM`), which is how emu8086.inc writes it. */
        if (is_endm(s)) {
            a->in_macro = 0;
        } else if (a->nmac > 0 && a->mac[a->nmac - 1].nlines < ASM_MAX_MLINES) {            macro_t *m = &a->mac[a->nmac - 1];
            snprintf(m->body[m->nlines], ASM_MAX_LINE, "%s", s);
            m->nlines++;
        }
        return;
    }
    {
        char head[64];
        char *r2;
        int n2 = 0;
        while (s[n2] && !isspace((unsigned char)s[n2]) && n2 < 63) { head[n2] = s[n2]; n2++; }
        head[n2] = 0;
        r2 = trim(s + n2);
        lower_str(head);
        if (starts_ci(r2, "macro")) {
            macro_t *m;
            char *p2;
            if (a->nmac >= ASM_MAX_MACROS) return;
            m = &a->mac[a->nmac++];
            memset(m, 0, sizeof(*m));
            lower_str(head);
            snprintf(m->name, sizeof(m->name), "%s", head);
            p2 = trim(r2 + 5);
            while (*p2 && m->nparam < ASM_MAX_MPARAM) {
                int k = 0;
                p2 = trim(p2);
                if (!*p2) break;
                while (*p2 && !isspace((unsigned char)*p2) && *p2 != ',' && k < 31)
                    m->param[m->nparam][k++] = *p2++;
                m->param[m->nparam][k] = 0;
                lower_str(m->param[m->nparam]);
                m->nparam++;
                p2 += strspn(p2, " 	,");
            }
            a->in_macro = 1;
            return;
        }
        {
            int mi;
            for (mi = 0; mi < a->nmac; mi++)
                if (!strcmp(a->mac[mi].name, head) && !*r2) break;
            if (mi < a->nmac) { expand_macro(a, &a->mac[mi], ""); return; }
            for (mi = 0; mi < a->nmac; mi++)
                if (!strcmp(a->mac[mi].name, head)) break;
            if (mi < a->nmac) { expand_macro(a, &a->mac[mi], r2); return; }
        }
    }

    /* leading label:  name:  or  name */
    {
        char *colon = strchr(s, ':');
        char *sp = s;
        while (*sp && !isspace((unsigned char)*sp) && *sp != ':') sp++;
        if (colon && colon == sp) {
            size_t n = (size_t)(colon - s);
            char namebuf[64];
            if (n >= sizeof(namebuf)) n = sizeof(namebuf) - 1;
            memcpy(namebuf, s, n);
            namebuf[n] = 0;
            if (is_ident(namebuf)) {
                snprintf(labelbuf, sizeof(labelbuf), "%s", namebuf);
                lower_str(namebuf);
                snprintf(a->last_label, sizeof(a->last_label), "%s", namebuf);
                label = labelbuf;
                s = trim(colon + 1);
            }
        }
    }

    if (label) {
        lower_str(label);
        {
            label_t *ex = find_label(a, label);
            if (!ex) {
                if (!add_label(a, label, (uint16_t)(a->ip), 0, 0))
                    asm_error(a, "too many labels");
            } else if (a->sizing) {
                ex->value = (uint16_t)(a->ip);
                ex->pass  = 1;
            }
            /* pass 2 must not move a label: pass 1 owns the addresses */
        }
    }

    if (!*s) return;

    /* directive or mnemonic */
    {
        char word[64];
        char *rest;
        int n = 0;
        while (s[n] && !isspace((unsigned char)s[n]) && n < 63) { word[n] = s[n]; n++; }
        word[n] = 0;
        rest = trim(s + n);

        lower_str(word);

        /* "name db ..." is a data label, not a label: */
        {
            char nxt[32];
            int k = 0;
            char *p = rest;
            while (*p && !isspace((unsigned char)*p) && k < 31) { nxt[k++] = *p; p++; }
            nxt[k] = 0;
            lower_str(nxt);
            if (!strcmp(nxt, "db") || !strcmp(nxt, "dw") || !strcmp(nxt, "dd") ||
                !strcmp(nxt, "equ") || !strcmp(nxt, "label") || !strcmp(nxt, "proc") ||
                !strcmp(nxt, "endp") || !strcmp(nxt, "ends") ||
                !strcmp(nxt, "segment") || !strcmp(nxt, "group")) {
                /* "name endp" / "name ends" close a block: no label move */
                if (!strcmp(nxt, "endp") || !strcmp(nxt, "ends")) return;
                label_t *ex = find_label(a, word);
                lower_str(word);
                snprintf(a->last_label, sizeof(a->last_label), "%s", word);
                if (!ex) add_label(a, word, (uint16_t)(a->ip), 0, 0);
                else if (a->sizing) ex->value = (uint16_t)(a->ip);
                if (!strcmp(nxt, "equ")) {
                    int ok, v = eval_expr(a, p, &ok);
                    if (ok) {
                        label_t *el = find_label(a, word);
                        if (!el) add_label(a, word, (uint16_t)v, -1, 0);
                        else if (a->sizing) { el->value = (uint16_t)v; el->size = -1; }
                    }
                    return;
                }
                if (!strcmp(nxt, "segment") || !strcmp(nxt, "group")) return;
                if (!strcmp(nxt, "proc")) {
                    label_t *pe = find_label(a, word);
                    if (!pe) add_label(a, word, (uint16_t)(a->ip), 0, 1);
                    else if (a->sizing) { pe->value = (uint16_t)(a->ip); pe->is_proc = 1; }
                    snprintf(a->last_label, sizeof(a->last_label), "%s", word);
                    return;
                }
                snprintf(word, sizeof(word), "%s", nxt);
                rest = trim(p);
            }
        }

        if (!strcmp(word, "org")) {
            int ok, v = eval_expr(a, rest, &ok);
            if (!ok) { asm_line_error(a, lineno, "bad org value"); return; }
            a->saw_org = 1;
            /* ORG makes the location counter absolute: labels and jump
             * targets are final addresses, while the emitted byte count
             * starts again from zero. */
            a->base   = v;
            a->org    = v;
            a->outlen = 0;
            a->ip     = v;
            return;
        }
        if (!strcmp(word, "db") || !strcmp(word, "dw") || !strcmp(word, "dd")) {
            emit_data(a, word, rest);
            return;
        }
        if (!strcmp(word, "equ")) { return; }
        if (!strcmp(word, "end")) { extern int asm_saw_end; asm_saw_end = 1; return; }
        if (!strcmp(word, "proc")) {
            /* "main proc": register the entry label, no bytes emitted */
            label_t *ex = find_label(a, a->last_label);
            if (ex && a->sizing) ex->is_proc = 1;
            return;
        }
        if (!strcmp(word, "endp") || !strcmp(word, "ends")) return;
        if (!strcmp(word, ".data")) {
            /* In an EXE the data items are addressed within their own segment,
             * so their offsets restart at 0 (the segment itself is reached via
             * `mov ax,@data` = 0x10).  div.exe_'s symbol table confirms:
             * MESSAGE 00000, RESULTAT 00012. */
            if (a->saw_model && !a->saw_org && a->outlen) {
                a->base = 0;
                a->ip   = 0;
            }
            define_const(a, "@data", 0x10);
            return;
        }
        if (!strcmp(word, ".code")) {
            /* the code segment starts on the next 16-byte boundary */
            /* only pad when data was laid out at the start (no ORG yet) */
            if (a->outlen && !a->saw_code && !a->saw_org && (a->ip & 15)) {
                int pad = (16 - (a->ip & 15)) & 15;
                while (pad--) emit8(a, 0x00);
            }
            a->saw_code = 1;
            if (!a->code_start) a->code_start = a->outlen;
            define_const(a, "@code", 0x00);
            return;
        }
        if (!strcmp(word, ".model")) { a->saw_model = 1; return; }
        if (!strcmp(word, ".stack")) {
            /* `.stack 100h` reserves the stack segment at the start of the
             * image; emu8086 fills it with 0x54 ('T').  The segment is laid
             * out before .data/.code, so the reservation happens here. */
            int v = 0, ok = 0;
            int n = eval_expr(a, rest, &ok);
            if (ok && n > 0) v = n;
            define_const(a, "@stack", 0);
            if (v > 0 && !a->saw_org) {
                while (v--) emit8(a, 0x54);
                a->stack_size = a->outlen;   /* stack occupies [0, outlen) */
            }
            return;
        }
        if (!strcmp(word, "ends") ||
            !strcmp(word, ".code") || !strcmp(word, ".data") ||
            !strcmp(word, ".stack") || !strcmp(word, "assume") || !strcmp(word, "segment") ||
            !strcmp(word, "name") || !strcmp(word, "title") || !strcmp(word, "page") ||
            !strcmp(word, "public") || !strcmp(word, "extrn") || !strcmp(word, "include") ||
            !strcmp(word, "option") || !strcmp(word, "uses") || !strcmp(word, "proc")) {
            return;
        }
        if (!strcmp(word, ".186") || !strcmp(word, ".286") || !strcmp(word, ".8086")) return;
        if (!strcmp(word, ".exit")) {                   /* MASM-style exit */
            emit8(a, 0xB8); emit16(a, 0x4C00); emit8(a, 0xCD); emit8(a, 0x21);
            return;
        }

        /* emu8086 control comments such as "#make_bin#" carry no code */
        if (word[0] == '#' && !strchr(rest, '=')) return;

        /* "#name=value" equ form: define a symbol, emit nothing */
        if (word[0] == '#' || strchr(rest, '=')) {
            char *eq = strchr(word, '=');
            if (!eq) eq = strchr(rest, '=');
            if (eq) {
                char nm[64];
                int ok, v;
                size_t n = (size_t)(eq - (eq >= word + strlen(word) ? rest : word));
                const char *src = (strchr(word, '=') ? word : rest);
                n = (size_t)(eq - src);
                if (n >= sizeof(nm)) n = sizeof(nm) - 1;
                memcpy(nm, src, n); nm[n] = 0;
                lower_str(nm);
                if (nm[0] == '#') memmove(nm, nm + 1, strlen(nm));
                v = eval_expr(a, eq + 1, &ok);
                if (ok && nm[0]) {
                    if (!find_label(a, nm)) add_label(a, nm, (uint16_t)v, 0, 0);
                    else find_label(a, nm)->value = (uint16_t)v;
                }
                return;
            }
        }

        /* instruction */
        {
            char *ops[8];
            int nops = split_operands(rest, ops, 8);
            encode_insn(a, word, ops, nops);
        }
    }
}

int asm_saw_end = 0;
static int asm_last_saw_model = 0;
static int asm_last_saw_org   = 0;
static int asm_last_stack     = 0;
static int asm_last_cstart    = 0;

/* =====================================================================
 * Two-pass driver
 * ===================================================================*/
static int pass_over(asm_t *a, char **lines, int nlines)
{
    int i;
    a->ip = 0;
    a->saw_code = 0;
    a->saw_org  = 0;
    a->saw_model = 0;
    a->stack_size = 0;
    a->code_start = 0;
    a->nmac     = 0;          /* macros are re-collected every pass */
    a->in_macro = 0;
    asm_reset_macro_serial(); /* LOCAL names must be identical every pass */
    a->outlen = 0;
    for (i = 0; i < nlines; i++) {
        char copy[ASM_MAX_LINE];
        snprintf(copy, sizeof(copy), "%s", lines[i]);
        process_line(a, copy, i + 1);
    }
    return a->errcount;
}

/* ---- source loading with `include` ---------------------------------- *
 *
 * emu8086 splices an included file in place of the `include` line, so the
 * included macros are defined exactly as if they had been typed there.  Bare
 * file names are resolved against the assembler's own inc\ directory.
 */
typedef struct {
    char **line;
    int    n;
    int    cap;
} srcbuf_t;

static void sb_push(srcbuf_t *sb, const char *s)
{
    if (sb->n >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 512;
        sb->line = (char **)realloc(sb->line, sizeof(char *) * (size_t)sb->cap);
    }
    sb->line[sb->n] = (char *)malloc(strlen(s) + 1);
    strcpy(sb->line[sb->n], s);
    sb->n++;
}

/* Find `name`: as given, next to the including file, then inc\ folders. */
static int resolve_include(const char *name, const char *cur_dir, char *out, int outsz)
{
    static const char *dirs[] = {
        "", ".", "inc", "..\\inc", "C:\\emu8086\\inc", NULL
    };
    int i;
    FILE *f;

    for (i = 0; dirs[i]; i++) {
        /* cur_dir can be as long as out, so build "dir/name" with an explicit
         * precision: a plain "%s/%s" could overflow silently. */
        int n;
        if (i == 0) n = snprintf(out, outsz, "%s", name);
        else if (i == 1 && cur_dir && *cur_dir)
            n = snprintf(out, outsz, "%.*s/%s", outsz - (int)strlen(name) - 2,
                         cur_dir, name);
        else n = snprintf(out, outsz, "%s/%s", dirs[i], name);
        if (n < 0 || n >= outsz) continue;      /* too long for this search */
        f = fopen(out, "r");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

static void load_source(srcbuf_t *sb, const char *path, int depth)
{
    FILE *f;
    char buf[ASM_MAX_LINE];
    char cur_dir[512] = {0};
    const char *slash;

    if (depth > 8)                 /* guard against pathological recursion */
        return;

    slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    if (slash && (size_t)(slash - path) < sizeof(cur_dir)) {
        memcpy(cur_dir, path, (size_t)(slash - path));
        cur_dir[slash - path] = 0;
    }

    f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(buf, sizeof(buf), f)) {
        size_t l = strlen(buf);
        char probe[ASM_MAX_LINE];
        char *p;

        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;

        /* recognise `include "name"` / `include name` */
        snprintf(probe, sizeof(probe), "%s", buf);
        for (p = probe; *p; p++) *p = (char)tolower((unsigned char)*p);
        p = probe;
        while (*p == ' ' || *p == '\t') p++;

        if (!strncmp(p, "include", 7) && (p[7] == ' ' || p[7] == '\t')) {
            char name[512], full[512];
            const char *q = p + 7;
            int k = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"' || *q == '\'') {
                char quote = *q++;
                while (*q && *q != quote && k < (int)sizeof(name) - 1) name[k++] = *q++;
            } else {
                while (*q && *q != ' ' && *q != '\t' && k < (int)sizeof(name) - 1) name[k++] = *q++;
            }
            name[k] = 0;
            if (k && resolve_include(name, cur_dir, full, sizeof(full))) {
                load_source(sb, full, depth + 1);
                continue;           /* the include line itself emits nothing */
            }
        }

        sb_push(sb, buf);
    }
    fclose(f);
}

int asm_assemble_file(const char *path, int want_com, uint8_t **out, int *outlen,
                      char *errbuf, int errbufsz)
{
    FILE *f = fopen(path, "r");
    static asm_t a;             /* large: keep it out of the stack */
    char **lines = NULL;
    int nlines = 0;

    if (!f) { snprintf(errbuf, errbufsz, "cannot open %s", path); return 0; }

    memset(&a, 0, sizeof(a));
    a.org  = want_com ? 0x100 : 0;
    a.base = a.org;

    fclose(f);

    /* Splice every `include "file"` into the statement stream before any pass
     * runs, so the sizing loop and the emit pass both see one merged source. */
    {
        srcbuf_t sb;
        sb.line = NULL; sb.n = 0; sb.cap = 0;
        load_source(&sb, path, 0);
        lines  = sb.line;
        nlines = sb.n;
    }

    /* Decide the load origin up front: an MZ EXE (`.model` with no `org`) is
     * addressed from offset 0, while a .COM image starts at 0x100.  The
     * per-pass scans need this before they run, so look for the two directives
     * directly in the collected source. */
    {
        int has_model = 0, has_org = 0, i;
        for (i = 0; i < nlines; i++) {
            char probe[ASM_MAX_LINE];
            char *p;
            snprintf(probe, sizeof(probe), "%s", lines[i]);
            for (p = probe; *p; p++) *p = (char)tolower((unsigned char)*p);
            p = probe;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, ".model", 6)) has_model = 1;
            if (!strncmp(p, "org", 3))     has_org   = 1;
        }
        if (has_model && !has_org) want_com = 0;
    }

    /* The original is a two-pass assembler, but short/long jump selection
     * makes instruction sizes depend on addresses that pass 1 is still
     * discovering.  Iterate the sizing pass until lengths stop changing,
     * then do the final emitting pass. */
    {
        static uint8_t prev[1 << 20];
        int prev_len = -1, round;
        for (round = 0; round < 12; round++) {
            asm_saw_end = 0;
            /* Size with the same rules the emit pass uses.  Sizing as "pass 1"
             * made forward jumps take a fixed wide form, so pass 1 converged on
             * a smaller image than pass 2 produced and labels ended up stale. */
            a.pass   = (round == 0) ? 1 : 2;
            a.sizing = 1;
            a.base   = want_com ? 0x100 : 0;
            a.ip     = 0;
            a.outlen = 0;
            a.errcount = 0;
            pass_over(&a, lines, nlines);
            if (a.outlen == prev_len &&
                (a.outlen == 0 || memcmp(prev, a.out, (size_t)a.outlen) == 0))
                break;
            if (a.out) memcpy(prev, a.out, (size_t)a.outlen);
            prev_len = a.outlen;
        }
    }

    /* The sizing loop has converged on a byte-identical image, so its buffer is
     * the answer: there is no separate emit pass that could disagree with the
     * labels it recorded.  (A distinct pass 2 used to emit 7 bytes more than
     * the sizing pass on include.asm, leaving every label 7 bytes stale.) */
    {
        int i; for (i = 0; i < nlines; i++) free(lines[i]); free(lines);
    }

    if (a.errcount) {
        snprintf(errbuf, errbufsz, "%s", a.err[0] ? a.err : "assembly failed");
        free(a.out);
        return 0;
    }

    *out    = a.out;
    *outlen = a.outlen;
    asm_last_saw_model = a.saw_model;
    asm_last_saw_org   = a.saw_org;
    asm_last_stack     = a.stack_size;
    asm_last_cstart    = a.code_start;
    return 1;
}

/* Was the most recent asm_assemble_file() source an EXE-model program?
 *
 * A source is an MZ EXE only when it declares `.model` *and* has no ORG: a
 * program with `org 100h` is a .COM image even if it carries `.model`
 * (div.com_ vs div.exe_ are exactly this pair). */
int asm_last_was_exe(void)
{
    return asm_last_saw_model && !asm_last_saw_org;
}

int asm_last_stack_size(void)
{
    return asm_last_stack;
}

int asm_last_code_start(void)
{
    return asm_last_cstart;
}
