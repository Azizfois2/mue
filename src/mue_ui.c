/* mue_ui.c - the emulator's windows.
 *
 * Reconstructed from a description of the running emu8086 4.08, recorded in
 * re/UI_REFERENCE.md.  That description is the authority here; the older
 * documentation GIFs are secondary and, where they disagree, the description
 * wins because it is the same build being cloned.
 *
 * There are TWO windows, and they are genuinely different layouts:
 *
 *   1. the main editor   - menus file/edit/bookmarks/assembler/emulator/math/
 *                          ascii codes/help; a toolbar of icon+label buttons
 *                          in four groups; a line-numbered editor; a status
 *                          bar reading "drag a file here to open".
 *
 *   2. the emulator      - its own menus file/math/debug/view/external/virtual
 *                          devices/virtual drive/help; toolbar Load/reload/
 *                          step back/single step/run plus a step-delay slider;
 *                          registers (AX..DX as H+L byte pairs, then CS IP SS
 *                          SP BP SI DI DS ES); a memory view; a disassembly
 *                          view; and eight bottom buttons screen/source/reset/
 *                          aux/vars/debug/stack/flags.
 *
 * Keeping them in one file but as two separate window builders is deliberate:
 * my first attempt tried to draw both layouts into a single window and the
 * panels overlapped.
 *
 * What is NOT copied is VB6 chrome.  The user chose cross-platform over Win32
 * compatibility, so widget style is FLTK's own and renders identically
 * everywhere.  Layout and behaviour are reproduced, because those are what
 * make the tool usable.
 *
 * This is a SEPARATE binary from `mue`.  The verified CLI links no toolkit, so
 * the test suite's oracle can never be perturbed by UI work.
 */
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.h>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>

#include "mue_icons.h"
#include "mue_highlight.h"
#include "mue_source.h"
#include "mue_core.h"
#include "mue_tools.h"
#include "cpu.h"
#include <FL/fl_draw.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FLTK's color() argument is an INDEX into its colour table, not an RGB
 * value.  Passing 0xFFFFFF does not give white: it indexes far past the end
 * of the table and lands on whatever entry happens to be there - on this
 * build, cyan.  That is what painted every panel cyan.
 *
 * These are the standard FLTK colour indices, which resolve to the right
 * system colours on every platform: */
#define C_BG      FL_BACKGROUND_COLOR    /* light grey face            */
#define C_TOOLBAR FL_BACKGROUND_COLOR
#define C_GUTTER  FL_DARK3               /* editor gutter, darker grey */
#define C_NUMCOL  FL_LIGHT2              /* line-number strip          */
#define C_FIELD   FL_WHITE               /* white text fields          */
#define C_TEXT    FL_FOREGROUND_COLOR    /* black                      */
#define C_DIM     FL_DARK2               /* dimmed labels              */
#define C_CHANGED FL_YELLOW              /* a register that moved      */
#define C_GREEN   FL_DARK_GREEN

/* ====================================================================== */
/* state                                                                   */

typedef struct {
    unsigned short ax, bx, cx, dx, sp, bp, si, di;
    unsigned short cs, ds, es, ss, ip, flags;
    long icount;
    int valid;
} RegState;

static const char *GENREGS[] = { "AX", "BX", "CX", "DX" };
#define NGEN ((int)(sizeof GENREGS / sizeof GENREGS[0]))

/* Order taken from the description: CS, IP, SS, SP, BP, SI, DI, DS, ES. */
static const char *SEGREGS[] = {
    "CS", "IP", "SS", "SP", "BP", "SI", "DI", "DS", "ES"
};
#define NSEG ((int)(sizeof SEGREGS / sizeof SEGREGS[0]))

static const struct { const char *n; unsigned short m; } FLAGDEFS[] = {
    {"CF", 0x0001}, {"PF", 0x0004}, {"AF", 0x0010}, {"ZF", 0x0040},
    {"SF", 0x0080}, {"TF", 0x0100}, {"IF", 0x0200}, {"DF", 0x0400},
    {"OF", 0x0800},
};
#define NFLAG ((int)(sizeof FLAGDEFS / sizeof FLAGDEFS[0]))

static Fl_Box    *regh[NGEN], *regl[NGEN], *segbox[NSEG];




/* A single CPU flag: name on top, 0/1 value below, clickable to toggle.
 * In the original these cells can be clicked to flip the flag, so this is a
 * button rather than a label - the value is the point of the panel, and a
 * label cannot be interacted with. */
class FlagCell : public Fl_Widget {
public:
    FlagCell(int x, int y, int w, int h, const char *nm, unsigned short mask)
        : Fl_Widget(x, y, w, h, 0), bit(mask), val(0) {
        strncpy(name, nm, sizeof name - 1);
        name[sizeof name - 1] = 0;
        box(FL_DOWN_BOX);
        color(FL_WHITE);
    }
    char name[4];
    unsigned short bit;
    int val;
    void value(int v) { val = v ? 1 : 0; color(val ? FL_YELLOW : FL_WHITE); redraw(); }
    int handle(int e) {
        if (e == FL_PUSH) { value(!val); do_callback(); return 1; }
        return Fl_Widget::handle(e);
    }
    void draw() {
        draw_box(box(), x(), y(), w(), h(), color());
        /* Dark text: the labels were faded grey and unreadable on white. */
        fl_font(FL_HELVETICA, 10);
        fl_color(32, 32, 32);
        fl_draw(name, x() + 1, y() + 4, w() - 2, 14, FL_ALIGN_CENTER);
        char v[2] = { (char)('0' + val), 0 };
        fl_font(FL_COURIER_BOLD, 12);
        /* Bold, and nudged down so the value is not flush against the name. */
        fl_color(val ? 0 : 90, val ? 0 : 90, val ? 0 : 90);
        fl_draw(v, x() + 1, y() + 19, w() - 2, 15, FL_ALIGN_CENTER);
    }
};

/* The memory list.  Each row has four fields in four colours - grey address,
 * green byte, black decimal, bold black ASCII - so it cannot be a plain
 * Fl_Multiline_Output, which paints all its text one colour.  Drawing the
 * rows by hand also gives the taller row spacing the original has. */
class HexList : public Fl_Widget {
public:
    HexList(int x, int y, int w, int h)
        : Fl_Widget(x, y, w, h, 0), nrows(0), base(0) {}
    int nrows;
    /* Physical address shown in the first row. */
    unsigned base;
    /* Row height and font size, chosen to match the original: bold 13pt
     * monospace on 18px rows. */
    static const int ROW_H = 18;

    /* One row per byte: address, the byte in hex, the same in decimal, and
     * the character if it is printable. The bytes come from the live
     * machine, not from a placeholder - a memory view that does not show
     * memory is worse than no memory view. */
    void draw() {
        draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
        fl_push_clip(x() + 1, y() + 1, w() - 2, h() - 2);
        const int cx_addr = x() + 8;
        const int cx_hex  = x() + w() - 128;
        const int cx_dec  = x() + w() - 92;
        const int cx_asc  = x() + w() - 52;
        for (int i = 0, yy = y() + 2; i < nrows && yy + ROW_H <= y() + h() - 2;
             i++, yy += ROW_H) {
            char a[24], hex[8], dec[8], asc[8];
            unsigned char v = core_peek8(base + (unsigned)i);
            snprintf(a, sizeof a, "%05X:", base + (unsigned)i);
            snprintf(hex, sizeof hex, "%02X", v);
            snprintf(dec, sizeof dec, "%03d", v);
            if (v >= 32 && v < 127 && v != ' ') {
                asc[0] = (char)v; asc[1] = 0;
            } else {
                snprintf(asc, sizeof asc, "NULL");
            }
            fl_font(FL_COURIER, 13);
            fl_color(112, 112, 112);
            fl_draw(a, cx_addr, yy, 66, ROW_H, FL_ALIGN_LEFT);
            fl_font(FL_COURIER_BOLD, 13);
            fl_color(0, 128, 0);
            fl_draw(hex, cx_hex, yy, 26, ROW_H, FL_ALIGN_LEFT);
            fl_color(FL_BLACK);
            fl_draw(dec, cx_dec, yy, 34, ROW_H, FL_ALIGN_LEFT);
            fl_draw(asc, cx_asc, yy, 46, ROW_H, FL_ALIGN_LEFT);
        }
        fl_pop_clip();
    }
    /* Rows that actually fit, so the caller does not have to guess. */
    int rows_that_fit() const { return (h() - 6) / ROW_H; }
};

static HexList *mem_list;   /* the memory panel, filled from live state */
/* The gutter: one grey line number per source line, right-aligned, drawn at
 * the same line spacing the editor uses.  Drawn by hand because a multi-line
 * label wraps at the box width and produced garbled text. */
class LineGutter : public Fl_Widget {
public:
    LineGutter(int x, int y, int w, int h) : Fl_Widget(x, y, w, h, 0) {}
    void draw() {
        draw_box(box() ? box() : FL_FLAT_BOX, x(), y(), w(), h(), color());
        fl_font(FL_COURIER, 11);
        fl_color(153, 153, 153);
        int lh = 15;
        for (int i = 1, yy = y() + 3; yy < y() + h() && i <= 200; i++, yy += lh) {
            char b[8];
            snprintf(b, sizeof b, "%02d", i);
            fl_draw(b, x() + w() - 26, yy, 22, lh, FL_ALIGN_RIGHT);
        }
    }
};

static FlagCell  *flagcell[NFLAG];
static SourceView          *editor_text;
static char                 guest_src[16384];
static int                  run_fast;   /* run with no step delay */
static Fl_Double_Window    *emu_window;   /* hidden until `emulate` */
static Fl_Multiline_Output *screen_area, *dis_area;
static Fl_Box    *status_editor, *status_emul;
static RegState   cur, prev;

/* Startup register state.  The real emulator shows CS=0050 and everything
 * else zero before any program is loaded, and its address fields read
 * "0050:0000" - so CS has to start at 0050 or the panel contradicts itself. */
static void init_startup_state(void)
{
    memset(&cur, 0, sizeof cur);
    memset(&prev, 0, sizeof prev);
    cur.cs = prev.cs = 0x0050;
    cur.valid = prev.valid = 1;
}

static unsigned short reg_value(const RegState *s, const char *n)
{
    if (!strcmp(n, "AX")) { return s->ax; }
    if (!strcmp(n, "BX")) { return s->bx; }
    if (!strcmp(n, "CX")) { return s->cx; }
    if (!strcmp(n, "DX")) { return s->dx; }
    if (!strcmp(n, "SP")) { return s->sp; }
    if (!strcmp(n, "BP")) { return s->bp; }
    if (!strcmp(n, "SI")) { return s->si; }
    if (!strcmp(n, "DI")) { return s->di; }
    if (!strcmp(n, "CS")) { return s->cs; }
    if (!strcmp(n, "DS")) { return s->ds; }
    if (!strcmp(n, "ES")) { return s->es; }
    if (!strcmp(n, "SS")) { return s->ss; }
    if (!strcmp(n, "IP")) { return s->ip; }
    return 0;
}

/* Highlight only the half that moved: if AL changed, the L box turns yellow
 * and the H box does not.  That is the point of the H/L split. */
static void refresh_state(void)
{
    int i;
    for (i = 0; i < NGEN; i++) {
        char bh[8], bl[8];
        unsigned short v = reg_value(&cur, GENREGS[i]);
        unsigned short p = reg_value(&prev, GENREGS[i]);
        int moved = cur.valid && prev.valid && v != p;
        snprintf(bh, sizeof bh, "%02X", (v >> 8) & 0xFF);
        snprintf(bl, sizeof bl, "%02X", v & 0xFF);
        regh[i]->copy_label(bh);
        regl[i]->copy_label(bl);
        regh[i]->color(moved ? C_CHANGED : C_FIELD);
        regl[i]->color(moved ? C_CHANGED : C_FIELD);
        regh[i]->redraw();
        regl[i]->redraw();
    }
    for (i = 0; i < NSEG; i++) {
        char b[8];
        unsigned short v = reg_value(&cur, SEGREGS[i]);
        unsigned short p = reg_value(&prev, SEGREGS[i]);
        int moved = cur.valid && prev.valid && v != p;
        snprintf(b, sizeof b, "%04X", v);
        segbox[i]->copy_label(b);
        segbox[i]->color(moved ? C_CHANGED : C_FIELD);
        segbox[i]->redraw();
    }
    for (i = 0; i < NFLAG; i++) {
        if (!flagcell[i]) continue;
        flagcell[i]->value(cur.valid && (cur.flags & FLAGDEFS[i].m) != 0);
    }
    if (status_emul && cur.valid) {
        char b[96];
        snprintf(b, sizeof b, "%ld instructions executed", cur.icount);
        status_emul->copy_label(b);
        status_emul->redraw();
    }
}

/* ====================================================================== */
/* driving the core in-process                                             */

/* Pull the machine state out of the core into the panels' RegState. */
static void state_from_core(RegState *st)
{
    unsigned short r8[8], sr4[4], ip, flags;
    long ic;
    core_regs(r8, sr4, &ip, &flags, &ic);
    st->ax = r8[AX]; st->cx = r8[CX]; st->dx = r8[DX]; st->bx = r8[BX];
    st->sp = r8[SP]; st->bp = r8[BP]; st->si = r8[SI]; st->di = r8[DI];
    st->es = sr4[ES]; st->cs = sr4[CS]; st->ss = sr4[SS]; st->ds = sr4[DS];
    st->ip = ip;
    st->flags = flags;
    st->icount = ic;
    st->valid = 1;
}

/* Redraw the memory and disassembly panels from live machine state. */
static void refresh_views(void)
{
    unsigned short cs, ip;
    char buf[4096];

    core_cs_ip(&cs, &ip);

    if (dis_area) {
        core_disasm_list(buf, sizeof buf, 22);
        dis_area->value(buf);
        dis_area->redraw();
    }
    if (mem_list) {
        /* The memory panel shows the bytes at CS:IP, as the original does
         * by default. */
        mem_list->base = ((uint32_t)cs << 4) + ip;
        mem_list->nrows = mem_list->rows_that_fit();
        mem_list->redraw();
    }
    if (screen_area) {
        char scr[2200];
        core_screen_text(scr, sizeof scr);
        screen_area->value(scr);
        screen_area->redraw();
    }
}

static void cb_emulate(Fl_Widget *, void *)
{
    /* Emulating is what brings the emulator window up - it is not shown at
     * startup. */
    if (emu_window && !emu_window->shown()) {
        emu_window->show();
        /* A window built while hidden has never been laid out, so its
         * children still have their construction positions and nothing has
         * been drawn. resize() forces the layout pass, redraw() then paints
         * every child. Without both the window came up blank. */
        emu_window->resize(emu_window->x(), emu_window->y(),
                           emu_window->w(), emu_window->h());
        emu_window->redraw();
        Fl::check();
        Fl::flush();
    }
    if (!editor_text) return;

    snprintf(guest_src, sizeof guest_src, "%s",
             editor_text->text().c_str());
    {
        char err[512];
        if (!core_load(guest_src, err, sizeof err)) {
            if (status_emul) {
                status_emul->copy_label(err[0] ? err : "assembly failed");
                status_emul->redraw();
            }
            if (status_editor) {
                status_editor->copy_label(err[0] ? err : "assembly failed");
                status_editor->redraw();
            }
            return;
        }
    }

    prev = cur;
    core_run(4000000);
    state_from_core(&cur);
    refresh_state();
    refresh_views();
    if (status_editor) {
        status_editor->copy_label("emulated");
        status_editor->redraw();
    }
}

static void cb_open_calc(Fl_Widget *, void *)  { tools_open_calculator(); }
static void cb_open_conv(Fl_Widget *, void *)  { tools_open_convertor(); }
static void cb_open_opts(Fl_Widget *, void *)  { tools_open_options(); }

static void cb_compile(Fl_Widget *, void *)
{
    if (status_editor) {
        const char *t = editor_text ? editor_text->text().c_str() : "";
        status_editor->copy_label(t && t[0] ? "compiled" : "nothing to compile");
        status_editor->redraw();
    }
}

static void cb_reset(Fl_Widget *, void *)
{
    memset(&cur, 0, sizeof cur);
    memset(&prev, 0, sizeof prev);
    if (screen_area) screen_area->value("");
    if (dis_area) dis_area->value("");
    for (int i = 0; i < NGEN; i++) {
        regh[i]->copy_label("00"); regh[i]->color(C_FIELD); regh[i]->redraw();
        regl[i]->copy_label("00"); regl[i]->color(C_FIELD); regl[i]->redraw();
    }
    for (int i = 0; i < NSEG; i++) {
        segbox[i]->copy_label("0000"); segbox[i]->color(C_FIELD);
        segbox[i]->redraw();
    }
    for (int i = 0; i < NFLAG; i++) if (flagcell[i]) flagcell[i]->value(0);
    refresh_state();
    if (status_emul) { status_emul->copy_label("reset"); status_emul->redraw(); }
}

/* One instruction, with the panels updated afterwards - this is the whole
 * point of linking the core into the UI rather than scraping the CLI. */
static void cb_step(Fl_Widget *, void *)
{
    if (!core_loaded()) {
        /* Nothing loaded yet: load and run nothing, so a user who steps
         * first gets a sensible starting state rather than a dead button. */
        cb_emulate(0, 0);
        return;
    }
    prev = cur;
    if (!core_step()) {
        if (status_emul) {
            status_emul->copy_label(core_halted() ? "halted"
                                                  : "cannot step further");
            status_emul->redraw();
        }
        return;
    }
    state_from_core(&cur);
    refresh_state();
    refresh_views();
    if (status_emul) {
        char b[96];
        snprintf(b, sizeof b, "step %ld", cur.icount);
        status_emul->copy_label(b);
        status_emul->redraw();
    }
}

/* Continuous run: step on a timer so the screen and registers stay live
 * instead of freezing until the program ends. */
static void run_timer(void *)
{
    if (!core_loaded() || core_halted()) {
        if (status_emul) {
            status_emul->copy_label(core_halted() ? "halted" : "stopped");
            status_emul->redraw();
        }
        return;
    }
    prev = cur;
    core_run(run_fast ? 20000 : 1000);
    state_from_core(&cur);
    refresh_state();
    refresh_views();
    if (status_emul) {
        char b[96];
        snprintf(b, sizeof b, "%ld instructions executed", cur.icount);
        status_emul->copy_label(b);
        status_emul->redraw();
    }
    if (!core_halted()) Fl::repeat_timeout(0.05, run_timer);
    else if (status_emul) {
        status_emul->copy_label("halted");
        status_emul->redraw();
    }
}

static void cb_run(Fl_Widget *, void *)
{
    if (!core_loaded()) { cb_emulate(0, 0); return; }
    if (core_halted()) return;
    Fl::add_timeout(0.05, run_timer);
}

static const char *zero_memory_disasm(void)
{
    static char buf[2048];
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i < 20; i++) {
        n += snprintf(buf + n, sizeof buf - n, "ADD [BX + SI], AL\n");
        if (n >= (int)sizeof buf - 32) break;
    }
    strncat(buf, "...", sizeof buf - strlen(buf) - 1);
    return buf;
}

/* ====================================================================== */
/* window 1: the main editor                                               */

static Fl_Double_Window *build_editor(void)
{
    const int W = 1000, H = 640;
    Fl_Double_Window *win = new Fl_Double_Window(W, H,
                                     "mue - 8086 assembler and emulator");
    win->color(C_BG);

    /* The eight menus of the real application, lowercase. */
    Fl_Menu_Bar *mb = new Fl_Menu_Bar(0, 0, W, 24);
    mb->textfont(FL_HELVETICA);
    mb->textsize(12);
    mb->add("file/new", 0, 0, 0, 0);
    mb->add("file/open...", 0, 0, 0, 0);
    mb->add("file/save", 0, 0, 0, 0);
    mb->add("file/exit", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/undo", 0, 0, 0, 0);
    mb->add("edit/cut", 0, 0, 0, 0);
    mb->add("edit/copy", 0, 0, 0, 0);
    mb->add("edit/paste", 0, 0, 0, 0);
    mb->add("bookmarks/set bookmark", 0, 0, 0, 0);
    mb->add("bookmarks/clear bookmarks", 0, 0, 0, 0);
    mb->add("assembler/compile", 0, cb_compile, 0, 0);
    mb->add("emulator/emulate", 0, cb_emulate, 0, 0);
    mb->add("emulator/single step", 0, cb_step, 0, 0);
    mb->add("emulator/reset", 0, cb_reset, 0, 0);
    mb->add("math/calculator", 0, cb_open_calc, 0, 0);
    mb->add("math/base convertor", 0, cb_open_conv, 0, 0);
    mb->add("ascii codes/table", 0, 0, 0, 0);
    mb->add("help/about", 0, 0, 0, 0);

    /* Toolbar: icon above label, four groups split by separators. */
    Fl_Group *tb = new Fl_Group(0, 24, W, 48);
    tb->box(FL_FLAT_BOX);
    tb->color(C_TOOLBAR);
    struct Btn { const char *n; Fl_Callback *cb; Fl_Image *ic; int drop; }
    btns[] = {
        {"new",      0,          icon_new(),     0},
        {"open",     0,          icon_open(),    0},
        {"examples", 0,          icon_star(),    1},   /* has a dropdown  */
        {"save",     0,          icon_save(),    1},   /* has a dropdown  */
        {"compile",  cb_compile, icon_compile(), 0},
        {"emulate",  cb_emulate, icon_run(),     0},
        {"calculator", cb_open_calc, icon_calc(),    0},
        {"convertor",  cb_open_conv, icon_convert(), 0},
        {"options",    cb_open_opts, icon_options(), 0},
        {"help",     0,          icon_help(),    0},
        {"about",    0,          icon_about(),   0},
    };
    const int nbtn = (int)(sizeof btns / sizeof btns[0]);
    int bx = 8;
    for (int i = 0; i < nbtn; i++) {
        if (i == 4 || i == 6 || i == 8) {           /* group separators */
            /* A 1px vertical line, not a 6px down-box: the box rendered as
             * a small rounded button instead of a separator. */
            Fl_Box *sp = new Fl_Box(bx, 30, 1, 40);
            sp->box(FL_FLAT_BOX);
            sp->color(FL_DARK3);
            bx += 8;
        }
        Fl_Button *b = make_tool(bx, 28, 74, 46, btns[i].n, btns[i].ic);
        if (btns[i].drop) {                 /* small black down-arrow */
            Fl_Box *ar = new Fl_Box(bx + 60, 30, 12, 10, "@2>>");
            ar->box(FL_NO_BOX);
            ar->labelsize(8);
            ar->labelcolor(FL_BLACK);
        }
        if (btns[i].cb) b->callback(btns[i].cb);
        bx += 78;
    }
    tb->end();

    /* Editor: gutter, line numbers, text, scrollbar - the dominant area. */
    Fl_Box *gutter = new Fl_Box(0, 72, 26, 520);
    gutter->box(FL_FLAT_BOX);
    gutter->color(C_GUTTER);

    {
        LineGutter *nums = new LineGutter(26, 72, 34, 520);
        nums->color(C_NUMCOL);
    }

    /* SourceView, not Fl_Text_Editor. FLTK's per-character style table is
     * accepted but never consulted when drawing in this build, so the editor
     * draws its own tokenised runs (see mue_source.h). */
    {
        editor_text = new SourceView(60, 72, 924, 520);
        editor_text->box(FL_DOWN_FRAME);
        editor_text->color(C_FIELD);
        editor_text->font_size(14);
        editor_text->set_text(
            "org 100h\n"
            "; a small demo of the editor\n"
            "mov ax,1234h\n"
            "mov bx,5678h\n"
            "mov cx,9ABCh\n"
            "mov dx,0DEF0h\n"
            "ret\n");
    }

    status_editor = new Fl_Box(0, H - 22, W, 20, "drag a file here to open");
    status_editor->box(FL_THIN_DOWN_BOX);
    status_editor->color(C_BG);
    status_editor->selection_color(C_BG);
    status_editor->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_editor->labelsize(11);

    win->end();
    return win;
}

/* ====================================================================== */
/* the emulator window                                                     */

/* Put a group's title on its frame border, as the original does, instead of
 * inside the group where it collides with the first child. */
/* A group title sitting IN the top border line, the way the original draws
 * it: a short run of the border is blanked and the text drawn there, with a
 * little clearance on each side so it does not touch the frame.
 *
 * FL_ALIGN_TOP alone is not enough - it puts the label above the widget, so
 * the text floats over whatever is above the group and reads as stray
 * writing. This subclass paints the notch and the text itself. */
class TitledFrame : public Fl_Group {
public:
    TitledFrame(int x, int y, int w, int h, const char *t)
        : Fl_Group(x, y, w, h, t)
    {
        box(FL_ENGRAVED_FRAME);
        color(C_BG);
        label(t);
        labelfont(FL_HELVETICA);
        labelsize(11);
        labelcolor(C_TEXT);
        align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    void draw() {
        /* Frame and children first, then the notch over the top border. */
        Fl_Group::draw();
        draw_label();
    }
    void draw_label() {
        const char *t = label();
        if (!t || !*t) return;
        fl_font(labelfont(), labelsize());
        int tw = 0, th = 0;
        fl_measure(t, tw, th, 0);
        /* Sit on the top border: 2px of the frame line above and below. */
        int ty = y() + 2;
        int tx = x() + 8;
        /* Blank the border run behind the text so the line does not strike
         * through the letters. */
        fl_color(color());
        fl_rectf(tx - 3, ty, tw + 6, th);
        fl_color(labelcolor());
        fl_draw(t, tx, ty, tw, th, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
};

static void title_on_border(Fl_Group *g, const char *title)
{
    /* Groups that need the notch are TitledFrame already; for a plain group
     * keep the old behaviour but left-aligned inside so nothing floats. */
    g->label(title);
    g->labelfont(FL_HELVETICA);
    g->labelsize(11);
    g->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g->labelcolor(C_TEXT);
}

static Fl_Double_Window *build_emulator(void)
{
    const int M = 6, W = 812, H = 812;
    /* All bands are derived from the one above, never hard-coded, and the
     * assertion below aborts if they ever overlap. */
    const int PANEL_Y = 86, PANEL_H = 384;
    const int FLAGS_Y = PANEL_Y + PANEL_H + 4, FLAGS_H = 96;
    const int BTN_Y = FLAGS_Y + FLAGS_H + 8, BTN_H = 26;
    const int SCREEN_Y = BTN_Y + BTN_H + 8;
    const int SCREEN_H = H - SCREEN_Y - 34;

    if (!(FLAGS_Y >= PANEL_Y + PANEL_H &&
          BTN_Y >= FLAGS_Y + FLAGS_H &&
          SCREEN_Y >= BTN_Y + BTN_H && SCREEN_H > 40)) {
        fprintf(stderr, "mue_ui: layout bands overlap\n");
        abort();
    }
    fprintf(stderr, "mue_ui bands: PANEL %d..%d FLAGS %d..%d BTN %d..%d "
            "SCREEN %d..%d H=%d\n",
            PANEL_Y, PANEL_Y + PANEL_H, FLAGS_Y, FLAGS_Y + FLAGS_H,
            BTN_Y, BTN_Y + BTN_H, SCREEN_Y, SCREEN_Y + SCREEN_H, H);

    Fl_Double_Window *win = new Fl_Double_Window(W, H,
                                     "8086 microprocessor emulator");
    win->color(C_BG);

    /* Its own menu bar, different from the editor's. */
    Fl_Menu_Bar *mb = new Fl_Menu_Bar(0, 0, W, 24);
    mb->textfont(FL_HELVETICA);
    mb->textsize(12);
    mb->add("file/load", 0, 0, 0, 0);
    mb->add("file/save", 0, 0, 0, 0);
    mb->add("math/base convertor", 0, cb_open_conv, 0, 0);
    mb->add("math/calculator", 0, cb_open_calc, 0, 0);
    mb->add("debug/single step", 0, 0, 0, 0);
    mb->add("debug/run", 0, 0, 0, 0);
    mb->add("view/memory", 0, 0, 0, 0);
    mb->add("virtual devices/screen", 0, 0, 0, 0);
    mb->add("help/about", 0, 0, 0, 0);

    /* ---- toolbar: load, reload, step back, single step, run + delay ---- */
    {
        Fl_Group *tb = new Fl_Group(0, 24, W, 54);
        tb->box(FL_FLAT_BOX);
        tb->color(C_TOOLBAR);
        static const char *nm[] = { "load", "reload", "step back",
                                    "single step", "run" };
        Fl_Image *ic[] = { icon_reload(), icon_reload(), icon_step_back(),
                           icon_step(), icon_run() };
        /* step back is disabled in the original until something has run, so
         * it is greyed here too rather than silently doing nothing. */
        for (int i = 0; i < 5; i++) {
            Fl_Button *b = make_tool(6 + i * 88, 28, 84, 46, nm[i], ic[i]);
            if (i == 3) b->callback(cb_step);
            if (i == 4) b->callback(cb_run);
            if (i == 2) { b->deimage(icon_grayed(icon_step_back())); b->deactivate(); }
        }
        Fl_Slider *sl = new Fl_Slider(6 + 5 * 88 + 14, 38, 190, 16, "");
        sl->type(FL_HOR_NICE_SLIDER);
        sl->color(C_BG);
        sl->selection_color(FL_DARK2);
        sl->range(0, 10);
        sl->value(0);
        Fl_Box *slb = new Fl_Box(6 + 5 * 88 + 8, 24, 200, 14,
                                 "step delay");
        slb->labelsize(10);
        slb->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        tb->end();
    }

    /* ---- registers: AX..DX as H/L byte pairs, then the nine segments ---- */
    {
        Fl_Group *g = new Fl_Group(M, PANEL_Y, 178, PANEL_H);
        g->box(FL_ENGRAVED_FRAME);
        g->color(C_BG);
        int y = PANEL_Y + 16;
        for (int i = 0; i < NGEN; i++) {
            Fl_Box *lbl = new Fl_Box(M + 8, y + 4, 34, 20, GENREGS[i]);
            lbl->labelsize(12);
            lbl->labelfont(FL_HELVETICA_BOLD);
            lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            regh[i] = new Fl_Box(M + 46, y, 34, 26, "00");
            regl[i] = new Fl_Box(M + 84, y, 34, 26, "00");
            Fl_Box *rn[2] = { regh[i], regl[i] };
            for (int k = 0; k < 2; k++) {
                rn[k]->box(FL_DOWN_BOX);
                rn[k]->color(C_FIELD);
                rn[k]->labelfont(FL_COURIER_BOLD);
                rn[k]->labelsize(14);
            }
            Fl_Box *hl = new Fl_Box(M + 40, y - 2, 20, 12, "h");
            hl->labelsize(9); hl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
            y += 27;
        }
        y += 6;
        for (int i = 0; i < NSEG; i++) {
            Fl_Box *lbl = new Fl_Box(M + 8, y + 3, 34, 20, SEGREGS[i]);
            lbl->labelsize(12);
            lbl->labelfont(FL_HELVETICA_BOLD);
            lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            segbox[i] = new Fl_Box(M + 46, y, 70, 26, "0000");
            segbox[i]->box(FL_DOWN_BOX);
            segbox[i]->color(C_FIELD);
            segbox[i]->labelfont(FL_COURIER_BOLD);
            segbox[i]->labelsize(14);
            y += 27;
        }
        title_on_border(g, "registers");
        g->end();
    }

    /* ---- memory: address box, list, proportional scrollbar ---- */
    {
        Fl_Group *mp = new Fl_Group(M + 190, PANEL_Y, 300, PANEL_H, "memory");
        mp->box(FL_ENGRAVED_FRAME);
        mp->color(C_BG);
        Fl_Input *maddr = new Fl_Input(M + 190 + 60, 96, 130, 28, "");
        maddr->box(FL_DOWN_BOX);
        maddr->value("0050:0000");
        maddr->textfont(FL_COURIER_BOLD);
        maddr->textsize(14);

        HexList *hl = new HexList(M + 196, 130, 272, PANEL_H - 52);
        hl->nrows = hl->rows_that_fit();
        mem_list = hl;
        Fl_Scrollbar *sb = new Fl_Scrollbar(M + 190 + 274, 130, 16,
                                            PANEL_H - 52);
        sb->linesize(1);
        sb->range(0, 65536 - hl->nrows);
        /* thumb proportional to how much is visible, as in the original */
        sb->slider_size((double)hl->nrows / 65536.0);
        title_on_border(mp, "memory");
        mp->end();
    }

    /* ---- disassembly ---- */
    {
        Fl_Group *dp = new Fl_Group(M + 498, PANEL_Y, W - 2 * M - 498,
                                    PANEL_H, "disassembly");
        dp->box(FL_ENGRAVED_FRAME);
        dp->color(C_BG);
        Fl_Input *daddr = new Fl_Input(M + 498 + 90, 96, 130, 28, "");
        daddr->box(FL_DOWN_BOX);
        daddr->value("0050:0000");
        daddr->textfont(FL_COURIER_BOLD);
        daddr->textsize(14);

        dis_area = new Fl_Multiline_Output(M + 504, 130,
                                           W - 2 * M - 498 - 28,
                                           PANEL_H - 52);
        dis_area->box(FL_DOWN_BOX);
        dis_area->color(C_FIELD);
        dis_area->textfont(FL_COURIER_BOLD);
        dis_area->textsize(13);
        dis_area->value(zero_memory_disasm());
        Fl_Scrollbar *sb = new Fl_Scrollbar(W - M - 18, 130, 16, PANEL_H - 52);
        sb->linesize(16);
        sb->range(0, 4096);
        sb->slider_size(0.35);
        title_on_border(dp, "disassembly");
        dp->end();
    }

    /* ---- flags ---- */
    {
        Fl_Group *g = new Fl_Group(M, FLAGS_Y, 182, FLAGS_H);
        g->box(FL_ENGRAVED_FRAME);
        g->color(C_BG);
        for (int i = 0; i < NFLAG; i++) {
            int cx = M + 8 + (i % 5) * 34;
            int cy = FLAGS_Y + 12 + (i / 5) * 36;
            flagcell[i] = new FlagCell(cx, cy, 34, 36, FLAGDEFS[i].n, FLAGDEFS[i].m);
        }
        title_on_border(g, "flags");
        g->end();
    }

    /* ---- the eight bottom buttons ---- */
    {
        static const char *nm[] = { "screen", "source", "reset", "aux",
                                    "vars", "debug", "stack", "flags" };
        for (int i = 0; i < 8; i++) {
            Fl_Button *b = new Fl_Button(M + i * 88, BTN_Y, 84, BTN_H, nm[i]);
            b->box(FL_UP_BOX);
            b->labelsize(11);
            if (i == 2) b->callback(cb_reset);
        }
    }
    /* The buttons are created before the screen panel so that they paint on
     * top of it; creating them after left them hidden behind the panel. */
    {
        TitledFrame *g = new TitledFrame(M, SCREEN_Y, W - 2 * M, SCREEN_H, "screen");
        screen_area = new Fl_Multiline_Output(M + 6, SCREEN_Y + 16,
                                              W - 2 * M - 12, SCREEN_H - 22);
        screen_area->box(FL_DOWN_BOX);
        screen_area->color(FL_BLACK);
        screen_area->textcolor(FL_GREEN);
        screen_area->textfont(FL_COURIER_BOLD);
        screen_area->textsize(13);
        screen_area->value("");
        g->end();
    }

    status_emul = new Fl_Box(0, H - 22, W, 20, "ready");
    status_emul->box(FL_THIN_DOWN_BOX);
    status_emul->color(C_BG);
    status_emul->selection_color(C_BG);
    status_emul->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_emul->labelsize(11);

    win->end();
    return win;
}

int main(int argc, char **argv)
{
    /* No Fl::scheme() call here. The scheme was originally set to "none" to
     * work around cyan fields, but the real cause of that was passing packed
     * RGB literals to color(); the constants now use FLTK table indices.
     * Worse, any scheme call installs its own text style table and silently
     * discards the one highlight_data() was given, which is why the editor
     * rendered every token in black. */

    init_startup_state();

    /* The real application opens the EDITOR only. The emulator window is a
     * result of pressing `emulate`, not something on screen from the start.
     * It is built up front but kept hidden, so its state can be reused. */
    Fl_Double_Window *ed = build_editor();
    emu_window = build_emulator();      /* built, but not shown */
    ed->show(argc, argv);
    /* Test hook: MUE_OPEN_TOOLS=options,convertor,calculator opens the
     * floating tool windows at startup so a check can verify each one
     * without driving menus through synthetic clicks. */
    {
        const char *tk = getenv("MUE_OPEN_TOOLS");
        if (tk) {
            if (strstr(tk, "options"))    tools_open_options();
            if (strstr(tk, "convertor"))  tools_open_convertor();
            if (strstr(tk, "calculator")) tools_open_calculator();
        }
    }
    ed->redraw();
    Fl::flush();
    refresh_state();
    return Fl::run();
}
