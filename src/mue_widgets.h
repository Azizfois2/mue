/* mue_widgets.h - the Win32/VB6 look, rebuilt in FLTK.
 *
 * emu8086 is a VB6 program: form face #F0F0F0, sunken white fields, 1px raised
 * buttons, bold monospace values, flat white menus. FLTK's defaults are a
 * different language entirely (grey bevels, Helvetica everywhere), so the
 * widgets the emulator window is built from are drawn here rather than
 * configured one property at a time.
 *
 * All colours below were sampled from a live capture of emu8086 4.08; see
 * re/UI_SPEC.md section 1.
 */
#ifndef MUE_WIDGETS_H
#define MUE_WIDGETS_H

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Image.H>

/* measured palette */
Fl_Color mue_face(void);       /* #F0F0F0 form face                 */
Fl_Color mue_field(void);      /* #FFFFFF text fields               */
Fl_Color mue_blue(void);       /* #0000FF addresses / atoms         */
Fl_Color mue_green(void);      /* #008000 hex bytes                 */
Fl_Color mue_grey(void);       /* #808080 decimals / dim text       */
Fl_Color mue_gutter_bg(void);  /* #E6E6E6 line-number strip         */
Fl_Color mue_gutter_line(void);/* #A0A0A0 gutter notch              */
Fl_Color mue_rail(void);       /* #AAAAAA editor left rail          */
Fl_Color mue_yellow(void);     /* #FFFF00 current source line       */
Fl_Color mue_red(void);        /* #FF0000 flag 1 / control codes    */

void mue_ui_font(int size = 11);   /* MS Sans Serif stand-in */
void mue_mono_font(int size = 12); /* bold monospace stand-in */

/* VB6 command-button frame: 1px black outline, white highlight on the top and
 * left, #808080 shadow on the bottom and right, swapped while held down. */
void mue_vb_box(int x, int y, int w, int h, int pressed);

/* Ordinary action button, as VB6 draws it. */
class MueButton : public Fl_Button {
public:
    MueButton(int X, int Y, int W, int H, const char *L = 0)
        : Fl_Button(X, Y, W, H, L) {
        box(FL_UP_BOX);
        down_box(FL_DOWN_BOX);
        color(mue_face());
        labelcolor(FL_BLACK);
        labelfont(FL_HELVETICA);
        labelsize(11);
        align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }
    void draw() override;
};

/* Toolbar button: icon above a lowercase label, optional black drop arrow.
 * The editor's strip is flat; the emulator's carries a 1px border. */
class MueTool : public Fl_Button {
public:
    MueTool(int X, int Y, int W, int H, const char *L, Fl_Image *ic,
            int drop = 0, int border = 0, int labelsz = 11)
        : Fl_Button(X, Y, W, H, L), icon_(ic), drop_(drop), border_(border) {
        box(FL_UP_BOX);
        down_box(FL_DOWN_BOX);
        color(mue_face());
        labelcolor(FL_BLACK);
        labelfont(FL_HELVETICA);
        labelsize(labelsz);
        align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }
    Fl_Image *icon_ref() const { return icon_; }
    void draw() override;

private:
    Fl_Image *icon_;
    int drop_;
    int border_;
};

/* The emulator's step-delay control: a thin groomed track with a square
 * thumb, six ticks under it, and the caption under that (measured at
 * 432..552 x 3..40 in the original client). */
class MueSlider : public Fl_Widget {
public:
    MueSlider(int X, int Y, int W, int H);
    int  value() const { return val_; }
    void value(int v) { if (v < 0) v = 0; if (v > max_) v = max_; val_ = v; redraw(); }
    void range(int m) { max_ = m > 0 ? m : 1; }
    void draw() override;
    int  handle(int ev) override;

private:
    int val_, max_;
    void set_from_x(int x);
};

/* VB6 frame: 1px etched rectangle with the caption sitting in the top line. */
class MueGroup : public Fl_Group {
public:
    MueGroup(int X, int Y, int W, int H, const char *t)
        : Fl_Group(X, Y, W, H, 0), title_(t) {
        box(FL_ENGRAVED_FRAME);
        color(mue_face());
    }
    void draw() override;

private:
    const char *title_;
};

/* Sunken white value field. `mono` picks the bold monospace face used for
 * registers and addresses; otherwise the label face is used. */
class MueField : public Fl_Input {
public:
    MueField(int X, int Y, int W, int H, int mono = 1)
        : Fl_Input(X, Y, W, H) {
        box(FL_DOWN_BOX);
        color(FL_WHITE);
        selection_color(mue_blue());
        textcolor(FL_BLACK);
        textfont(mono ? FL_COURIER_BOLD : FL_HELVETICA);
        textsize(mono ? 12 : 11);
        align(FL_ALIGN_CENTER);
    }
};

/* One physical byte per row: `07100: EB 235 e`, exactly as emu8086 draws it.
 * The bytes of the instruction at CS:IP are selected in blue. */
class MemList : public Fl_Widget {
public:
    MemList(int X, int Y, int W, int H);
    void base(unsigned phys) { base_ = phys; redraw(); }
    unsigned base() const { return base_; }
    void selection_phys(unsigned p, int n) { selp_ = p; seln_ = n; redraw(); }
    void pick_fn(void (*f)(unsigned phys, void *u)) { pick_ = f; }
    void user(void *u) { user_ = u; }
    int rows_visible() const;
    void draw() override;
    int handle(int ev) override;

private:
    unsigned base_;
    unsigned selp_;
    int seln_;
    void (*pick_)(unsigned, void *);
    void *user_;
};

/* One instruction per row, no address column, current CS:IP selected. */
class DisasmList : public Fl_Widget {
public:
    DisasmList(int X, int Y, int W, int H);
    void anchor(unsigned short seg, unsigned short off) {
        seg_ = seg; off_ = off; redraw();
    }
    void current(unsigned short seg, unsigned short off) {
        cseg_ = seg; coff_ = off; redraw();
    }
    void selection(unsigned short seg, unsigned short off) {
        sseg_ = seg; soff_ = off; ssel_ = 1; redraw();
    }
    unsigned short anchor_off() const { return off_; }
    unsigned short anchor_seg() const { return seg_; }
    int row_at(int y) const;
    void pick_fn(void (*f)(unsigned short seg, unsigned short off, void *u)) {
        pick_ = f;
    }
    void user(void *u) { user_ = u; }
    int rows_visible() const;
    void draw() override;
    int handle(int ev) override;

private:
    unsigned short seg_, off_;
    unsigned short cseg_, coff_;
    unsigned short sseg_, soff_;
    int ssel_;
    void (*pick_)(unsigned short, unsigned short, void *);
    void *user_;
};

/* 16 bytes per row hex dump for the RAM window. */
class HexTable : public Fl_Widget {
public:
    HexTable(int X, int Y, int W, int H);
    void anchor(unsigned short seg, unsigned short off) {
        seg_ = seg; off_ = off; redraw();
    }
    void list_mode(int on) { list_ = on ? 1 : 0; redraw(); }
    void draw() override;
    int handle(int ev) override;

private:
    unsigned short seg_, off_;
    int list_;
    int top_;
};

/* stack: SSSS:OOOO followed by the word at it, starting at SS:SP and going
 * down in 2-byte steps. */
class StackList : public Fl_Widget {
public:
    StackList(int X, int Y, int W, int H);
    void set(unsigned short ss, unsigned short sp) {
        ss_ = ss; sp_ = sp; sel_ = 0; redraw();
    }
    void draw() override;
    int handle(int ev) override;

private:
    unsigned short ss_, sp_;
    int sel_;
};

/* Editor status bar: line / col / hint, four sunken panels + size grip. */
class MueStatus : public Fl_Widget {
public:
    MueStatus(int X, int Y, int W, int H)
        : Fl_Widget(X, Y, W, H), line_(1), col_(1) {
        box(FL_FLAT_BOX);
        color(mue_face());
    }
    void caret(int line, int col) { line_ = line; col_ = col; redraw(); }
    void hint(const char *h) { hint_ = h ? h : ""; redraw(); }
    void draw() override;

private:
    int line_, col_;
    const char *hint_ = "drag a file here to open";
};

/* The guest text screen: 80x25 cells, black background, CP437 glyphs. */
class ScreenGrid : public Fl_Widget {
public:
    ScreenGrid(int X, int Y, int W, int H);
    void draw() override;
};

/* ALU bit grid (435x132 in the original). */
class AluGrid : public Fl_Widget {
public:
    AluGrid(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    void draw() override;
};

#endif
