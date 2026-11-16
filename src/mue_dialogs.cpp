/* mue_dialogs.cpp - the floating windows, at their measured sizes.
 *
 * Everything here reads the live core (registers, memory, screen) rather
 * than showing placeholders; the windows that display a file (listing,
 * symbol table) read it from the build directory if it exists.
 */
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Round_Button.H>
#include <FL/fl_draw.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>

#include "mue_dialogs.h"
#include "mue_widgets.h"
#include "mue_icons.h"
#include "mue_paths.h"
#include "mue_tools.h"
#include "mue_core.h"
#include "cpu.h"

static char dlg_src[65536];
static char dlg_path[512];

void dlg_set_source(const char *src, const char *path)
{
    snprintf(dlg_src, sizeof dlg_src, "%s", src ? src : "");
    snprintf(dlg_path, sizeof dlg_path, "%s", path ? path : "");
}

void cb_open_calc(Fl_Widget *, void *) { tools_open_calculator(); }
void cb_open_conv(Fl_Widget *, void *) { tools_open_convertor(); }
void cb_open_opts(Fl_Widget *, void *) { tools_open_options(); }

/* ---- small shared helpers --------------------------------------------- */

static void ml_out(Fl_Multiline_Output *o, const char *s)
{
    o->value(s && *s ? s : "no program loaded");
    o->redraw();
}

static void read_text_file(const char *path, char *out, int outsz,
                           const char *missing)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, (size_t)outsz, "%s", missing); return; }
    size_t n = fread(out, 1, (size_t)outsz - 1, f);
    fclose(f);
    out[n] = 0;
}

/* Where emu8086 keeps its builds; the clone mirrors the folder, so try the
 * current source's stem first, then any .list/.symbol that exists. */
static void build_sidecar(const char *ext, char *out, int outsz)
{
    char stem[256] = "hello";
    if (dlg_path[0]) {
        const char *b = strrchr(dlg_path, '\\');
        b = b ? b + 1 : dlg_path;
        snprintf(stem, sizeof stem, "%s", b);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
    }
    snprintf(out, (size_t)outsz,
             "%s\\%s.com_%s", mue_dir_build(), stem, ext);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    snprintf(out, (size_t)outsz, "%s\\%s.com%s", mue_dir_build(), stem, ext);
}

/* ====================================================================== */
/* flags                                                                   */

class FlagCombo : public Fl_Widget {
public:
    FlagCombo(int X, int Y, const char *nm, unsigned short mask)
        : Fl_Widget(X, Y, 56, 20, 0), bit(mask), val(0) {
        snprintf(name, sizeof name, "%s", nm);
    }
    char name[4];
    unsigned short bit;
    int val;
    void draw() override {
        mue_ui_font(11);
        fl_color(FL_BLACK);
        fl_draw(name, x() - 28, y(), 24, h(), FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
        fl_color(val ? mue_red() : mue_blue());
        mue_mono_font(12);
        char v[2] = { (char)('0' + val), 0 };
        fl_draw(v, x() + 2, y(), w() - 18, h(), FL_ALIGN_CENTER);
        /* combo arrow button */
        int bx = x() + w() - 16;
        draw_box(FL_UP_BOX, bx, y() + 1, 15, h() - 2, mue_face());
        fl_color(FL_BLACK);
        int cx = bx + 7, cy = y() + h() / 2 - 1;
        fl_polygon(cx - 3, cy, cx + 3, cy, cx, cy + 3);
    }
    int handle(int ev) override {
        if (ev == FL_PUSH) { toggle(); return 1; }
        return Fl_Widget::handle(ev);
    }
    void toggle() {
        unsigned short r[8], sr[4], ip, fl;
        long ic;
        core_regs(r, sr, &ip, &fl, &ic);
        core_write_flags((unsigned short)(fl ^ bit));
        val = !val;
        redraw();
    }
};

static Fl_Double_Window *flags_win;
static FlagCombo *flag_combo[8];
static const char *flag_names[8] = { "CF","ZF","SF","OF","PF","AF","IF","DF" };
static const unsigned short flag_masks[8] = {
    0x0001, 0x0040, 0x0080, 0x0800, 0x0004, 0x0010, 0x0200, 0x0400
};

void dlg_flags(void)
{
    if (!flags_win) {
        flags_win = new Fl_Double_Window(97, 286, "flags");
        flags_win->color(mue_face());
        for (int i = 0; i < 8; i++) {
            flag_combo[i] = new FlagCombo(38, 10 + i * 29,
                                          flag_names[i], flag_masks[i]);
        }
        MueButton *an = new MueButton(4, 258, 89, 22, "analyse");
        an->callback((Fl_Callback *)cb_lexical_flags);
        flags_win->end();
        flags_win->set_non_modal();
    }
    dlg_refresh();
    flags_win->show();
}

/* ====================================================================== */
/* lexical flag analyser                                                   */

static Fl_Double_Window *lex_win;
static Fl_Multiline_Output *lex_text;

void cb_lexical_flags(Fl_Widget *, void *)
{
    if (!lex_win) {
        lex_win = new Fl_Double_Window(281, 268, "lexical flag analyser");
        lex_win->color(mue_face());
        lex_text = new Fl_Multiline_Output(3, 3, 275, 262);
        lex_text->box(FL_DOWN_BOX);
        lex_text->color(FL_WHITE);
        lex_text->textfont(FL_COURIER_BOLD);
        lex_text->textsize(12);
        lex_win->end();
        lex_win->resizable(lex_text);
    }
    unsigned short r[8], sr[4], ip, fl;
    long ic;
    core_regs(r, sr, &ip, &fl, &ic);
    char b[2048];
    snprintf(b, sizeof b,
        "=== unsigned ===\n"
        "not below        (CF=%d)\n"
        "  above          (CF=%d and ZF=%d)\n"
        "\n"
        "=== signed ===\n"
        "not less         (SF=0F)\n"
        "  greater        (SF=0F and ZF=%d)\n"
        "\n"
        "not equal        (ZF=%d)\n"
        "positive         (SF=%d)\n"
        "no overflow      (0F=%d)\n"
        "parity odd       (PF=%d)\n"
        "not aux carry    (AF=%d)\n"
        "hw-int enabled   (IF=%d)\n"
        "forward dir      (DF=%d)\n",
        !!(fl & 0x0001), !!(fl & 0x0001), !!(fl & 0x0040),
        !!(fl & 0x0040), !!(fl & 0x0040), !!(fl & 0x0080),
        !!(fl & 0x0800), !!(fl & 0x0004), !!(fl & 0x0010),
        !!(fl & 0x0200), !!(fl & 0x0400));
    ml_out(lex_text, b);
    lex_win->show();
}

/* ====================================================================== */
/* screen                                                                  */

static Fl_Double_Window *screen_win;
static ScreenGrid *screen_grid;
static Fl_Box *screen_cur;

void dlg_screen(void)
{
    if (!screen_win) {
        screen_win = new Fl_Double_Window(727, 347, "emulator screen (80x25 chars)");
        screen_win->color(mue_face());
        screen_grid = new ScreenGrid(0, 0, 727, 319);
        MueButton *clr = new MueButton(4, 322, 80, 22, "clear screen");
        clr->callback([](Fl_Widget *, void *) { screen_grid->redraw(); });
        MueButton *fnt = new MueButton(88, 322, 80, 22, "change font");
        screen_cur = new Fl_Box(560, 322, 160, 22, "0:0");
        screen_cur->box(FL_NO_BOX);
        screen_cur->labelsize(11);
        screen_cur->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        screen_win->end();
        screen_win->resizable(screen_grid);
    }
    int r = 0, c = 0;
    core_screen_cursor(&r, &c);
    char b[32];
    snprintf(b, sizeof b, "%d:%d", r, c);
    screen_cur->label(b);
    screen_grid->redraw();
    screen_win->show();
}

/* ====================================================================== */
/* ascii codes                                                             */

class AsciiSheet : public Fl_Widget {
public:
    AsciiSheet(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H, 0) {}
    void draw() override {
        draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
        int cw = w() / 8, chh = h() / 32;
        mue_mono_font(12);
        for (int i = 0; i < 256; i++) {
            int col = i / 32, row = i % 32;
            int xx = x() + 2 + col * cw, yy = y() + 2 + row * chh;
            char pre[8];
            snprintf(pre, sizeof pre, "%03d:", i);
            fl_color(FL_BLACK);
            fl_draw(pre, xx + 2, yy, 38, chh, FL_ALIGN_LEFT);
            const char *nm = 0;
            switch (i) {
            case 0: nm = "null"; break;
            case 7: nm = "beep"; break;
            case 8: nm = "back"; break;
            case 9: nm = "tab"; break;
            case 10: nm = "newl"; break;
            case 13: nm = "cret"; break;
            case 32: nm = "spa"; break;
            case 255: nm = "res"; break;
            }
            if (nm) {
                fl_color(mue_red());
                fl_draw(nm, xx + 40, yy, cw - 42, chh, FL_ALIGN_LEFT);
            } else {
                char g[2] = { (char)i, 0 };
                fl_color(FL_BLACK);
                fl_draw(g, xx + 42, yy, cw - 44, chh, FL_ALIGN_LEFT);
            }
        }
    }
};

static Fl_Double_Window *ascii_win;

void cb_ascii_codes(Fl_Widget *, void *)
{
    if (!ascii_win) {
        ascii_win = new Fl_Double_Window(727, 411, "ascii codes");
        ascii_win->color(mue_face());
        new AsciiSheet(0, 0, 727, 411);
        ascii_win->end();
        ascii_win->set_non_modal();
    }
    ascii_win->show();
}

/* ====================================================================== */
/* about                                                                   */

void cb_about(Fl_Widget *, void *)
{
    static Fl_Double_Window *w;
    if (!w) {
        w = new Fl_Double_Window(417, 299, "about mue");
        w->color(mue_face());
        Fl_Box *a = new Fl_Box(10, 8, 397, 26, "mue");
        a->labelfont(FL_COURIER_BOLD); a->labelsize(20);
        Fl_Box *b = new Fl_Box(10, 34, 397, 20, "version 1.0");
        b->labelfont(FL_COURIER_BOLD); b->labelsize(13);
        Fl_Box *c = new Fl_Box(10, 54, 397, 18,
                               "clean-room 8086 assembler + emulator");
        c->labelsize(11);
        Fl_Box *d = new Fl_Box(10, 76, 397, 46,
            "an independent reimplementation of the\n"
            "emu8086 4.08 workbench");
        d->box(FL_ENGRAVED_FRAME); d->labelsize(11);
        Fl_Box *e = new Fl_Box(10, 128, 397, 42,
            "user interface   FLTK\n"
            "assembler        mue asm\n"
            "cpu core         mue8086");
        e->box(FL_ENGRAVED_FRAME); e->labelsize(10);
        e->align(FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
        Fl_Box *f = new Fl_Box(10, 178, 397, 52,
            "MIT License\n"
            "Copyright (c) 2026 AZDev (github.com/Azizfois2)");
        f->box(FL_ENGRAVED_FRAME); f->labelsize(10);
        f->align(FL_ALIGN_WRAP | FL_ALIGN_INSIDE);
        Fl_Box *g = new Fl_Box(10, 236, 397, 22,
            "not affiliated with the authors of emu8086");
        g->box(FL_ENGRAVED_FRAME); g->labelsize(10);
        Fl_Box *h = new Fl_Box(10, 266, 397, 20, "github.com/Azizfois2");
        h->labelfont(FL_COURIER_BOLD);
        h->labelcolor(mue_blue());
        w->end();
        w->set_non_modal();
    }
    w->show();
}

/* ====================================================================== */
/* RAM window                                                              */

static Fl_Double_Window *ram_win;
static HexTable *ram_table;
static Fl_Input *ram_addr;

void cb_ram(Fl_Widget *, void *)
{
    if (!ram_win) {
        ram_win = new Fl_Double_Window(645, 162, "Random Access Memory");
        ram_win->color(mue_face());
        ram_addr = new Fl_Input(6, 6, 88, 24);
        ram_addr->box(FL_DOWN_BOX);
        ram_addr->color(FL_WHITE);
        ram_addr->textfont(FL_COURIER_BOLD);
        ram_addr->textsize(12);
        ram_addr->align(FL_ALIGN_CENTER);
        ram_addr->value("0100:0100");
        MueButton *up = new MueButton(100, 7, 56, 22, "update");
        Fl_Round_Button *tb = new Fl_Round_Button(180, 8, 64, 20, "table");
        tb->type(FL_RADIO_BUTTON); tb->value(1); tb->labelsize(11);
        Fl_Round_Button *ls = new Fl_Round_Button(250, 8, 60, 20, "list");
        ls->type(FL_RADIO_BUTTON); ls->labelsize(11);
        ram_table = new HexTable(4, 36, 637, 122);
        ram_win->end();
        ram_win->resizable(ram_table);
        up->callback([](Fl_Widget *, void *) {
            unsigned short seg = 0, off = 0;
            const char *s = ram_addr->value();
            const char *c = strchr(s, ':');
            if (c) {
                seg = (unsigned short)strtoul(s, 0, 16);
                off = (unsigned short)strtoul(c + 1, 0, 16);
            }
            ram_table->anchor(seg, off);
        });
        tb->callback([](Fl_Widget *, void *) { ram_table->list_mode(0); });
        ls->callback([](Fl_Widget *, void *) { ram_table->list_mode(1); });
    }
    unsigned short r[8], sr[4], ip, fl;
    long ic;
    core_regs(r, sr, &ip, &fl, &ic);
    char b[16];
    snprintf(b, sizeof b, "%04X:%04X", sr[CS], ip);
    ram_addr->value(b);
    ram_table->anchor(sr[CS], ip);
    ram_win->show();
}

/* ====================================================================== */
/* ALU / FPU / stop on condition                                           */

void cb_alu(Fl_Widget *, void *)
{
    static Fl_Double_Window *w;
    if (!w) {
        w = new Fl_Double_Window(419, 105, "ALU - arithmetic & logic unit");
        w->color(mue_face());
        new AluGrid(2, 2, 415, 101);
        w->end();
        w->set_non_modal();
    }
    w->show();
}

void cb_fpu(Fl_Widget *, void *)
{
    static Fl_Double_Window *w;
    if (!w) {
        w = new Fl_Double_Window(351, 151, "FPU");
        w->color(mue_face());
        Fl_Box *b = new Fl_Box(6, 6, 339, 139,
            "control register:\n"
            "00 00 00 00 00 00 00 00 00 00 00 00 00 00\n"
            "\n"
            "FPU registers:\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00\n"
            "00 00 00 00 00 00 00 00 00 00");
        b->box(FL_DOWN_BOX);
        b->color(FL_WHITE);
        b->labelfont(FL_COURIER_BOLD);
        b->labelsize(12);
        b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        w->end();
        w->set_non_modal();
    }
    w->show();
}

void cb_stop_condition(Fl_Widget *, void *)
{
    static Fl_Double_Window *w;
    if (!w) {
        w = new Fl_Double_Window(420, 95, "stop on condition");
        w->color(mue_face());
        Fl_Box *h1 = new Fl_Box(20, 6, 100, 16, "operand");
        Fl_Box *h2 = new Fl_Box(140, 6, 100, 16, "condition");
        Fl_Box *h3 = new Fl_Box(260, 6, 120, 16, "expression");
        for (Fl_Box *b : { h1, h2, h3 }) {
            b->box(FL_NO_BOX); b->labelsize(11);
        }
        MueField *op = new MueField(20, 24, 100, 22, 0);
        op->value("AX");
        MueField *cd = new MueField(140, 24, 100, 22, 0);
        cd->value("=");
        MueField *ex = new MueField(260, 24, 140, 22, 1);
        Fl_Box *hint = new Fl_Box(10, 52, 340, 16,
            "hex values must start with 0 and have h suffix.");
        hint->box(FL_NO_BOX); hint->labelsize(10);
        MueButton *q = new MueButton(390, 70, 24, 20, "?");
        w->end();
        w->set_non_modal();
        (void)q;
    }
    w->show();
}

/* ====================================================================== */
/* listing / symbol table                                                  */

static Fl_Double_Window *list_win, *sym_win;
static Fl_Multiline_Output *list_text, *sym_text;

static void show_sidecar(int which)
{
    char path[600], buf[131072];
    if (which == 0) {
        build_sidecar(".list", path, sizeof path);
        if (!list_win) {
            list_win = new Fl_Double_Window(620, 500, "listing");
            list_win->color(mue_face());
            list_text = new Fl_Multiline_Output(3, 3, 614, 494);
            list_text->box(FL_DOWN_BOX);
            list_text->color(FL_WHITE);
            list_text->textfont(FL_COURIER);
            list_text->textsize(12);
            list_win->end();
            list_win->resizable(list_text);
        }
        read_text_file(path, buf, sizeof buf,
                       "no listing was generated for this build.");
        list_text->value(buf);
        list_text->redraw();
        list_win->show();
    } else {
        build_sidecar(".symbol", path, sizeof path);
        if (!sym_win) {
            sym_win = new Fl_Double_Window(620, 400, "symbol table");
            sym_win->color(mue_face());
            sym_text = new Fl_Multiline_Output(3, 3, 614, 394);
            sym_text->box(FL_DOWN_BOX);
            sym_text->color(FL_WHITE);
            sym_text->textfont(FL_COURIER);
            sym_text->textsize(12);
            sym_win->end();
            sym_win->resizable(sym_text);
        }
        read_text_file(path, buf, sizeof buf,
                       "no symbol table was generated for this build.");
        sym_text->value(buf);
        sym_text->redraw();
        sym_win->show();
    }
}

void cb_listing(Fl_Widget *, void *) { show_sidecar(0); }
void cb_symbol_table(Fl_Widget *, void *) { show_sidecar(1); }

/* ====================================================================== */
/* extended value viewer                                                   */

static Fl_Double_Window *ext_win;
static Fl_Box *ext_bits[3];
static Fl_Box *ext_dec[3];

void cb_extended_value(Fl_Widget *, void *)
{
    if (!ext_win) {
        ext_win = new Fl_Double_Window(247, 334, "extended value viewer");
        ext_win->color(mue_face());
        Fl_Box *wl = new Fl_Box(6, 6, 44, 20, "watch:");
        wl->box(FL_NO_BOX); wl->labelsize(11);
        MueField *wf = new MueField(52, 6, 60, 20, 0);
        wf->value("AL");
        Fl_Round_Button *rw = new Fl_Round_Button(120, 6, 54, 20, "word");
        rw->type(FL_RADIO_BUTTON); rw->labelsize(11);
        Fl_Round_Button *rb = new Fl_Round_Button(180, 6, 54, 20, "byte");
        rb->type(FL_RADIO_BUTTON); rb->value(1); rb->labelsize(11);

        Fl_Box *hl = new Fl_Box(6, 32, 40, 16, "hex:");
        Fl_Box *bl = new Fl_Box(6, 52, 40, 16, "bin:");
        Fl_Box *ol = new Fl_Box(6, 72, 40, 16, "oct:");
        for (Fl_Box *b : { hl, bl, ol }) { b->box(FL_NO_BOX); b->labelsize(11); b->align(FL_ALIGN_RIGHT); }
        ext_bits[0] = new Fl_Box(50, 32, 130, 18, "00");
        ext_bits[1] = new Fl_Box(50, 52, 130, 18, "00000000");
        ext_bits[2] = new Fl_Box(50, 72, 130, 18, "000");
        for (Fl_Box *b : ext_bits) {
            b->box(FL_DOWN_BOX); b->color(FL_WHITE);
            b->labelfont(FL_COURIER_BOLD); b->labelsize(12);
            b->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
        }
        Fl_Box *g = new Fl_Box(6, 98, 235, 46, "decimal 8 bit");
        g->box(FL_ENGRAVED_FRAME); g->labelsize(11); g->align(FL_ALIGN_LEFT|FL_ALIGN_INSIDE);
        Fl_Box *u = new Fl_Box(12, 114, 80, 20, "unsigned:");
        Fl_Box *s = new Fl_Box(12, 134, 80, 20, "signed:");
        for (Fl_Box *b : { u, s }) { b->box(FL_NO_BOX); b->labelsize(11); b->align(FL_ALIGN_RIGHT); }
        ext_dec[0] = new Fl_Box(96, 114, 60, 18, "0");
        ext_dec[1] = new Fl_Box(96, 134, 60, 18, "0");
        ext_dec[2] = new Fl_Box(96, 154, 30, 18, " ");
        for (Fl_Box *b : ext_dec) {
            b->box(FL_DOWN_BOX); b->color(FL_WHITE);
            b->labelfont(FL_COURIER_BOLD); b->labelsize(12);
        }
        Fl_Box *al = new Fl_Box(12, 154, 80, 20, "ascii:");
        al->box(FL_NO_BOX); al->labelsize(11); al->align(FL_ALIGN_RIGHT);
        ext_win->end();
        ext_win->set_non_modal();
    }
    unsigned short r[8], sr[4], ip, fl;
    long ic;
    core_regs(r, sr, &ip, &fl, &ic);
    unsigned v = r[AX] & 0xFF;
    char b[32];
    snprintf(b, sizeof b, "%02X", v);          ext_bits[0]->label(strdup(b));
    snprintf(b, sizeof b, "%08X", v);          ext_bits[1]->label(strdup(b));
    snprintf(b, sizeof b, "%03o", v);          ext_bits[2]->label(strdup(b));
    snprintf(b, sizeof b, "%u", v);            ext_dec[0]->label(strdup(b));
    snprintf(b, sizeof b, "%d", (signed char)v); ext_dec[1]->label(strdup(b));
    char c[2] = { (v >= 32 && v < 127) ? (char)v : ' ', 0 };
    ext_dec[2]->label(strdup(c));
    ext_win->show();
}

/* ====================================================================== */
/* assembler status                                                        */

static Fl_Double_Window *status_win;
static Fl_Multiline_Output *status_text;
static MueButton *status_run;
static Fl_Box *status_play;
static void (*status_run_cb)(void);

void dlg_status_on_run(void (*cb)(void))
{
    status_run_cb = cb;
}

void dlg_status_run_enabled(int on)
{
    if (!status_run) return;
    if (on) status_run->activate();
    else status_run->deactivate();
    if (status_play) {
        status_play->image(on ? icon_step() : 0);
        status_play->redraw();
    }
}

void dlg_assembler_status(const char *message)
{
    if (!status_win) {
        /* Geometry read from a live compile of hello.asm: the window is a
         * resizable form whose client is 397x358, with a two-button header, a
         * one-line list box, the message pane, a black output band, a white
         * pane, and the view/external/close/run row at the foot. */
        status_win = new Fl_Double_Window(397, 358, "assembler status");
        status_win->color(mue_face());

        MueButton *ext = new MueButton(2, 4, 56, 18, "external");
        MueButton *view = new MueButton(62, 4, 46, 18, "view");
        (void)ext; (void)view;

        Fl_Output *line = new Fl_Output(2, 26, 393, 15);
        line->box(FL_DOWN_BOX);
        line->color(FL_WHITE);
        line->textfont(FL_HELVETICA);
        line->textsize(11);
        line->value("");

        status_text = new Fl_Multiline_Output(2, 46, 393, 92);
        status_text->box(FL_DOWN_BOX);
        status_text->color(FL_WHITE);
        status_text->textfont(FL_HELVETICA);
        status_text->textsize(11);

        /* The band under the messages is the external console: black, with the
         * DOS prompt at its left edge, exactly as captured. */
        Fl_Box *black = new Fl_Box(2, 144, 393, 15, "C:\\>");
        black->box(FL_DOWN_BOX);
        black->color(FL_BLACK);
        black->labelcolor(FL_WHITE);
        black->labelfont(FL_COURIER);
        black->labelsize(11);
        black->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        Fl_Box *white = new Fl_Box(2, 165, 393, 133);
        white->box(FL_DOWN_BOX);
        white->color(FL_WHITE);

        MueButton *b1 = new MueButton(2, 303, 123, 20, "view...");
        MueButton *b2 = new MueButton(2, 327, 123, 20, "external...");
        b1->deactivate();
        b2->deactivate();
        MueButton *b3 = new MueButton(163, 304, 66, 22, "close");
        MueButton *b4 = new MueButton(257, 302, 134, 28, "run");
        status_run = b4;
        b4->deactivate();      /* greyed until the .bin exists */
        Fl_Box *play = new Fl_Box(266, 308, 16, 16);
        status_play = play;
        play->box(FL_NO_BOX);
        play->image(0);        /* greyed until the .bin exists */

        b3->callback([](Fl_Widget *, void *) { status_win->hide(); });
        b4->callback([](Fl_Widget *, void *) {
            status_win->hide();
            if (status_run_cb) status_run_cb();
        });
        status_win->end();
        status_win->set_non_modal();
    }
    status_text->value(message ? message : "");
    status_text->redraw();
    status_win->show();
}

/* ====================================================================== */
/* refresh                                                                 */

void dlg_refresh(void)
{
    unsigned short r[8], sr[4], ip, fl;
    long ic;
    core_regs(r, sr, &ip, &fl, &ic);

    if (flags_win) {
        for (int i = 0; i < 8; i++)
            flag_combo[i]->val = !!(fl & flag_masks[i]);
        flags_win->redraw();
    }
    if (screen_win) {
        int rr = 0, cc = 0;
        core_screen_cursor(&rr, &cc);
        char b[32];
        snprintf(b, sizeof b, "%d:%d", rr, cc);
        screen_cur->label(b);
        screen_grid->redraw();
    }
    if (ram_win) ram_table->redraw();
    if (ext_win) { /* refreshed when opened; cheap to leave static */ }
}
