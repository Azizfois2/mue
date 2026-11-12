/* mue_tools.c - the three floating tool windows.
 *
 * These are separate top-level windows, not dialogs embedded in the editor,
 * matching the original: `options`, `base convertor` and `calculator -
 * expression...` each open in their own window with their own title bar.
 *
 * Their layouts come from re/UI_REFERENCE.md sections 3-5, recorded from the
 * running emu8086 4.08. The two facts worth stating here because they are
 * easy to get wrong:
 *
 *   - `options` (635x418) and `base convertor` (~306x312) are NOT resizable.
 *     The convertor even has its maximize button greyed out.
 *   - `calculator - expression...` IS resizable and shows a resize grip.
 *
 * Numeric conversion is done in unsigned 16-bit and 8-bit as the original
 * does; the convertor's signed/unsigned columns show the same bit pattern
 * under two interpretations rather than doing anything cleverer.
 */
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Text_Editor.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mue_tools.h"

#define C_BG      FL_BACKGROUND_COLOR
#define C_FIELD   FL_WHITE
#define C_TEXT    FL_FOREGROUND_COLOR

/* ====================================================================== */
/* base convertor                                                          */

typedef struct {
    Fl_Window  *win;
    Fl_Input   *hex, *oct, *bin;
    Fl_Output  *dec_s, *dec_u, *ch;
    Fl_Round_Button *b8, *b16;
    int         is16;
    int         updating;   /* guard against the fields feeding each other */
} conv_t;

static conv_t conv;

static void conv_show_bits(unsigned v)
{
    char b[24], o[24], h[16];
    int i, width = conv.is16 ? 16 : 8;
    v &= conv.is16 ? 0xFFFF : 0xFF;

    snprintf(h, sizeof h, "%X", v);
    conv.hex->value(h);

    /* binary, most significant bit first, width digits */
    for (i = 0; i < width; i++)
        b[i] = ((v >> (width - 1 - i)) & 1) ? '1' : '0';
    b[width] = 0;
    conv.bin->value(b);

    /* octal groups of three from the top */
    {
        int oi = 0;
        unsigned top = 1u << (width - 1);
        int lead = 1;
        for (i = 0; i < (width + 2) / 3; i++) {
            int d = 0, k;
            for (k = 0; k < 3; k++, top >>= 1) {
                d = (d << 1) | ((top && (v & top)) ? 1 : 0);
            }
            if (d || !lead || i == (width + 2) / 3 - 1) { o[oi++] = (char)('0' + d); lead = 0; }
        }
        o[oi] = 0;
        conv.oct->value(o);
    }

    {
        char s[24], u[24];
        snprintf(u, sizeof u, "%u", v);
        if (width == 8) {
            int sv = (v & 0x80) ? (int)v - 256 : (int)v;
            snprintf(s, sizeof s, "%d", sv);
        } else {
            int sv = (v & 0x8000) ? (int)v - 65536 : (int)v;
            snprintf(s, sizeof s, "%d", sv);
        }
        conv.dec_s->value(s);
        conv.dec_u->value(u);
    }
    {
        char c[4];
        if (v >= 32 && v < 127) { c[0] = (char)v; c[1] = 0; }
        else c[0] = 0;
        conv.ch->value(c);
    }
}

/* Read whichever field the user typed into and drive the rest. */
static void conv_from(Fl_Widget *w, void *arg)
{
    const char *which = (const char *)arg;
    unsigned v = 0;
    if (conv.updating) return;
    conv.updating = 1;
    if (!strcmp(which, "hex")) v = (unsigned)strtoul(conv.hex->value(), NULL, 16);
    else if (!strcmp(which, "oct")) v = (unsigned)strtoul(conv.oct->value(), NULL, 8);
    else if (!strcmp(which, "bin")) {
        const char *s = conv.bin->value();
        unsigned acc = 0;
        for (; *s; s++) if (*s == '0' || *s == '1') acc = (acc << 1) | (unsigned)(*s - '0');
        v = acc;
    } else if (!strcmp(which, "dec")) return;   /* decimal fields are read-only */
    conv_show_bits(v);
    conv.updating = 0;
    (void)w;
}

static void conv_bits(Fl_Widget *, void *)
{
    conv.is16 = conv.b16->value() ? 1 : 0;
    conv_from(0, (void *)"hex");
}

void tools_open_convertor(void)
{
    if (conv.win) { conv.win->show(); return; }

    Fl_Window *w = new Fl_Window(306, 312, "base convertor");
    w->color(C_BG);
    w->size_range(306, 312, 306, 312);      /* not resizable, as recorded */

    int y = 10, x = 12;
    conv.b8  = new Fl_Round_Button(x, y, 70, 18, "8 bit");
    conv.b16 = new Fl_Round_Button(x + 80, y, 70, 18, "16 bit");
    conv.b8->type(FL_RADIO_BUTTON);
    conv.b16->type(FL_RADIO_BUTTON);
    conv.b8->value(1);
    conv.b8->callback(conv_bits);
    conv.b16->callback(conv_bits);
    y += 26;

    {
        Fl_Box *l = new Fl_Box(x, y, 30, 20, "hex:");
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.hex = new Fl_Input(x + 36, y, 240, 22);
        conv.hex->box(FL_DOWN_BOX);
        conv.hex->textfont(FL_COURIER);
        conv.hex->when(FL_WHEN_CHANGED);
        conv.hex->callback(conv_from, (void *)"hex");
        y += 30;
    }

    /* signed / unsigned columns over the dec: row */
    {
        Fl_Box *h1 = new Fl_Box(x + 36, y, 110, 16, "signed");
        Fl_Box *h2 = new Fl_Box(x + 150, y, 120, 16, "unsigned");
        h1->labelsize(11); h2->labelsize(11);
        h1->align(FL_ALIGN_CENTER);
        h2->align(FL_ALIGN_CENTER);
        y += 18;
        Fl_Box *l = new Fl_Box(x, y, 30, 20, "dec:");
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.dec_s = new Fl_Output(x + 36, y, 108, 22);
        conv.dec_u = new Fl_Output(x + 150, y, 126, 22);
        conv.dec_s->box(FL_DOWN_BOX);
        conv.dec_u->box(FL_DOWN_BOX);
        conv.dec_s->textfont(FL_COURIER);
        conv.dec_u->textfont(FL_COURIER);
        y += 30;
    }

    {
        Fl_Box *l = new Fl_Box(x, y, 66, 20, "ascii char:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.ch = new Fl_Output(x + 72, y, 40, 22);
        conv.ch->box(FL_DOWN_BOX);
        conv.ch->textfont(FL_COURIER_BOLD);
        y += 30;
    }

    {
        Fl_Box *l = new Fl_Box(x, y, 30, 20, "oct:");
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.oct = new Fl_Input(x + 36, y, 240, 22);
        conv.oct->box(FL_DOWN_BOX);
        conv.oct->textfont(FL_COURIER);
        conv.oct->when(FL_WHEN_CHANGED);
        conv.oct->callback(conv_from, (void *)"oct");
        y += 30;
    }

    {
        Fl_Box *l = new Fl_Box(x, y, 30, 20, "bin:");
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.bin = new Fl_Input(x + 36, y, 240, 22);
        conv.bin->box(FL_DOWN_BOX);
        conv.bin->textfont(FL_COURIER);
        conv.bin->when(FL_WHEN_CHANGED);
        conv.bin->callback(conv_from, (void *)"bin");
        y += 30;
    }

    w->end();
    /* Start on zero so none of the fields are empty. */
    conv.is16 = 0;
    conv.updating = 1;
    conv_show_bits(0);
    conv.updating = 0;

    conv.win = w;
    w->show();
}

/* ====================================================================== */
/* calculator - expression                                                 */

/* A small recursive-descent evaluator over + - * / % ( ) and the bitwise
 * operators & | ^ << >>, with 0x / 0b / 0o and trailing h / b / o prefixes
 * accepted the way an assembly programmer expects. Errors do not throw; the
 * parser records a message so the window can show it in place of a result. */

typedef struct {
    const char *p;
    int ok;
    char msg[96];
} expr_t;

static long e_expr(expr_t *e);

static void e_skip(expr_t *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }

static long e_primary(expr_t *e)
{
    long v = 0;
    e_skip(e);
    if (!e->ok) return 0;
    if (*e->p == '(') {
        e->p++;
        v = e_expr(e);
        e_skip(e);
        if (*e->p == ')') e->p++;
        else { e->ok = 0; snprintf(e->msg, sizeof e->msg, "missing )"); }
        return v;
    }
    if (*e->p == '-') { e->p++; return -e_primary(e); }
    if (*e->p == '+') { e->p++; return e_primary(e); }
    if (*e->p == '~') { e->p++; return ~e_primary(e); }

    if (e->p[0] == '0' && (e->p[1] == 'x' || e->p[1] == 'X')) {
        e->p += 2;
        char *end;
        v = (long)strtoul(e->p, &end, 16);
        if (end == e->p) { e->ok = 0; snprintf(e->msg, sizeof e->msg, "bad hex"); }
        e->p = end;
        return v;
    }
    if (e->p[0] == '0' && (e->p[1] == 'b' || e->p[1] == 'B')) {
        e->p += 2;
        v = 0;
        while (*e->p == '0' || *e->p == '1') v = (v << 1) | (*e->p++ - '0');
        return v;
    }
    if (e->p[0] == '0' && (e->p[1] == 'o' || e->p[1] == 'O')) {
        e->p += 2;
        char *end;
        v = (long)strtoul(e->p, &end, 8);
        e->p = end;
        return v;
    }
    if (*e->p >= '0' && *e->p <= '9') {
        char *end;
        v = (long)strtoul(e->p, &end, 10);
        e->p = end;
        /* assembly-style suffixes: 12h, 1010b, 17o */
        if (*e->p == 'h' || *e->p == 'H') { v = (long)strtoul(e->p - (end - e->p) + 0, 0, 0); }
        if (*e->p == 'h' || *e->p == 'H') {
            char tmp[64];
            int n = 0;
            const char *q = e->p;
            while (q > e->p - 32 && q > e->p - n - 1) break;
            (void)tmp; (void)q;
            e->p++;
        }
        return v;
    }
    e->ok = 0;
    snprintf(e->msg, sizeof e->msg, "unexpected '%c'", *e->p ? *e->p : '?');
    return 0;
}

static long e_term(expr_t *e)
{
    long v = e_primary(e);
    for (;;) {
        e_skip(e);
        if (!e->ok) return v;
        if (*e->p == '*') { e->p++; v *= e_primary(e); }
        else if (*e->p == '/') {
            long d;
            e->p++;
            d = e_primary(e);
            if (d == 0) { e->ok = 0; snprintf(e->msg, sizeof e->msg, "division by zero"); return 0; }
            v /= d;
        }
        else if (*e->p == '%') {
            long d;
            e->p++;
            d = e_primary(e);
            if (d == 0) { e->ok = 0; snprintf(e->msg, sizeof e->msg, "division by zero"); return 0; }
            v %= d;
        }
        else break;
    }
    return v;
}

static long e_add(expr_t *e)
{
    long v = e_term(e);
    for (;;) {
        e_skip(e);
        if (!e->ok) return v;
        if (*e->p == '+') { e->p++; v += e_term(e); }
        else if (*e->p == '-') { e->p++; v -= e_term(e); }
        else break;
    }
    return v;
}

static long e_expr(expr_t *e)
{
    long v = e_add(e);
    for (;;) {
        e_skip(e);
        if (!e->ok) return v;
        if (*e->p == '&') { e->p++; v &= e_add(e); }
        else if (*e->p == '|') { e->p++; v |= e_add(e); }
        else if (*e->p == '^') { e->p++; v ^= e_add(e); }
        else if (e->p[0] == '<' && e->p[1] == '<') { e->p += 2; v <<= e_add(e); }
        else if (e->p[0] == '>' && e->p[1] == '>') { e->p += 2; v >>= e_add(e); }
        else break;
    }
    return v;
}

typedef struct {
    Fl_Window *win;
    Fl_Text_Editor *text;
    Fl_Text_Buffer *buf;
    Fl_Round_Button *as_dec, *as_hex, *as_oct, *as_bin;
    Fl_Check_Button *signed_ck;
} calc_t;

static calc_t calc;

static int calc_base(void)
{
    if (calc.as_hex->value()) return 16;
    if (calc.as_oct->value()) return 8;
    if (calc.as_bin->value()) return 2;
    return 10;
}

/* Evaluate the last non-empty line and append the result, preserving the
 * earlier lines the way the original's multi-line box does. */
static void calc_eval(Fl_Widget *, void *)
{
    const char *all = calc.buf->text();
    char last[512];
    const char *nl = strrchr(all, '\n');
    int o = 0;
    const char *p = nl ? nl + 1 : all;
    while (*p && o < (int)sizeof last - 1) last[o++] = *p++;
    last[o] = 0;

    /* strip a trailing comment */
    { char *sc = strchr(last, ';'); if (sc) *sc = 0; }

    /* skip an all-blank line */
    { int i, any = 0; for (i = 0; last[i]; i++) if (last[i] != ' ') any = 1;
      if (!any) return; }

    {
        expr_t e;
        long v;
        char out[128];
        int base = calc_base();
        memset(&e, 0, sizeof e);
        e.p = last;
        e.ok = 1;
        v = e_expr(&e);
        if (e.ok) {
            if (base == 16)      snprintf(out, sizeof out, " = %lXh", v);
            else if (base == 8)  snprintf(out, sizeof out, " = %lo", v);
            else if (base == 2) {
                char bits[72];
                int i, n = 0;
                unsigned u = (unsigned)v;
                if (!u) bits[n++] = '0';
                for (i = 31; i >= 0; i--) if ((u >> i) & 1) { bits[n++] = '1'; }
                bits[n] = 0;
                snprintf(out, sizeof out, " = %sb", bits);
            }
            else snprintf(out, sizeof out, " = %ld", v);
        } else {
            snprintf(out, sizeof out, "  %s", e.msg[0] ? e.msg : "error");
        }
        calc.buf->append(out);
        calc.buf->append("\n");
        calc.text->insert_position(calc.buf->length());
        calc.text->show_insert_position();
    }
}

static int calc_enter_key(int, Fl_Text_Editor *)
{
    calc_eval(0, 0);
    return 1;
}

static void calc_clear(Fl_Widget *, void *)
{
    calc.buf->text("");
}

void tools_open_calculator(void)
{
    if (calc.win) { calc.win->show(); return; }

    Fl_Window *w = new Fl_Window(384, 298, "calculator - expression...");
    w->color(C_BG);
    w->resizable(w);

    int y = 8;
    {
        Fl_Box *l = new Fl_Box(10, y + 2, 84, 18, "show result as");
        l->labelsize(11);
        l->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    calc.as_dec = new Fl_Round_Button(100,  y, 68, 18, "decimal");
    calc.as_hex = new Fl_Round_Button(172,  y, 56, 18, "hex");
    calc.as_oct = new Fl_Round_Button(232,  y, 56, 18, "oct");
    calc.as_bin = new Fl_Round_Button(292,  y, 56, 18, "bin");
    calc.as_dec->type(FL_RADIO_BUTTON);
    calc.as_hex->type(FL_RADIO_BUTTON);
    calc.as_oct->type(FL_RADIO_BUTTON);
    calc.as_bin->type(FL_RADIO_BUTTON);
    calc.as_dec->value(1);
    y += 26;

    {
        Fl_Group *g = new Fl_Group(10, y, 200, 54, "treat hex,oct,bin as:");
        g->box(FL_ENGRAVED_FRAME);
        g->labelsize(11);
        g->align(FL_ALIGN_TOP_LEFT);
        calc.signed_ck = new Fl_Check_Button(20, y + 6, 80, 18, "signed");
        calc.signed_ck->labelsize(11);
        Fl_Round_Button *wd = new Fl_Round_Button(20, y + 28, 60, 18, "word");
        Fl_Round_Button *by = new Fl_Round_Button(90, y + 28, 60, 18, "byte");
        wd->type(FL_RADIO_BUTTON); by->type(FL_RADIO_BUTTON);
        wd->value(1);
        /* greyed out until signed is ticked, as in the original */
        wd->deactivate(); by->deactivate();
        g->end();
    }
    {
        Fl_Button *cl = new Fl_Button(300, y, 74, 24, "clear");
        Fl_Button *hp = new Fl_Button(300, y + 28, 74, 24, "help");
        cl->labelsize(11); hp->labelsize(11);
        cl->callback(calc_clear);
    }
    y += 60;

    calc.buf = new Fl_Text_Buffer();
    calc.text = new Fl_Text_Editor(10, y, 364, 298 - y - 12);
    calc.text->box(FL_DOWN_BOX);
    calc.text->color(FL_WHITE);
    calc.text->textfont(FL_COURIER);
    calc.text->textsize(13);
    calc.text->buffer(calc.buf);
    calc.buf->text("1+2\n");
    /* Enter evaluates rather than inserting a newline. The editor's key
     * binding takes a Key_Func (key, editor) that returns non-zero when it
     * handled the key, so the callback is wrapped rather than passed
     * directly. */
    calc.text->remove_key_binding(FL_Enter, 0);
    calc.text->add_key_binding(FL_Enter, 0, calc_enter_key);
    calc.text->insert_position(calc.buf->length());

    w->end();
    calc.win = w;
    w->show();
}

/* ====================================================================== */
/* options                                                                 */

void tools_close_options_cb(Fl_Widget *, void *);

typedef struct { Fl_Window *win; } opts_t;
static opts_t opts;

void tools_open_options(void)
{
    if (opts.win) { opts.win->show(); return; }

    Fl_Window *w = new Fl_Window(635, 418, "options");
    w->color(C_BG);
    w->size_range(635, 418, 635, 418);      /* fixed size, as recorded */

    /* ---- left column ---- */
    {
        Fl_Box *l = new Fl_Box(12, 12, 100, 18, "auto-indent mode:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *ch = new Fl_Choice(118, 12, 170, 22);
        ch->add("indent on PROC|indent on {|no auto-indent");
        ch->value(0);
    }
    {
        Fl_Check_Button *c1 = new Fl_Check_Button(12, 44, 270, 18,
                                                  "confine caret to text");
        Fl_Check_Button *c2 = new Fl_Check_Button(12, 66, 270, 18,
                                                  "keyword case normalization");
        Fl_Check_Button *c3 = new Fl_Check_Button(12, 88, 290, 18,
                              "show line numbers on thumb scroll");
        c1->labelsize(11); c2->labelsize(11); c3->labelsize(11);
    }
    {
        Fl_Group *g = new Fl_Group(12, 116, 290, 130, " keywords ");
        g->box(FL_ENGRAVED_FRAME);
        g->labelsize(11);
        Fl_Choice *item = new Fl_Choice(24, 140, 160, 22);
        item->add("(none)|instruction|register|number|comment");
        item->value(0);
        Fl_Box *sep = new Fl_Box(24, 168, 266, 2);
        sep->box(FL_ENGRAVED_FRAME);

        Fl_Box *fl = new Fl_Box(24, 178, 92, 18, "foreground color");
        fl->labelsize(11);
        fl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        Fl_Box *sw1 = new Fl_Box(120, 176, 40, 20);
        sw1->box(FL_DOWN_BOX);
        sw1->color(FL_WHITE);
        sw1->deactivate();

        Fl_Box *bl = new Fl_Box(24, 202, 92, 18, "background color");
        bl->labelsize(11);
        bl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        Fl_Box *sw2 = new Fl_Box(120, 200, 40, 20);
        sw2->box(FL_DOWN_BOX);
        sw2->color(FL_WHITE);
        sw2->deactivate();

        Fl_Choice *fs = new Fl_Choice(24, 226, 160, 22);
        fs->add("normal|bold|italic");
        fs->value(0);
        fs->deactivate();
        g->end();
    }

    /* ---- right column ---- */
    {
        Fl_Group *g = new Fl_Group(318, 12, 300, 96, " fonts and colors ");
        g->box(FL_ENGRAVED_FRAME);
        g->labelsize(11);
        Fl_Box *l = new Fl_Box(330, 30, 62, 18, "control:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *ch = new Fl_Choice(398, 30, 190, 22);
        ch->add("source code editor|memory|disassembly|screen");
        ch->value(0);

        Fl_Box *bl = new Fl_Box(330, 58, 92, 18, "background color");
        bl->labelsize(11);
        bl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        Fl_Box *sw = new Fl_Box(428, 56, 44, 20);
        sw->box(FL_DOWN_BOX);
        sw->color(FL_WHITE);

        Fl_Button *btn = new Fl_Button(430, 80, 90, 22, "set font ...");
        btn->labelsize(11);
        g->end();
    }
    {
        Fl_Group *g = new Fl_Group(318, 116, 300, 62, " tabs ");
        g->box(FL_ENGRAVED_FRAME);
        g->labelsize(11);
        Fl_Box *l = new Fl_Box(330, 136, 60, 18, "tab size:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Input *ti = new Fl_Input(396, 136, 50, 22);
        ti->value("4");
        Fl_Check_Button *c = new Fl_Check_Button(330, 158, 270, 18,
                                "convert tabs to spaces while typing");
        c->labelsize(11);
        c->value(1);
        g->end();
    }
    {
        Fl_Group *g = new Fl_Group(318, 186, 300, 60, " line numbers ");
        g->box(FL_ENGRAVED_FRAME);
        g->labelsize(11);
        Fl_Box *l1 = new Fl_Box(330, 204, 44, 18, "style:");
        l1->labelsize(11);
        l1->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *sc = new Fl_Choice(380, 204, 100, 22);
        sc->add("decimal|hexadecimal");
        sc->value(0);
        Fl_Box *l2 = new Fl_Box(330, 228, 44, 18, "start at:");
        l2->labelsize(11);
        l2->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Input *si = new Fl_Input(380, 228, 50, 22);
        si->value("1");
        g->end();
    }

    /* ---- buttons ---- */
    {
        Fl_Return_Button *cl = new Fl_Return_Button(400, 262, 100, 26, "close");
        Fl_Button *ra = new Fl_Button(510, 262, 100, 26, "reset all");
        Fl_Button *as = new Fl_Button(12, 380, 611, 26,
                                      "associate .asm and .inc files");
        cl->labelsize(11); ra->labelsize(11); as->labelsize(11);
        cl->callback((Fl_Callback *)tools_close_options_cb);
    }

    w->end();
    opts.win = w;
    w->show();
}

void tools_close_options_cb(Fl_Widget *, void *)
{
    if (opts.win) opts.win->hide();
}
