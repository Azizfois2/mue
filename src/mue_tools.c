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
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mue_tools.h"
#include "mue_widgets.h"

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

    /* Measured from the live dialog: window 252x253, so the FLTK client is
     * 246x224.  Three VB6 frames, a narrow hex field, two decimal columns, a
     * short ascii field and full-width oct/bin fields. */
    Fl_Window *w = new Fl_Window(246, 224, "base convertor");
    w->color(C_BG);
    w->size_range(246, 224, 246, 224);      /* not resizable, as recorded */

    {
        MueGroup *g = new MueGroup(5, 9, 237, 55, 0);
        conv.b8  = new Fl_Round_Button(20, 14, 66, 16, "8 bit");
        conv.b16 = new Fl_Round_Button(128, 14, 70, 16, "16 bit");
        conv.b8->type(FL_RADIO_BUTTON);
        conv.b16->type(FL_RADIO_BUTTON);
        conv.b8->value(1);
        conv.b8->labelsize(11);
        conv.b16->labelsize(11);
        conv.b8->callback(conv_bits);
        conv.b16->callback(conv_bits);

        Fl_Box *l = new Fl_Box(8, 34, 36, 18, "hex:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.hex = new Fl_Input(94, 36, 43, 19);
        conv.hex->box(FL_DOWN_BOX);
        conv.hex->textfont(FL_COURIER_BOLD);
        conv.hex->textsize(12);
        conv.hex->when(FL_WHEN_CHANGED);
        conv.hex->callback(conv_from, (void *)"hex");
        g->end();
    }

    {
        MueGroup *g = new MueGroup(5, 76, 237, 53, 0);
        Fl_Box *h1 = new Fl_Box(57, 82, 79, 14, "signed");
        Fl_Box *h2 = new Fl_Box(153, 82, 79, 14, "unsigned");
        h1->labelsize(11); h2->labelsize(11);
        h1->align(FL_ALIGN_CENTER);
        h2->align(FL_ALIGN_CENTER);

        Fl_Box *l = new Fl_Box(4, 99, 36, 18, "dec:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.dec_s = new Fl_Output(57, 100, 79, 19);
        conv.dec_u = new Fl_Output(153, 100, 79, 19);
        conv.dec_s->box(FL_DOWN_BOX);
        conv.dec_u->box(FL_DOWN_BOX);
        conv.dec_s->textfont(FL_COURIER_BOLD);
        conv.dec_u->textfont(FL_COURIER_BOLD);
        conv.dec_s->textsize(12);
        conv.dec_u->textsize(12);
        g->end();
    }

    {
        Fl_Box *l = new Fl_Box(30, 142, 84, 18, "ascii char:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.ch = new Fl_Output(141, 140, 24, 16);
        conv.ch->box(FL_DOWN_BOX);
        conv.ch->textfont(FL_COURIER_BOLD);
        conv.ch->textsize(12);
    }

    {
        Fl_Box *l = new Fl_Box(9, 164, 36, 18, "oct:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.oct = new Fl_Input(53, 164, 182, 21);
        conv.oct->box(FL_DOWN_BOX);
        conv.oct->textfont(FL_COURIER_BOLD);
        conv.oct->textsize(12);
        conv.oct->when(FL_WHEN_CHANGED);
        conv.oct->callback(conv_from, (void *)"oct");
    }

    {
        Fl_Box *l = new Fl_Box(9, 194, 36, 18, "bin:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        conv.bin = new Fl_Input(53, 194, 182, 21);
        conv.bin->box(FL_DOWN_BOX);
        conv.bin->textfont(FL_COURIER_BOLD);
        conv.bin->textsize(12);
        conv.bin->when(FL_WHEN_CHANGED);
        conv.bin->callback(conv_from, (void *)"bin");
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
    Fl_Scrollbar *vbar, *hbar;
} calc_t;

static calc_t calc;

/* A text box with both scrollbars always on show, like the VB6 control in the
 * original: FLTK hides them when the text fits, which changes the pane's
 * shape.  They are therefore separate widgets laid out around the editor. */
static void calc_pane_scroll(Fl_Widget *w, void *)
{
    if (w == calc.vbar)
        calc.text->scroll((int)calc.vbar->value(), 0);
    else
        calc.text->scroll(1, (int)calc.hbar->value());
}

static void calc_sync_bars(void)
{
    int len = calc.buf->length();
    int lines = calc.buf->count_lines(0, len) + 1;
    int widest = 0, i, vis;
    for (i = 0; i < lines; i++) {
        int a = calc.buf->skip_lines(0, i);
        int b = calc.buf->skip_lines(a, 1);
        int n = b - a - 1;
        if (n > widest) widest = n;
    }
    vis = calc.text->h() / (calc.text->textsize() + 4);
    if (vis < 1) vis = 1;
    calc.vbar->value(1, vis, 1, lines);
    calc.hbar->value(0, calc.text->w(), 0, widest * 8 + 1);
}

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
        calc_sync_bars();
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
    calc_sync_bars();
}

/* The dotted triangle VB6 paints in a resizable form's bottom-right corner. */
class CalcGrip : public Fl_Box {
public:
    CalcGrip(int X, int Y, int W, int H) : Fl_Box(X, Y, W, H)
    {
        box(FL_FLAT_BOX);
        color(mue_face());
    }
    void draw() override
    {
        draw_box();
        fl_color((Fl_Color)0x808080);
        /* three diagonal rows of dots, the way VB6 paints the grip */
        for (int row = 0; row < 3; row++) {
            int y0 = y() + h() - 3 - row * 4;
            for (int i = 0; i <= row; i++) {
                int x0 = x() + w() - 3 - i * 4;
                fl_point(x0, y0);
                fl_point(x0 + 1, y0 + 1);
            }
        }
    }
};

/* The calculator is resizable, like the VB6 form, but its controls do not
 * stretch: VB6 leaves them where they are and grows the text pane, which is
 * also what keeps the window from looking broken when it is maximised. */
class CalcWindow : public Fl_Window {
public:
    CalcWindow(int X, int Y, int W, int H, const char *L)
        : Fl_Window(X, Y, W, H, L), head(0), grip(0) {}
    Fl_Group *head;
    CalcGrip *grip;
    void layout()
    {
        if (head) head->size(w() - 12, head->h());
        if (calc.text) calc.text->resize(8, 98, w() - 30, h() - 121);
        if (calc.vbar) calc.vbar->resize(w() - 22, 98, 15, h() - 121);
        if (calc.hbar) calc.hbar->resize(8, h() - 23, w() - 30, 15);
        if (grip) grip->position(w() - 22, h() - 23);
    }
    void resize(int X, int Y, int W, int H) override
    {
        Fl_Window::resize(X, Y, W, H);
        layout();
    }
};

void tools_open_calculator(void)
{
    if (calc.win) { calc.win->show(); return; }

    /* Window 319x247 with a thick resizable frame leaves a 303x208 client. */
    CalcWindow *w = new CalcWindow(0, 0, 303, 208, "calculator - expression...");
    w->color(C_BG);
    w->size_range(303, 208);

    {
        MueGroup *g = new MueGroup(2, 8, 291, 42, "show result as");
        w->head = g;
        calc.as_dec = new Fl_Round_Button(18, 24, 64, 16, "decimal");
        calc.as_hex = new Fl_Round_Button(86, 24, 46, 16, "hex");
        calc.as_oct = new Fl_Round_Button(148, 24, 46, 16, "oct");
        calc.as_bin = new Fl_Round_Button(210, 24, 46, 16, "bin");
        calc.as_dec->type(FL_RADIO_BUTTON);
        calc.as_hex->type(FL_RADIO_BUTTON);
        calc.as_oct->type(FL_RADIO_BUTTON);
        calc.as_bin->type(FL_RADIO_BUTTON);
        calc.as_dec->value(1);
        calc.as_dec->labelsize(11);
        calc.as_hex->labelsize(11);
        calc.as_oct->labelsize(11);
        calc.as_bin->labelsize(11);
        g->end();
    }

    {
        MueGroup *g = new MueGroup(2, 58, 228, 36, "treat hex,oct,bin as:");
        calc.signed_ck = new Fl_Check_Button(10, 68, 76, 16, "signed");
        calc.signed_ck->labelsize(11);
        Fl_Round_Button *wd = new Fl_Round_Button(114, 68, 60, 16, "word");
        Fl_Round_Button *by = new Fl_Round_Button(176, 68, 52, 16, "byte");
        wd->type(FL_RADIO_BUTTON); by->type(FL_RADIO_BUTTON);
        wd->value(1);
        wd->labelsize(11); by->labelsize(11);
        /* greyed out until signed is ticked, as in the original */
        wd->deactivate(); by->deactivate();
        g->end();
    }
    {
        MueButton *cl = new MueButton(234, 54, 62, 20, "clear");
        MueButton *hp = new MueButton(234, 76, 62, 20, "help");
        cl->callback(calc_clear);
    }

    calc.buf = new Fl_Text_Buffer();
    calc.text = new Fl_Text_Editor(8, 98, 273, 87);
    calc.text->box(FL_DOWN_BOX);
    calc.text->color(FL_WHITE);
    calc.text->textfont(FL_COURIER_BOLD);
    calc.text->textsize(12);
    calc.text->buffer(calc.buf);
    calc.buf->text("1+2\n");

    /* The original's text box keeps both scrollbars on show (VB6
     * `ScrollBars = 3`), with a resize grip in the corner between them. */
    calc.vbar = new Fl_Scrollbar(281, 98, 15, 87);
    calc.vbar->type(FL_VERTICAL);
    calc.vbar->callback(calc_pane_scroll);
    calc.hbar = new Fl_Scrollbar(8, 185, 273, 15);
    calc.hbar->type(FL_HORIZONTAL);
    calc.hbar->callback(calc_pane_scroll);
    {
        w->grip = new CalcGrip(281, 185, 15, 15);
    }
    calc_sync_bars();

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

    /* Measured 508x333 overall, so a 502x304 client.  Two columns of VB6
     * frames: the left one holds the typing options and the keyword colours,
     * the right one the font/tabs/line-number frames, with close/reset all
     * above the full-width file-association button. */
    Fl_Window *w = new Fl_Window(502, 304, "options");
    w->color(C_BG);
    w->size_range(502, 304, 502, 304);      /* fixed size, as recorded */

    /* ---- left column ---- */
    {
        Fl_Box *l = new Fl_Box(8, 6, 96, 18, "auto-indent mode:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *ch = new Fl_Choice(108, 6, 136, 21);
        ch->add("indent on PROC|indent on {|no auto-indent");
        ch->value(0);
        ch->labelsize(11);
        ch->textsize(11);
    }
    {
        Fl_Check_Button *c1 = new Fl_Check_Button(8, 30, 236, 16,
                                                  "confine caret to text");
        Fl_Check_Button *c2 = new Fl_Check_Button(8, 50, 236, 16,
                                                  "keyword case normalization");
        Fl_Check_Button *c3 = new Fl_Check_Button(8, 70, 236, 16,
                              "show line numbers on thumb scroll");
        c1->labelsize(11); c2->labelsize(11); c3->labelsize(11);
    }
    {
        MueGroup *g = new MueGroup(8, 94, 236, 122, "keywords");
        Fl_Box *il = new Fl_Box(14, 108, 34, 18, "item:");
        il->labelsize(11);
        il->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *item = new Fl_Choice(54, 108, 180, 21);
        item->add("(none)|instruction|register|number|comment");
        item->value(0);
        item->labelsize(11);
        item->textsize(11);

        Fl_Box *fl = new Fl_Box(14, 134, 96, 18, "foreground color:");
        fl->labelsize(11);
        fl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Box *sw1 = new Fl_Box(116, 134, 36, 18);
        sw1->box(FL_DOWN_BOX);
        sw1->color(FL_WHITE);

        Fl_Box *bl = new Fl_Box(14, 158, 96, 18, "background color:");
        bl->labelsize(11);
        bl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Box *sw2 = new Fl_Box(116, 158, 36, 18);
        sw2->box(FL_DOWN_BOX);
        sw2->color(FL_WHITE);

        Fl_Box *sl = new Fl_Box(14, 182, 96, 18, "font style:");
        sl->labelsize(11);
        sl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *fs = new Fl_Choice(116, 182, 118, 21);
        fs->add("normal|bold|italic");
        fs->value(0);
        fs->labelsize(11);
        fs->textsize(11);
        g->end();
    }

    /* ---- right column ---- */
    {
        MueGroup *g = new MueGroup(252, 8, 242, 92, "fonts and colors");
        Fl_Box *l = new Fl_Box(260, 24, 56, 18, "control:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *ch = new Fl_Choice(320, 24, 166, 21);
        ch->add("source code editor|memory|disassembly|screen");
        ch->value(0);
        ch->labelsize(11);
        ch->textsize(11);

        Fl_Box *bl = new Fl_Box(260, 52, 96, 18, "background color:");
        bl->labelsize(11);
        bl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Box *sw = new Fl_Box(362, 52, 36, 18);
        sw->box(FL_DOWN_BOX);
        sw->color(FL_WHITE);

        MueButton *btn = new MueButton(386, 74, 100, 20, "set font ...");
        g->end();
    }
    {
        MueGroup *g = new MueGroup(252, 108, 242, 58, "tabs");
        Fl_Box *l = new Fl_Box(260, 122, 60, 18, "tab size:");
        l->labelsize(11);
        l->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Input *ti = new Fl_Input(326, 122, 44, 20);
        ti->value("4");
        Fl_Check_Button *c = new Fl_Check_Button(260, 144, 226, 16,
                                 "convert tabs to spaces while typing");
        c->labelsize(11);
        c->value(1);
        g->end();
    }
    {
        MueGroup *g = new MueGroup(252, 174, 242, 58, "line numbers");
        Fl_Box *l1 = new Fl_Box(260, 188, 44, 18, "style:");
        l1->labelsize(11);
        l1->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Choice *sc = new Fl_Choice(310, 188, 100, 21);
        sc->add("decimal|hexadecimal");
        sc->value(0);
        sc->labelsize(11);
        sc->textsize(11);
        Fl_Box *l2 = new Fl_Box(260, 210, 44, 18, "start at:");
        l2->labelsize(11);
        l2->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        Fl_Input *si = new Fl_Input(310, 210, 44, 20);
        si->value("1");
        g->end();
    }

    /* ---- buttons ---- */
    {
        MueButton *cl = new MueButton(262, 240, 110, 24, "close");
        MueButton *ra = new MueButton(384, 240, 110, 24, "reset all");
        MueButton *as = new MueButton(8, 272, 486, 24,
                                      "associate .asm and .inc files");
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

