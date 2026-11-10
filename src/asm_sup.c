/* mue — assembler support: diagnostics, emission, labels, expressions
 * asm_sup.c
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include "asm.h"

void asm_error(asm_t *a, const char *fmt, ...)
{
    va_list ap;
    char buf[400];

    /* pass 1 only measures sizes: forward references are expected to fail,
     * so diagnostics are collected in pass 2 only. */
    if (a->pass != 2 && !getenv("ASM_DEBUG")) return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (a->errcount == 0 && a->err[0] == 0) {
        snprintf(a->err, sizeof(a->err), "%s", buf);
    }
    a->errcount++;
}

void asm_line_error(asm_t *a, int lineno, const char *fmt, ...)
{
    va_list ap;
    char buf[400], all[512];
    if (a->pass != 2 && !getenv("ASM_DEBUG")) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    snprintf(all, sizeof(all), "line %d: %s", lineno, buf);
    asm_error(a, "%s", all);
}

/* ---- emission -------------------------------------------------------- */
static void out_grow(asm_t *a, int need)
{
    if (a->outlen + need <= a->outcap) return;
    while (a->outcap < a->outlen + need) a->outcap = a->outcap ? a->outcap * 2 : 4096;
    a->out = (uint8_t *)realloc(a->out, (size_t)a->outcap);
}

void emit8(asm_t *a, uint8_t v)
{
    out_grow(a, 1);
    a->out[a->outlen++] = v;
    a->ip++;
}

void emit16(asm_t *a, uint16_t v)
{
    emit8(a, (uint8_t)(v & 0xFF));
    emit8(a, (uint8_t)(v >> 8));
}

void emit_bytes(asm_t *a, const uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++) emit8(a, p[i]);
}

/* ---- labels ---------------------------------------------------------- */
label_t *find_label(asm_t *a, const char *name)
{
    int i;
    char low[64];
    int n = 0;
    /* symbol names are case-insensitive */
    while (name[n] && n < 63) { low[n] = (char)tolower((unsigned char)name[n]); n++; }
    low[n] = 0;
    for (i = 0; i < a->nlab; i++)
        if (strcmp(a->lab[i].name, low) == 0) return &a->lab[i];
    return NULL;
}

int add_label(asm_t *a, const char *name, uint16_t value, int size, int is_proc)
{
    label_t *l;
    if (a->nlab >= ASM_MAX_LABELS) return 0;
    l = &a->lab[a->nlab++];
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->value   = value;
    l->size    = size;
    l->is_proc = is_proc;
    l->seg     = LCODE;
    l->pass    = a->pass;
    return 1;
}

/* ---- operand splitting ----------------------------------------------- */
/* Split "a, b, c" but not ", 5" inside brackets or quotes. */
int split_operands(char *s, char **out, int max)
{
    int n = 0, depth = 0, q = 0;
    char *p = s;

    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        out[n++] = p;
        while (*p) {
            if (q) {
                if (*p == q) q = 0;
            } else if (*p == '\'' || *p == '"') {
                q = *p;
            } else if (*p == '[') {
                depth++;
            } else if (*p == ']') {
                depth--;
            } else if (*p == ',' && depth == 0) {
                *p++ = 0;
                break;
            }
            p++;
        }
    }
    /* trim trailing blanks on each operand */
    {
        int i;
        for (i = 0; i < n; i++) {
            char *e = out[i] + strlen(out[i]);
            while (e > out[i] && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        }
    }
    return n;
}

/* ---- expression evaluation ------------------------------------------- */
/* Recursive descent:  || && | ^ &  + -  * /  unary - ~  ( )  number  'c'
 *  label    $   (current location counter)
 */
typedef struct { asm_t *a; const char *p; int ok; } ex_t;

static int e_or(ex_t *e);

static void skipws(ex_t *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }

static int hexval(int c)
{
    if (c == '_') return 0;                 /* digit separator */
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int e_primary(ex_t *e)
{
    skipws(e);

    if (*e->p == '(') {
        int v;
        e->p++;
        v = e_or(e);
        skipws(e);
        if (*e->p == ')') e->p++; else e->ok = 0;
        return v;
    }

    if (*e->p == '\'' || *e->p == '"') {
        int q = *e->p++;
        int v = (unsigned char)*e->p;
        if (*e->p) e->p++;
        while (*e->p && *e->p != q) e->p++;
        if (*e->p == q) e->p++;
        return v;
    }

    if (*e->p == '$') { e->p++; return e->a->ip; }

    if (isdigit((unsigned char)*e->p)) {
        int v = 0;
        /* 0x / 0X prefix */
        if (e->p[0] == '0' && (e->p[1] == 'x' || e->p[1] == 'X')) {
            e->p += 2;
            while (hexval((unsigned char)*e->p) >= 0) { v = v * 16 + hexval((unsigned char)*e->p); e->p++; }
            /* emu8086 tolerates a redundant trailing h, e.g. 0ffh */
            if (*e->p == 'h' || *e->p == 'H') e->p++;
            return v;
        }
        /* scan the run first so a trailing h/b/d can re-interpret it */
        {
            const char *start = e->p;
            unsigned hv = 0;
            int ndig = 0;
            /* a run of 0/1 digits followed by 'b' is binary, not hex */
            {
                const char *q = e->p;
                while (*q == '0' || *q == '1' || *q == '_') q++;
                /* require at least two digits so "0b800h" stays hex */
                if (q - e->p >= 2 && (*q == 'b' || *q == 'B')) {
                    int b = 0;
                    const char *r = e->p;
                    while (r < q) { if (*r != '_') b = b * 2 + (*r - '0'); r++; }
                    e->p = q + 1;
                    return b;
                }
            }
            while ((hexval((unsigned char)*e->p) >= 0 || *e->p == '_') && ndig < 12) {
                if (*e->p != '_') { hv = hv * 16 + (unsigned)hexval((unsigned char)*e->p); ndig++; }
                e->p++;
            }
            if (*e->p == 'h' || *e->p == 'H') { e->p++; return (int)(hv & 0xFFFF); }
            if (*e->p == 'b' || *e->p == 'B') {
                int b = 0, binoks = 1;
                const char *q = start;
                while (q < e->p) { if (*q != '0' && *q != '1' && *q != '_') { binoks = 0; break; } q++; }
                if (binoks) {
                    q = start;
                    while (q < e->p) { if (*q != '_') b = b * 2 + (*q - '0'); q++; }
                    e->p++;
                    return b;
                }
                e->p++;            /* trailing 'b' as a hex digit tail: not valid */
                return (int)(hv & 0xFFFF);
            }
            if (*e->p == 'd' || *e->p == 'D') { e->p++; return (int)(hv & 0xFFFF); }
            /* decimal only if every consumed char was 0-9 */
            {
                int all_dec = 1;
                const char *q = start;
                while (q < e->p) { if (!isdigit((unsigned char)*q) && *q != '_') { all_dec = 0; break; } q++; }
                if (all_dec) {
                    e->p = start;
                    while (isdigit((unsigned char)*e->p) || *e->p == '_') { if (*e->p != '_') v = v * 10 + (*e->p - '0'); e->p++; }
                    return v;
                }
            }
            /* bare hex containing a-f, e.g. 0ABh was handled above; treat as hex */
            return (int)(hv & 0xFFFF);
        }
    }

    /* "offset sym" and "seg sym" both evaluate to the symbol's offset here */
    {
        static const char *kw[] = { "offset", "seg", "loffset", NULL };
        int i;
        for (i = 0; kw[i]; i++) {
            size_t n = strlen(kw[i]);
            if (strncmp(e->p, kw[i], n) == 0 && isspace((unsigned char)e->p[n])) {
                e->p += n;
                skipws(e);
                break;
            }
        }
    }

    if (isalpha((unsigned char)*e->p) || *e->p == '.' || *e->p == '@' || *e->p == '?') {
        char name[64];
        int n = 0;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_' || *e->p == '.' ||
                *e->p == '@' || *e->p == '?') && n < 63)
            name[n++] = *e->p++;
        name[n] = 0;
        {
            label_t *l = find_label(e->a, name);
            if (l) return l->value;
        }
        e->ok = 0;
        return 0;
    }

    e->ok = 0;
    return 0;
}

static int e_unary(ex_t *e)
{
    skipws(e);
    if (*e->p == '-') { e->p++; return -e_unary(e); }
    if (*e->p == '+') { e->p++; return  e_unary(e); }
    if (*e->p == '~' || *e->p == '!') { e->p++; return ~e_unary(e); }
    return e_primary(e);
}

static int e_mul(ex_t *e)
{
    int v = e_unary(e);
    for (;;) {
        skipws(e);
        if (*e->p == '*') { e->p++; v = v * e_unary(e); }
        else if (*e->p == '/') { e->p++; { int d = e_unary(e); v = d ? v / d : 0; } }
        else if (*e->p == '%' || (*e->p == 'm' && e->p[1] == 'o' && e->p[2] == 'd')) {
            if (*e->p == '%') e->p++; else e->p += 3;
            { int d = e_unary(e); v = d ? v % d : 0; }
        }
        else break;
    }
    return v;
}

static int e_add(ex_t *e)
{
    int v = e_mul(e);
    for (;;) {
        skipws(e);
        if (*e->p == '+') { e->p++; v = v + e_mul(e); }
        else if (*e->p == '-') { e->p++; v = v - e_mul(e); }
        else break;
    }
    return v;
}

static int e_shift(ex_t *e);

static int e_and(ex_t *e)
{
    int v = e_shift(e);
    for (;;) {
        skipws(e);
        if (*e->p == '&' && e->p[1] != '&') { e->p++; v = v & e_shift(e); }
        else break;
    }
    return v;
}

static int e_shift(ex_t *e)
{
    int v = e_add(e);
    for (;;) {
        skipws(e);
        if (e->p[0] == '<' && e->p[1] == '<') { e->p += 2; v = (int)((unsigned)v << (e_add(e) & 31)); }
        else if (e->p[0] == '>' && e->p[1] == '>') { e->p += 2; v = (int)((unsigned)v >> (e_add(e) & 31)); }
        else break;
    }
    return v;
}

static int e_xor(ex_t *e)
{
    int v = e_and(e);
    for (;;) {
        skipws(e);
        if (*e->p == '^') { e->p++; v = v ^ e_and(e); }
        else break;
    }
    return v;
}

static int e_or(ex_t *e)
{
    int v = e_xor(e);
    for (;;) {
        skipws(e);
        if (*e->p == '|' && e->p[1] != '|') { e->p++; v = v | e_xor(e); }
        else break;
    }
    return v;
}

int eval_expr(asm_t *a, const char *s, int *ok)
{
    ex_t e;
    int v;
    e.a = a; e.p = s; e.ok = 1;
    v = e_or(&e);
    skipws(&e);
    if (*e.p) e.ok = 0;             /* trailing junk */
    if (!e.ok) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    return v;
}
