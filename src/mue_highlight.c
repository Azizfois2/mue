/* mue_highlight.c - 8086 source syntax highlighting for the editor.
 *
 * emu8086 colours its editor by token class, and the classes are visible in
 * the shipped screenshots:
 *
 *   instruction mnemonics   blue          mov, add, int, org
 *   registers               dark red      ax, al, cs, si
 *   numbers                 green         1234h, 0DEh, 10
 *   comments                grey          ; anything to end of line
 *   strings                 purple        'text' / "text"
 *   directives/labels       black         name:, .model, include
 *
 * The tokeniser is deliberately simple and line-local: 8086 source has no
 * block comments and no multi-line strings, so a per-line scan cannot get
 * out of step the way a stateful lexer can.
 *
 * Words are matched case-insensitively, because the assembler accepts both
 * `MOV` and `mov` and a highlighter that only knew one case would look
 * broken on half the examples in C:/emu8086/examples.
 */
#include <FL/Fl.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "mue_highlight.h"

/* ---- token classes ---------------------------------------------------- */

static const char *MNEMONICS[] = {
    /* data movement */
    "mov", "movsx", "movzx", "xchg", "lea", "lds", "les", "lahf", "sahf",
    "push", "pop", "pusha", "popa", "pushf", "popf", "xlat",
    "in", "out", "cbw", "cwd",
    /* arithmetic */
    "add", "adc", "sub", "sbb", "cmp", "inc", "dec", "neg", "mul", "imul",
    "div", "idiv", "daa", "das", "aaa", "aas", "aam", "aad",
    /* logic */
    "and", "or", "xor", "not", "test", "shl", "shr", "sal", "sar",
    "rol", "ror", "rcl", "rcr",
    /* control flow */
    "jmp", "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle",
    "jna", "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge", "jnl",
    "jnle", "jno", "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo", "js",
    "jz", "call", "ret", "retn", "retf", "loop", "loope", "loopne",
    "loopz", "loopnz", "jcxz", "int", "into", "iret", "nop", "hlt",
    "wait", "esc", "lock",
    /* string ops */
    "movs", "movsb", "movsw", "cmps", "cmpsb", "cmpsw",
    "scas", "scasb", "scasw", "lods", "lodsb", "lodsw",
    "stos", "stosb", "stosw", "rep", "repe", "repz", "repne", "repnz",
    /* flags */
    "clc", "stc", "cmc", "cld", "std", "cli", "sti",
    /* conditional byte set */
    "seta", "setae", "setb", "setbe", "setc", "sete", "setg", "setge",
    "setl", "setle", "setna", "setne", "setno", "setnp", "setns",
    "setnz", "seto", "setp", "sets", "setz",
    /* assembler directives that read like instructions */
    "org", "end", "db", "dw", "dd", "dq", "equ", "include", "proc",
    "endp", "ends", "segment", "assume", "offset", "ptr", "dup",
};

static const char *REGISTERS[] = {
    "ax", "bx", "cx", "dx", "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh",
    "si", "di", "sp", "bp", "ip", "cs", "ds", "es", "ss",
    "eax", "ebx", "ecx", "edx",
    "byte", "word", "dword", "qword", "far", "near", "short",
};

#define N_MNEM  ((int)(sizeof MNEMONICS / sizeof MNEMONICS[0]))
#define N_REG   ((int)(sizeof REGISTERS / sizeof REGISTERS[0]))

/* ---- the styles ------------------------------------------------------- */

/* FLTK styles must be 0..255 and A..Z must stay unused (FLTK reserves them),
 * so the classes start at 'a'. */
#define S_MNEM     "a"
#define S_REG      "b"
#define S_NUM      "c"
#define S_CMT      "d"
#define S_STR      "e"

/* The style table.  FLTK 1.4's Style_Table_Entry is
 *   { Fl_Color color; Fl_Font font; Fl_Fontsize size; unsigned attr;
 *     Fl_Color bgcolor; }
 * and highlight_data() takes a table of 256 entries indexed by the style
 * character, so this is a full 256-entry array with a..e filled in.
 *
 * Colours are RGB triples converted with fl_rgb_color, NOT literal 0xRRGGBB
 * values: Fl_Color is a table index, and passing a packed RGB value is what
 * once painted the whole UI cyan.
 */
Fl_Text_Display::Style_Table_Entry mue_style_table[256];

static void init_styles(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    Fl_Color plain = fl_rgb_color(32, 32, 32);        /* default text   */
    Fl_Color mnemo = fl_rgb_color(0, 0, 170);         /* instructions   */
    Fl_Color reg   = fl_rgb_color(160, 0, 0);         /* registers      */
    Fl_Color num   = fl_rgb_color(0, 128, 0);         /* numbers        */
    Fl_Color cmt   = fl_rgb_color(128, 128, 128);     /* comments       */
    Fl_Color str   = fl_rgb_color(128, 0, 128);       /* strings        */

    for (int i = 0; i < 256; i++) {
        mue_style_table[i].color   = plain;
        mue_style_table[i].font    = FL_COURIER;
        mue_style_table[i].size    = 13;
        mue_style_table[i].attr    = 0;
        mue_style_table[i].bgcolor = FL_WHITE;
    }
    mue_style_table['a'].color = mnemo;
    mue_style_table['b'].color = reg;
    mue_style_table['c'].color = num;
    mue_style_table['d'].color = cmt;
    mue_style_table['e'].color = str;
}

/* ---- the tokeniser ---------------------------------------------------- */

static int ci_equal(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int in_list(const char *w, int n, const char **list, int count)
{
    for (int i = 0; i < count; i++)
        if ((int)strlen(list[i]) == n && ci_equal(w, list[i], n))
            return 1;
    return 0;
}

static int is_word_start(int c)
{
    return isalpha(c) || c == '_' || c == '.' || c == '@' || c == '$';
}

static int is_word_char(int c)
{
    return isalnum(c) || c == '_' || c == '.' || c == '@' || c == '$' || c == '?';
}

/* Highlight one line into out[], which must have room for len+1. */
static void highlight_line(const char *src, char *out, int len)
{
    int i = 0;
    while (i < len) {
        int c = (unsigned char)src[i];

        /* comment to end of line */
        if (c == ';') {
            for (; i < len; i++) out[i] = 'd';
            break;
        }

        /* string or character literal */
        if (c == '\'' || c == '"') {
            int q = c;
            out[i++] = 'e';
            while (i < len) {
                out[i++] = 'e';
                if ((unsigned char)src[i - 1] == q) break;
            }
            continue;
        }

        /* number: digits, optionally followed by h/b/o/d or a decimal point */
        if (isdigit(c) || (c == '$' && i + 1 < len &&
                           isxdigit((unsigned char)src[i + 1]))) {
            out[i++] = 'c';
            while (i < len) {
                int d = (unsigned char)src[i];
                if (isalnum(d) || d == '.') out[i++] = 'c';
                else break;
            }
            continue;
        }

        /* identifier */
        if (is_word_start(c)) {
            int start = i;
            while (i < len && is_word_char((unsigned char)src[i])) i++;
            char cls = 'A';
            int n = i - start;
            if (in_list(src + start, n, MNEMONICS, N_MNEM))      cls = 'a';
            else if (in_list(src + start, n, REGISTERS, N_REG))  cls = 'b';
            else if (i < len && src[i] == ':')                   cls = 'A';
            for (int k = start; k < i; k++) out[k] = cls;
            continue;
        }

        out[i++] = 'A';
    }
    out[len] = 0;
}

/* ---- the public entry point ------------------------------------------- */

void mue_rehighlight(Fl_Text_Buffer *buf, Fl_Text_Buffer *style)
{
    if (!buf || !style) return;
    init_styles();

    /* Rebuild the whole style buffer. Doing it line by line keeps memory use
     * bounded and makes each line independent, so an edit cannot leave the
     * styling offset from the text. */
    style->text("");
    int ntext = buf->length();
    char *src = buf->text();
    if (!src) return;

    char *out = 0;
    int cap = 0;
    int nstyles = 0;
    for (int pos = 0; pos < ntext; ) {
        int start = pos;
        while (pos < ntext && src[pos] != '\n') pos++;
        int len = pos - start;
        if (len + 2 > cap) {
            cap = len + 64;
            out = (char *)realloc(out, cap);
        }
        highlight_line(src + start, out, len);
        /* keep the newline itself unstyled */
        out[len] = 0;
        style->append(out);
        nstyles += len;
        if (pos < ntext) { style->append("A"); nstyles++; pos++; }
    }
    free(out);
    free(src);
    (void)nstyles;
}

/* Highlight a single line, for incremental edits. Returns a malloc'd
 * string the caller owns. */
char *mue_highlight_one(const char *line, int len)
{
    init_styles();
    char *out = (char *)malloc(len + 1);
    if (!out) return 0;
    highlight_line(line, out, len);
    return out;
}

/* ---- public per-line API used by the custom editor -------------------- */

void mue_highlight_line(const char *src, char *cls, int len)
{
    init_styles();
    highlight_line(src, cls, len);
}

void mue_token_rgb(int cls, unsigned char *r, unsigned char *g, unsigned char *b)
{
    *r = 32; *g = 32; *b = 32;                      /* plain, near-black */
    switch (cls) {
    case MUE_TKN_MNEM: *r = 0;   *g = 0;   *b = 170; break;  /* blue   */
    case MUE_TKN_REG:  *r = 160; *g = 0;   *b = 0;   break;  /* red    */
    case MUE_TKN_NUM:  *r = 0;   *g = 128; *b = 0;   break;  /* green  */
    case MUE_TKN_CMT:  *r = 128; *g = 128; *b = 128; break;  /* grey   */
    case MUE_TKN_STR:  *r = 128; *g = 0;   *b = 128; break;  /* purple */
    default: break;
    }
}
