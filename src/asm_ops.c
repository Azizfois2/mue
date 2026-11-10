/* mue — assembler: operand model, register tables, ModRM builder
 * asm_ops.c
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "asm.h"
#include "asm_ops.h"

/* ---- register tables ------------------------------------------------- */
static const char *R8[8]  = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *R16[8] = { "ax","cx","dx","bx","sp","bp","si","di" };
static const char *SR[4]  = { "es","cs","ss","ds" };

int reg8_index(const char *s)
{
    int i;
    for (i = 0; i < 8; i++) if (strcmp(s, R8[i]) == 0) return i;
    return -1;
}

int reg16_index(const char *s)
{
    int i;
    for (i = 0; i < 8; i++) if (strcmp(s, R16[i]) == 0) return i;
    return -1;
}

int sreg_index(const char *s)
{
    int i;
    for (i = 0; i < 4; i++) if (strcmp(s, SR[i]) == 0) return i;
    return -1;
}

/* ---- lower-cased copy of an operand ---------------------------------- */
/* Lower-case everything except the inside of character literals, which
 * carry the actual byte value the program wants. */
static void lower(char *dst, const char *src, int cap)
{
    int i, q = 0;
    for (i = 0; src[i] && i < cap - 1; i++) {
        char c = src[i];
        if (q) {
            dst[i] = c;
            if (c == q) q = 0;
        } else if (c == '\'' || c == '"') {
            q = c;
            dst[i] = c;
        } else {
            dst[i] = (char)tolower((unsigned char)c);
        }
    }
    dst[i] = 0;
}

static const char *trim_copy(const char *s)
{
    static char buf[256];
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '	')) n--;
    while (*s == ' ' || *s == '	') { s++; n--; }
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

/* ---- operand classification ------------------------------------------ */
/* Recognised size keywords in front of a memory operand. */
static int size_keyword(const char *s, int *size, const char **rest)
{
    static const struct { const char *k; int sz; } keys[] = {
        { "byte ptr ",   8 }, { "dword ptr ", 32 },
        { "word ptr ",  16 }, { "far ptr ",   32 },
        { "near ptr ",  16 }, { "qword ptr ", 64 },
        { "byte ",  8 }, { "word ", 16 }, { "dword ", 32 },
        { "far ",  32 }, { "near ", 16 },
    };
    unsigned i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t n = strlen(keys[i].k);
        if (strncmp(s, keys[i].k, n) == 0) {
            *size = keys[i].sz;
            *rest = s + n;
            return 1;
        }
    }
    return 0;
}

int parse_operand(asm_t *a, const char *text, operand_t *op)
{
    char buf[256], low[256];
    const char *rest;

    memset(op, 0, sizeof(*op));
    op->seg = -1;                 /* no segment override by default */
    op->base = -1;
    op->index = -1;

    snprintf(buf, sizeof(buf), "%s", text);
    lower(low, buf, sizeof(low));
    op->text = text;

    /* size override */
    op->size = 0;
    rest = low;
    {
        int sz;
        if (size_keyword(low, &sz, &rest)) op->size = sz;
    }

    /* registers */
    {
        int r;
        if ((r = reg16_index(rest)) >= 0) { op->kind = OP_REG16; op->reg = r; op->size = 16; return 1; }
        if ((r = reg8_index (rest)) >= 0) { op->kind = OP_REG8;  op->reg = r; op->size = 8;  return 1; }
        if ((r = sreg_index(rest))  >= 0) { op->kind = OP_SREG;  op->reg = r; op->size = 16; return 1; }
    }

    /* string instruction operands */
    if (!strcmp(rest, "byte ptr es:[di]") || !strcmp(rest, "es:[di]")) {
        op->kind = OP_MEM; op->base = -1; op->index = -1;
        op->size = op->size ? op->size : 8; op->moffs = 0;
        return 1;
    }

    /* memory: [ ... ] or expr[ ... ] */
    {
        const char *lb = strchr(rest, '[');
        if (lb) {
            char inside[256], before[256];
            const char *rb = strrchr(lb, ']');
            size_t bl = (size_t)(lb - rest);
            int ok;
            if (bl >= sizeof(before)) bl = sizeof(before) - 1;
            memcpy(before, rest, bl); before[bl] = 0;

            /* optional segment override "es:" etc. */
            {
                char segbuf[8] = {0};
                const char *c = strchr(before, ':');
                if (c) {
                    size_t n = (size_t)(c - before);
                    if (n < sizeof(segbuf)) { memcpy(segbuf, before, n); segbuf[n] = 0; }
                    op->seg = sreg_index(segbuf);
                    memmove(before, c + 1, strlen(c + 1) + 1);
                }
            }

            snprintf(inside, sizeof(inside), "%.*s",
                     rb ? (int)(rb - lb - 1) : (int)strlen(lb + 1), lb + 1);

            op->kind = OP_MEM;
            op->base = -1;
            op->index = -1;
            op->disp = 0;
            op->has_disp = 0;
            op->moffs = 0;

            /* a label in front of the bracket ("msg[si]") becomes a bias */
            {
                char pre[256];
                snprintf(pre, sizeof(pre), "%s", trim_copy(before));
                if (pre[0]) {
                    int pv = eval_expr(a, pre, &ok);
                    if (ok) { op->disp += pv; op->has_disp = 1; }
                }
            }

            /* A pure constant inside [] is a direct address; the original
             * accepts "mov ax,[1400h]" as moffs-style absolute. */
            {
                char t[256];
                snprintf(t, sizeof(t), "%s", inside);
                if (!strpbrk(t, "bxsidi+bp*")) {
                    int v = eval_expr(a, t, &ok);
                    if (ok) {
                        op->disp += v;
                        op->has_disp = 1;
                        /* moffs only when the address is a bare constant */
                        /* decided at the end, once base/index are known */
                    }
                } else {
                    /* [bx+si+4] / [bp-2] / [si] ... */
                    char *p = inside;
                    while (*p) {
                        int sign = 1, val = 0, got = 0;
                        char term[64];
                        int n = 0;
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p == '+') { p++; }
                        else if (*p == '-') { sign = -1; p++; }
                        while (*p == ' ' || *p == '\t') p++;
                        while (*p && *p != '+' && *p != '-' && *p != ' ' && *p != '\t' && n < 63)
                            term[n++] = *p++;
                        term[n] = 0;
                        if (!n) break;
                        {
                            int r;
                            if ((r = reg16_index(term)) >= 0 && (r == 3 || r == 5 || r == 6 || r == 7)) {
                                if (op->base < 0) op->base = r; else op->index = r;
                                got = 1;
                            }
                        }
                        if (!got) {
                            int v = eval_expr(a, term, &ok);
                            if (ok) val = v;
                        }
                        op->disp += sign * val;
                        op->has_disp = 1;
                    }
                }
            }
            if (op->base < 0 && op->index < 0 && op->has_disp) op->moffs = 1;
            if (!op->size) op->size = 0;
            return 1;
        }
    }

    /* bare expression: immediate, or a symbol used as a direct address */
    {
        int ok;
        int v = eval_expr(a, rest, &ok);
        if (ok) {
            /* A symbol used alone is a memory operand, but an "equ" constant
             * and an immediate expression are values.  `rest` may carry a
             * displacement ("buf+2"), so match its leading identifier --
             * but only when the operand is exactly that symbol plus a
             * displacement, never for a plain arithmetic expression. */
            char lead[256];
            size_t k = 0;
            while (rest[k] && (isalnum((unsigned char)rest[k]) || rest[k] == '_' ||
                               rest[k] == '@' || rest[k] == '$' || rest[k] == '?') &&
                   k < sizeof(lead) - 1)
                lead[k] = rest[k], k++;
            lead[k] = 0;

            label_t *l = k ? find_label(a, lead) : NULL;
            /* Only treat it as an address when the rest of the operand is a
             * pure +/- displacement.  "count*2" must stay an immediate. */
            int only_disp = 0;
            if (l) {
                const char *q = rest + k;
                only_disp = 1;
                while (*q) {
                    if (isspace((unsigned char)*q) || *q == '+' || *q == '-' ||
                        isalnum((unsigned char)*q) || *q == '_') q++;
                    else { only_disp = 0; break; }
                }
            }
            if (l && l->size >= 0 && only_disp) {
                op->kind = OP_MEM;
                op->disp = v;
                op->has_disp = 1;
                op->moffs = 1;
                op->base = -1;
                op->index = -1;
                return 1;
            }
            op->kind = OP_IMM;
            op->imm = v;
            return 1;
        }
    }
    return 0;
}

/* ---- ModRM encoding -------------------------------------------------- */
/* Returns 0 on success.  `regf` is the /reg field. */
int build_modrm(asm_t *a, const operand_t *op, int regf, int *out, int *outn)
{
    int n = 0;
    int mod, rm;
    static const int8_t bt[8] = { 3, 3, 5, 5, 6, 7, 5, 3 };   /* bx,bx,bp,bp,si,di,bp,bx */

    if (op->kind == OP_REG8 || op->kind == OP_REG16 || op->kind == OP_SREG) {
        out[n++] = (uint8_t)(0xC0 | (regf << 3) | (op->reg & 7));
        *outn = n;
        return 0;
    }

    if (op->kind != OP_MEM) return -1;

    /* direct address, no base/index */
    if (op->base < 0 && op->index < 0) {
        out[n++] = (uint8_t)(0x06 | (regf << 3));
        out[n++] = (uint8_t)(op->disp & 0xFF);
        out[n++] = (uint8_t)((op->disp >> 8) & 0xFF);
        *outn = n;
        return 0;
    }

    /* determine rm code from the base/index pair */
    {
        int b = op->base, i = op->index;
        if (b > i && (b == 6 || b == 7) && (i == 3 || i == 5)) { int t = b; b = i; i = t; }
        if (b < 0) { int t = b; b = i; i = t; }

        if      (b == 3 && i < 0) rm = 7;   /* [bx]    */
        else if (b == 5 && i < 0) rm = 6;   /* [bp]    */
        else if (b == 6 && i < 0) rm = 4;   /* [si]    */
        else if (b == 7 && i < 0) rm = 5;   /* [di]    */
        else if (b == 3 && i == 6) rm = 0;  /* [bx+si] */
        else if (b == 3 && i == 7) rm = 1;  /* [bx+di] */
        else if (b == 5 && i == 6) rm = 2;  /* [bp+si] */
        else if (b == 5 && i == 7) rm = 3;  /* [bp+di] */
        else return -1;

        (void)bt;

        if (!op->has_disp || op->disp == 0) {
            mod = 0;
            if (rm == 6) mod = 1;           /* [bp] needs disp8=0 */
        } else if (op->disp >= -128 && op->disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }

        out[n++] = (uint8_t)((mod << 6) | (regf << 3) | rm);
        if (mod == 1) out[n++] = (uint8_t)(op->disp & 0xFF);
        else if (mod == 2) { out[n++] = (uint8_t)(op->disp & 0xFF);
                             out[n++] = (uint8_t)((op->disp >> 8) & 0xFF); }
    }
    *outn = n;
    return 0;
}
