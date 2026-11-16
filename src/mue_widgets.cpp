/* mue_widgets.cpp - VB6-shaped controls and the live-data panes. */
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Scrollbar.H>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mue_widgets.h"
#include "mue_core.h"
#include "cpu.h"

/* ---- palette ---------------------------------------------------------- */

static Fl_Color rgb(unsigned v)
{
    return fl_rgb_color((unsigned char)(v >> 16), (unsigned char)(v >> 8),
                        (unsigned char)v);
}

Fl_Color mue_face(void)        { return rgb(0xF0F0F0); }
Fl_Color mue_field(void)       { return rgb(0xFFFFFF); }
Fl_Color mue_blue(void)        { return rgb(0x0000FF); }
Fl_Color mue_green(void)       { return rgb(0x008000); }
Fl_Color mue_grey(void)        { return rgb(0x808080); }
Fl_Color mue_gutter_bg(void)   { return rgb(0xE6E6E6); }
Fl_Color mue_gutter_line(void) { return rgb(0xA0A0A0); }
Fl_Color mue_rail(void)        { return rgb(0xAAAAAA); }
Fl_Color mue_yellow(void)      { return rgb(0xFFFF00); }
Fl_Color mue_red(void)         { return rgb(0xFF0000); }

void mue_ui_font(int size)   { fl_font(FL_HELVETICA, size); }
void mue_mono_font(int size) { fl_font(FL_COURIER_BOLD, size); }

void mue_vb_box(int X, int Y, int W, int H, int pressed)
{
    /* Sampled from the emulator's bottom row: a black hairline all round, then
     * a white bevel on the top/left and #808080 on the bottom/right, with the
     * face colour filling the middle.  While the button is held the bevel
     * inverts, which is what VB6 does instead of moving the label. */
    fl_color(FL_BLACK);
    fl_rect(X, Y, W, H);
    if (W < 4 || H < 4) return;

    fl_color(pressed ? mue_grey() : mue_field());
    fl_rectf(X + 1, Y + 1, W - 2, 1);
    fl_rectf(X + 1, Y + 1, 1, H - 2);
    fl_color(pressed ? mue_field() : mue_grey());
    fl_rectf(X + 1, Y + H - 2, W - 2, 1);
    fl_rectf(X + W - 2, Y + 1, 1, H - 2);

    fl_color(mue_face());
    fl_rectf(X + 2, Y + 2, W - 4, H - 4);
}

/* ---- toolbar button --------------------------------------------------- */

void MueTool::draw()
{
    /* The editor's toolbar is flat: the bevel appears only while the button
     * is under the pointer or pressed. The emulator's buttons carry a
     * permanent VB6 border, so `border_` selects that shape. */
    int pressed = Fl::pushed() == this || value();
    if (border_)
        mue_vb_box(x(), y(), w(), h(), pressed);
    else if (pressed || Fl::belowmouse() == this)
        draw_box(FL_UP_BOX, x(), y(), w(), h(), mue_face());
    else
        draw_box(FL_FLAT_BOX, x(), y(), w(), h(), mue_face());

    int off = pressed ? 1 : 0;
    int isz = 16;
    int iy = y() + (border_ ? 4 : 2) + off;
    if (icon_) {
        /* Icons are materialised at 16x16 by ICON_END; draw at the image's
         * own size so FLTK never has to clip or rescale it. */
        int iw = icon_->w() ? icon_->w() : isz;
        int ih = icon_->h() ? icon_->h() : isz;
        icon_->draw(x() + (w() - iw) / 2 + off, iy + (isz - ih) / 2);
    }
    mue_ui_font(labelsize());
    fl_color(active_r() ? FL_BLACK : mue_grey());
    int tw = (int)fl_width(label());
    int tx = x() + (w() - tw) / 2 + off;
    int ly = y() + h() - (border_ ? 17 : 14) + off;
    fl_draw(label(), tx, ly, tw, 13,
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

    if (drop_) {
        /* small black down triangle just after the word, as in the original
         * ("examples v"), not at the far edge of the button */
        int cx = tx + tw + 6, cy = ly + 5;
        fl_color(FL_BLACK);
        fl_polygon(cx - 3, cy, cx + 3, cy, cx, cy + 3);
    }
}

/* ---- ordinary command button ------------------------------------------ */

void MueButton::draw()
{
    int pressed = Fl::pushed() == this || value();
    mue_vb_box(x(), y(), w(), h(), pressed);
    mue_ui_font(labelsize());
    fl_color(active_r() ? labelcolor() : mue_grey());
    const char *t = label() ? label() : "";
    fl_draw(t, x() + 1 + (pressed ? 1 : 0), y() + 1 + (pressed ? 1 : 0),
            w() - 2, h() - 2, FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
}

/* ---- step-delay slider ------------------------------------------------ */

MueSlider::MueSlider(int X, int Y, int W, int H)
    : Fl_Widget(X, Y, W, H, 0), val_(0), max_(10)
{
    box(FL_NO_BOX);
    color(mue_face());
}

void MueSlider::set_from_x(int px)
{
    /* The thumb's centre rides a 102px groove that starts 2px right of
     * its leftmost seat, so both ends stay inside the track. */
    int x0 = x() + 10, x1 = x() + 112;
    if (x1 <= x0) x1 = x0 + 1;
    int v = (int)((long)(px - x0) * max_ / (x1 - x0) + 0.5);
    if (v < 0) v = 0;
    if (v > max_) v = max_;
    value(v);
}

void MueSlider::draw()
{
    /* Measured from cap4/win1.png (widget at content 414,2): the groove runs
     * x 426..528 in five shaded rows, the 5x10 thumb hangs above it, six
     * ticks sit at x 426/446/467/487/508/528, and the caption is left
     * aligned at x 429 - it is not centred under the track. */
    const int x0 = x() + 12, x1 = x() + 114;
    const int ty = y() + 4;

    fl_color(rgb(0xB0B0B0)); fl_rectf(x0, ty,     x1 - x0 + 1, 1);
    fl_color(rgb(0x747474)); fl_rectf(x0, ty + 1, x1 - x0 + 1, 1);
    fl_color(rgb(0xCBCBCB)); fl_rectf(x0, ty + 2, x1 - x0 + 1, 1);
    fl_color(rgb(0xF9F9F9)); fl_rectf(x0, ty + 3, x1 - x0 + 1, 1);
    fl_color(rgb(0xF3F3F3)); fl_rectf(x0, ty + 4, x1 - x0 + 1, 1);

    int tw = 5, th = 10;
    int tx = x0 - 2 + (int)(102.0 * val_ / (max_ ? max_ : 1) + 0.5);
    draw_box(FL_UP_BOX, tx, ty - 2, tw, th, mue_face());

    fl_color(FL_BLACK);
    for (int i = 0; i <= 5; i++) {
        int px = x0 + (i * (x1 - x0) + 2) / 5;
        fl_rectf(px, y() + 13, 1, 4);
    }

    char b[48];
    snprintf(b, sizeof b, "step delay ms: %d", val_);
    mue_ui_font(11);
    fl_color(FL_BLACK);
    fl_draw(b, x() + 15, y() + 27, w() - 18, 11,
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

int MueSlider::handle(int ev)
{
    if (ev == FL_PUSH || ev == FL_DRAG) {
        set_from_x(Fl::event_x());
        if (when() & FL_WHEN_CHANGED) do_callback();
        if (ev == FL_PUSH) return 1;
        return 1;
    }
    return Fl_Widget::handle(ev);
}

/* ---- group box -------------------------------------------------------- */

void MueGroup::draw()
{
    draw_box(FL_ENGRAVED_FRAME, x(), y(), w(), h(), mue_face());
    if (title_ && *title_) {
        /* VB6 puts the caption *on* the frame line, half above and half
         * below it, and leaves a gap in the line behind the text. */
        mue_ui_font(11);
        int tw = (int)fl_width(title_);
        fl_color(mue_face());
        fl_rectf(x() + 7, y() - 4, tw + 6, 10);
        fl_color(FL_BLACK);
        fl_draw(title_, x() + 10, y() + 4);
    }
    draw_children();
}

/* ---- memory list ------------------------------------------------------ */

MemList::MemList(int X, int Y, int W, int H)
    : Fl_Widget(X, Y, W, H, 0), base_(0), selp_(0), seln_(0),
      pick_(0), user_(0)
{
    box(FL_DOWN_BOX);
    color(FL_WHITE);
}

int MemList::rows_visible() const { return (h() - 2) / 12; }

/* emu8086 prints the stored byte as the OEM character it would print, so
 * 0x0E is a note and 0xEB a delta; only the handful of control codes with a
 * mnemonic in the original keep a name.  The table is UTF-8 because
 * fl_draw() expects UTF-8. */
static const char *byte_char(unsigned char v)
{
    static const char *const cp437[256] = {
"\x4e\x55\x4c\x4c",       /* 00 */
    "\xe2\x98\xba",           /* 01 */
    "\xe2\x98\xbb",           /* 02 */
    "\xe2\x99\xa5",           /* 03 */
    "\xe2\x99\xa6",           /* 04 */
    "\xe2\x99\xa3",           /* 05 */
    "\xe2\x99\xa0",           /* 06 */
    "\xe2\x80\xa2",           /* 07 */
    "\xe2\x97\x98",           /* 08 */
    "\xe2\x97\x8b",           /* 09 */
    "\xe2\x97\x99",           /* 0A */
    "\xe2\x99\x82",           /* 0B */
    "\xe2\x99\x80",           /* 0C */
    "\xe2\x99\xaa",           /* 0D */
    "\xe2\x99\xab",           /* 0E */
    "\xe2\x98\xbc",           /* 0F */
    "\xe2\x96\xba",           /* 10 */
    "\xe2\x97\x84",           /* 11 */
    "\xe2\x86\x95",           /* 12 */
    "\xe2\x80\xbc",           /* 13 */
    "\xc2\xb6",               /* 14 */
    "\xc2\xa7",               /* 15 */
    "\xe2\x96\xac",           /* 16 */
    "\xe2\x86\xa8",           /* 17 */
    "\xe2\x86\x91",           /* 18 */
    "\xe2\x86\x93",           /* 19 */
    "\xe2\x86\x92",           /* 1A */
    "\xe2\x86\x90",           /* 1B */
    "\xe2\x88\x9f",           /* 1C */
    "\xe2\x86\x94",           /* 1D */
    "\xe2\x96\xb2",           /* 1E */
    "\xe2\x96\xbc",           /* 1F */
    "\x20",                   /* 20 */
    "\x21",                   /* 21 */
    "\x22",                   /* 22 */
    "\x23",                   /* 23 */
    "\x24",                   /* 24 */
    "\x25",                   /* 25 */
    "\x26",                   /* 26 */
    "\x27",                   /* 27 */
    "\x28",                   /* 28 */
    "\x29",                   /* 29 */
    "\x2a",                   /* 2A */
    "\x2b",                   /* 2B */
    "\x2c",                   /* 2C */
    "\x2d",                   /* 2D */
    "\x2e",                   /* 2E */
    "\x2f",                   /* 2F */
    "\x30",                   /* 30 */
    "\x31",                   /* 31 */
    "\x32",                   /* 32 */
    "\x33",                   /* 33 */
    "\x34",                   /* 34 */
    "\x35",                   /* 35 */
    "\x36",                   /* 36 */
    "\x37",                   /* 37 */
    "\x38",                   /* 38 */
    "\x39",                   /* 39 */
    "\x3a",                   /* 3A */
    "\x3b",                   /* 3B */
    "\x3c",                   /* 3C */
    "\x3d",                   /* 3D */
    "\x3e",                   /* 3E */
    "\x3f",                   /* 3F */
    "\x40",                   /* 40 */
    "\x41",                   /* 41 */
    "\x42",                   /* 42 */
    "\x43",                   /* 43 */
    "\x44",                   /* 44 */
    "\x45",                   /* 45 */
    "\x46",                   /* 46 */
    "\x47",                   /* 47 */
    "\x48",                   /* 48 */
    "\x49",                   /* 49 */
    "\x4a",                   /* 4A */
    "\x4b",                   /* 4B */
    "\x4c",                   /* 4C */
    "\x4d",                   /* 4D */
    "\x4e",                   /* 4E */
    "\x4f",                   /* 4F */
    "\x50",                   /* 50 */
    "\x51",                   /* 51 */
    "\x52",                   /* 52 */
    "\x53",                   /* 53 */
    "\x54",                   /* 54 */
    "\x55",                   /* 55 */
    "\x56",                   /* 56 */
    "\x57",                   /* 57 */
    "\x58",                   /* 58 */
    "\x59",                   /* 59 */
    "\x5a",                   /* 5A */
    "\x5b",                   /* 5B */
    "\x5c",                   /* 5C */
    "\x5d",                   /* 5D */
    "\x5e",                   /* 5E */
    "\x5f",                   /* 5F */
    "\x60",                   /* 60 */
    "\x61",                   /* 61 */
    "\x62",                   /* 62 */
    "\x63",                   /* 63 */
    "\x64",                   /* 64 */
    "\x65",                   /* 65 */
    "\x66",                   /* 66 */
    "\x67",                   /* 67 */
    "\x68",                   /* 68 */
    "\x69",                   /* 69 */
    "\x6a",                   /* 6A */
    "\x6b",                   /* 6B */
    "\x6c",                   /* 6C */
    "\x6d",                   /* 6D */
    "\x6e",                   /* 6E */
    "\x6f",                   /* 6F */
    "\x70",                   /* 70 */
    "\x71",                   /* 71 */
    "\x72",                   /* 72 */
    "\x73",                   /* 73 */
    "\x74",                   /* 74 */
    "\x75",                   /* 75 */
    "\x76",                   /* 76 */
    "\x77",                   /* 77 */
    "\x78",                   /* 78 */
    "\x79",                   /* 79 */
    "\x7a",                   /* 7A */
    "\x7b",                   /* 7B */
    "\x7c",                   /* 7C */
    "\x7d",                   /* 7D */
    "\x7e",                   /* 7E */
    "\xe2\x8c\x82",           /* 7F */
    "\xc3\x87",               /* 80 */
    "\xc3\xbc",               /* 81 */
    "\xc3\xa9",               /* 82 */
    "\xc3\xa2",               /* 83 */
    "\xc3\xa4",               /* 84 */
    "\xc3\xa0",               /* 85 */
    "\xc3\xa5",               /* 86 */
    "\xc3\xa7",               /* 87 */
    "\xc3\xaa",               /* 88 */
    "\xc3\xab",               /* 89 */
    "\xc3\xa8",               /* 8A */
    "\xc3\xaf",               /* 8B */
    "\xc3\xae",               /* 8C */
    "\xc3\xac",               /* 8D */
    "\xc3\x84",               /* 8E */
    "\xc3\x85",               /* 8F */
    "\xc3\x89",               /* 90 */
    "\xc3\xa6",               /* 91 */
    "\xc3\x86",               /* 92 */
    "\xc3\xb4",               /* 93 */
    "\xc3\xb6",               /* 94 */
    "\xc3\xb2",               /* 95 */
    "\xc3\xbb",               /* 96 */
    "\xc3\xb9",               /* 97 */
    "\xc3\xbf",               /* 98 */
    "\xc3\x96",               /* 99 */
    "\xc3\x9c",               /* 9A */
    "\xc2\xa2",               /* 9B */
    "\xc2\xa3",               /* 9C */
    "\xc2\xa5",               /* 9D */
    "\xe2\x82\xa7",           /* 9E */
    "\xc6\x92",               /* 9F */
    "\xc3\xa1",               /* A0 */
    "\xc3\xad",               /* A1 */
    "\xc3\xb3",               /* A2 */
    "\xc3\xba",               /* A3 */
    "\xc3\xb1",               /* A4 */
    "\xc3\x91",               /* A5 */
    "\xc2\xaa",               /* A6 */
    "\xc2\xba",               /* A7 */
    "\xc2\xbf",               /* A8 */
    "\xe2\x8c\x90",           /* A9 */
    "\xc2\xac",               /* AA */
    "\xc2\xbd",               /* AB */
    "\xc2\xbc",               /* AC */
    "\xc2\xa1",               /* AD */
    "\xc2\xab",               /* AE */
    "\xc2\xbb",               /* AF */
    "\xe2\x96\x91",           /* B0 */
    "\xe2\x96\x92",           /* B1 */
    "\xe2\x96\x93",           /* B2 */
    "\xe2\x94\x82",           /* B3 */
    "\xe2\x94\xa4",           /* B4 */
    "\xe2\x95\xa1",           /* B5 */
    "\xe2\x95\xa2",           /* B6 */
    "\xe2\x95\x96",           /* B7 */
    "\xe2\x95\x95",           /* B8 */
    "\xe2\x95\xa3",           /* B9 */
    "\xe2\x95\x91",           /* BA */
    "\xe2\x95\x97",           /* BB */
    "\xe2\x95\x9d",           /* BC */
    "\xe2\x95\x9c",           /* BD */
    "\xe2\x95\x9b",           /* BE */
    "\xe2\x94\x90",           /* BF */
    "\xe2\x94\x94",           /* C0 */
    "\xe2\x94\xb4",           /* C1 */
    "\xe2\x94\xac",           /* C2 */
    "\xe2\x94\x9c",           /* C3 */
    "\xe2\x94\x80",           /* C4 */
    "\xe2\x94\xbc",           /* C5 */
    "\xe2\x95\x9e",           /* C6 */
    "\xe2\x95\x9f",           /* C7 */
    "\xe2\x95\x9a",           /* C8 */
    "\xe2\x95\x94",           /* C9 */
    "\xe2\x95\xa9",           /* CA */
    "\xe2\x95\xa6",           /* CB */
    "\xe2\x95\xa0",           /* CC */
    "\xe2\x95\x90",           /* CD */
    "\xe2\x95\xac",           /* CE */
    "\xe2\x95\xa7",           /* CF */
    "\xe2\x95\xa8",           /* D0 */
    "\xe2\x95\xa4",           /* D1 */
    "\xe2\x95\xa5",           /* D2 */
    "\xe2\x95\x99",           /* D3 */
    "\xe2\x95\x98",           /* D4 */
    "\xe2\x95\x92",           /* D5 */
    "\xe2\x95\x93",           /* D6 */
    "\xe2\x95\xab",           /* D7 */
    "\xe2\x95\xaa",           /* D8 */
    "\xe2\x94\x98",           /* D9 */
    "\xe2\x94\x8c",           /* DA */
    "\xe2\x96\x88",           /* DB */
    "\xe2\x96\x84",           /* DC */
    "\xe2\x96\x8c",           /* DD */
    "\xe2\x96\x90",           /* DE */
    "\xe2\x96\x80",           /* DF */
    "\xce\xb1",               /* E0 */
    "\xc3\x9f",               /* E1 */
    "\xce\x93",               /* E2 */
    "\xcf\x80",               /* E3 */
    "\xce\xa3",               /* E4 */
    "\xcf\x83",               /* E5 */
    "\xc2\xb5",               /* E6 */
    "\xcf\x84",               /* E7 */
    "\xce\xa6",               /* E8 */
    "\xce\x98",               /* E9 */
    "\xce\xa9",               /* EA */
    "\xce\xb4",               /* EB */
    "\xe2\x88\x9e",           /* EC */
    "\xcf\x86",               /* ED */
    "\xce\xb5",               /* EE */
    "\xe2\x88\xa9",           /* EF */
    "\xe2\x89\xa1",           /* F0 */
    "\xc2\xb1",               /* F1 */
    "\xe2\x89\xa5",           /* F2 */
    "\xe2\x89\xa4",           /* F3 */
    "\xe2\x8c\xa0",           /* F4 */
    "\xe2\x8c\xa1",           /* F5 */
    "\xc3\xb7",               /* F6 */
    "\xe2\x89\x88",           /* F7 */
    "\xc2\xb0",               /* F8 */
    "\xe2\x88\x99",           /* F9 */
    "\xc2\xb7",               /* FA */
    "\xe2\x88\x9a",           /* FB */
    "\xe2\x81\xbf",           /* FC */
    "\xc2\xb2",               /* FD */
    "\xe2\x96\xa0",           /* FE */
    "\xc2\xa0",               /* FF */
    };
    return cp437[v];
}

static const char *byte_name(unsigned char v)
{
    switch (v) {
    case 0x00: return "NULL";
    case 0x07: return "BEP";
    case 0x08: return "BAK";
    case 0x09: return "TAB";
    case 0x0A: return "LIN";
    case 0x0D: return "CRE";
    case 0x20: return "SPA";
    }
    return 0;
}

void MemList::draw()
{
    draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
    fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);

    /* Measured columns in cap4/win1.png: every row starts 10px inside the
     * frame (content x 131 for a list at 121), the address is six 8px
     * cells, hex starts at +66, the decimal at +90 and the character at
     * +122.  Courier bold 13 gives the 8px advance the original uses. */
    const int ax = x() + 10;
    const int hx = x() + 66;
    const int dx = x() + 90;
    const int cx = x() + 122;
    int rows = rows_visible();

    for (int i = 0; i < rows; i++) {
        unsigned phys = base_ + (unsigned)i;
        int yy = y() + 1 + i * 12;
        int lit = seln_ > 0 && phys >= selp_ &&
                  phys < selp_ + (unsigned)seln_;
        if (lit) {
            fl_color(mue_blue());
            fl_rectf(x() + 1, yy, w() - 2, 12);
        }
        unsigned char v = core_peek8(phys);
        char a[16], h[4], d[5];
        snprintf(a, sizeof a, "%05X:", phys & 0xFFFFF);
        snprintf(h, sizeof h, "%02X", v);
        snprintf(d, sizeof d, "%03d", v);
        const char *nm = byte_name(v);

        mue_mono_font(13);
        fl_color(lit ? FL_WHITE : mue_grey());
        fl_draw(a, ax, yy, 56, 12, FL_ALIGN_LEFT);
        fl_color(lit ? FL_WHITE : mue_green());
        fl_draw(h, hx, yy, 20, 12, FL_ALIGN_LEFT);
        fl_color(lit ? FL_WHITE : mue_grey());
        fl_draw(d, dx, yy, 30, 12, FL_ALIGN_LEFT);
        fl_color(lit ? FL_WHITE : FL_BLACK);
        fl_draw(nm ? nm : byte_char(v), cx, yy, 70, 12, FL_ALIGN_LEFT);
    }
    fl_pop_clip();
}

int MemList::handle(int ev)
{
    if (ev == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        int row = (Fl::event_y() - y() - 1) / 12;
        if (row >= 0 && row < rows_visible() && pick_)
            pick_(base_ + (unsigned)row, user_);
        return 1;
    }
    if (ev == FL_MOUSEWHEEL) {
        int d = Fl::event_dy() * 16;
        long nb = (long)base_ + d;
        if (nb < 0) nb = 0;
        if (nb > 0xFFFFF) nb = 0xFFFFF;
        base_ = (unsigned)nb & 0xFFFFF;
        redraw();
        return 1;
    }
    return Fl_Widget::handle(ev);
}

/* ---- disassembly list ------------------------------------------------- */

DisasmList::DisasmList(int X, int Y, int W, int H)
    : Fl_Widget(X, Y, W, H, 0), seg_(0x0700), off_(0x0100),
      cseg_(0x0700), coff_(0x0100), sseg_(0), soff_(0), ssel_(0),
      pick_(0), user_(0)
{
    box(FL_DOWN_BOX);
    color(FL_WHITE);
}

int DisasmList::rows_visible() const { return (h() - 2) / 12; }

int DisasmList::row_at(int yy) const { return (yy - y() - 1) / 12; }

/* emu8086 prints the pane in upper case and tags hex literals with a
 * trailing 'h' ("JMP 0110h", "DEC AX", "DB 65h", "SUB AL, 020h"); its
 * core also puts a space after the operand comma.  The engine's
 * disassembler speaks lower case without the suffix, so fold each row into
 * the house style here rather than teaching the CPU about cosmetics. */
static void house_style(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < cap; ) {
        char c = in[i];
        if (c == ',') {
            out[o++] = ',';
            if (in[i + 1] != ' ') out[o++] = ' ';
            i++;
            continue;
        }
        if (isalnum((unsigned char)c)) {
            char tok[32];
            size_t n = 0;
            while (in[i] && n + 1 < sizeof tok &&
                   isalnum((unsigned char)in[i]))
                tok[n++] = in[i++];
            tok[n] = 0;
            int hex = 1, digit = 0;
            for (size_t k = 0; k < n; k++) {
                char t = (char)toupper((unsigned char)tok[k]);
                if (!((t >= '0' && t <= '9') || (t >= 'A' && t <= 'F')))
                    hex = 0;
                if (t >= '0' && t <= '9') digit = 1;
            }
            for (size_t k = 0; k < n && o + 2 < cap; k++)
                out[o++] = (char)toupper((unsigned char)tok[k]);
            if (hex && digit && o + 1 < cap) out[o++] = 'h';
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = 0;
}

void DisasmList::draw()
{
    draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
    fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);

    unsigned short addrs[64];
    int lens[64];
    char text[64 * 64];
    int rows = rows_visible();
    if (rows > 64) rows = 64;
    core_disasm_rows(seg_, off_, rows, addrs, lens, text, 64);

    for (int i = 0; i < rows; i++) {
        int yy = y() + 1 + i * 12;
        int cur = (seg_ == cseg_ && addrs[i] == coff_);
        int sel = ssel_ && seg_ == sseg_ && addrs[i] == soff_;
        if (cur || sel) {
            fl_color(mue_blue());
            fl_rectf(x() + 1, yy, w() - 2, 12);
        }
        char line[96];
        house_style(text + (size_t)i * 64, line, sizeof line);
        mue_mono_font(13);
        fl_color((cur || sel) ? FL_WHITE : FL_BLACK);
        fl_draw(line, x() + 10, yy, w() - 12, 12, FL_ALIGN_LEFT);
    }
    fl_pop_clip();
}

int DisasmList::handle(int ev)
{
    if (ev == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        int row = row_at(Fl::event_y());
        if (row >= 0 && row < rows_visible()) {
            unsigned short a = off_;
            for (int i = 0; i < row; i++) {
                int len = core_insn_len_at(seg_, a);
                if (len <= 0) break;
                a = (unsigned short)(a + len);
            }
            sseg_ = seg_; soff_ = a; ssel_ = 1;
            if (pick_) pick_(seg_, a, user_);
            redraw();
        }
        return 1;
    }
    if (ev == FL_MOUSEWHEEL) {
        int n = Fl::event_dy();
        while (n > 0) {            /* scroll down one instruction */
            int len = core_insn_len_at(seg_, off_);
            off_ = (unsigned short)(off_ + (len > 0 ? len : 1));
            n--;
        }
        while (n < 0) {            /* scroll up: walk a window back */
            unsigned short p = off_;
            for (int i = 0; i < 8 && p > 0; i++) {
                p--;
                /* accept the first address whose instruction ends at off_ */
                int len = core_insn_len_at(seg_, p);
                if (len > 0 && (unsigned short)(p + len) == off_) {
                    off_ = p;
                    break;
                }
            }
            n++;
        }
        redraw();
        return 1;
    }
    return Fl_Widget::handle(ev);
}

/* ---- RAM window hex table --------------------------------------------- */

HexTable::HexTable(int X, int Y, int W, int H)
    : Fl_Widget(X, Y, W, H, 0), seg_(0x0700), off_(0x0100), list_(0), top_(0)
{
    box(FL_DOWN_BOX);
    color(FL_WHITE);
}

void HexTable::draw()
{
    draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
    fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);
    mue_mono_font(12);
    int rows = (h() - 6) / 18;
    if (list_) {
        for (int i = 0; i < rows; i++) {
            unsigned phys = ((unsigned)seg_ << 4) + off_ + (unsigned)top_ + i;
            unsigned char v = core_peek8(phys & 0xFFFFF);
            char a[16], h[4], d[4], c[2] = { (char)v, 0 };
            snprintf(a, sizeof a, "%05X:", phys & 0xFFFFF);
            snprintf(h, sizeof h, "%02X", v);
            snprintf(d, sizeof d, "%03d", v);
            int yy = y() + 3 + i * 18;
            fl_color(rgb(0x707070)); fl_draw(a, x() + 6, yy, 60, 18, FL_ALIGN_LEFT);
            fl_color(mue_green());   fl_draw(h, x() + 70, yy, 30, 18, FL_ALIGN_LEFT);
            fl_color(mue_grey());    fl_draw(d, x() + 104, yy, 30, 18, FL_ALIGN_LEFT);
            fl_color(FL_BLACK);
            fl_draw((v >= 32 && v < 127) ? c : ".", x() + 138, yy, 20, 18,
                    FL_ALIGN_LEFT);
        }
    } else {
        for (int i = 0; i < rows; i++) {
            unsigned so = (unsigned)(off_ + top_ + i * 16) & 0xFFFF;
            unsigned phys = ((unsigned)seg_ << 4) + so;
            char a[16];
            snprintf(a, sizeof a, "%04X:%04X", seg_, so);
            int yy = y() + 3 + i * 18;
            fl_color(FL_BLACK);
            fl_draw(a, x() + 6, yy, 76, 18, FL_ALIGN_LEFT);
            for (int j = 0; j < 16; j++) {
                unsigned char v = core_peek8((phys + (unsigned)j) & 0xFFFFF);
                char h[4];
                snprintf(h, sizeof h, "%02X", v);
                fl_color(FL_BLACK);
                fl_draw(h, x() + 88 + j * 21, yy, 20, 18, FL_ALIGN_LEFT);
            }
            fl_color(rgb(0x404040));
            char asc[18];
            for (int j = 0; j < 16; j++) {
                unsigned char v = core_peek8((phys + (unsigned)j) & 0xFFFFF);
                asc[j] = (v >= 32 && v < 127) ? (char)v : '.';
            }
            asc[16] = 0;
            fl_draw(asc, x() + 88 + 16 * 21 + 6, yy, 16 * 8, 18, FL_ALIGN_LEFT);
        }
    }
    fl_pop_clip();
}

int HexTable::handle(int ev)
{
    if (ev == FL_MOUSEWHEEL) {
        top_ -= Fl::event_dy() * (list_ ? 1 : 16);
        if (top_ < 0) top_ = 0;
        redraw();
        return 1;
    }
    if (ev == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        int row = (Fl::event_y() - y() - 3) / 18;
        if (row < 0) row = 0;
        top_ += row * (list_ ? 1 : 16);
        redraw();
        return 1;
    }
    return Fl_Widget::handle(ev);
}

/* ---- stack list ------------------------------------------------------- */

StackList::StackList(int X, int Y, int W, int H)
    : Fl_Widget(X, Y, W, H, 0), ss_(0x0700), sp_(0xFFFE), sel_(0)
{
    box(FL_DOWN_BOX);
    color(FL_WHITE);
}

void StackList::draw()
{
    draw_box(FL_DOWN_BOX, x(), y(), w(), h(), FL_WHITE);
    fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);
    mue_mono_font(12);
    int rows = (h() - 4) / 18;
    for (int i = 0; i < rows; i++) {
        unsigned off = (unsigned)(sp_ - i * 2) & 0xFFFF;
        int yy = y() + 2 + i * 18;
        if (i == sel_) {
            fl_color(mue_blue());
            fl_rectf(x() + 2, yy, w() - 4, 18);
        }
        char a[16], v[8];
        snprintf(a, sizeof a, "%04X:%04X", ss_, off);
        snprintf(v, sizeof v, "%04X",
                 core_peek16(((unsigned)ss_ << 4) + off));
        fl_color(i == sel_ ? FL_WHITE : FL_BLACK);
        fl_draw(a, x() + 4, yy, 70, 18, FL_ALIGN_LEFT);
        fl_draw(v, x() + 80, yy, 50, 18, FL_ALIGN_LEFT);
    }
    fl_pop_clip();
}

int StackList::handle(int ev)
{
    if (ev == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
        int r = (Fl::event_y() - y() - 2) / 18;
        if (r >= 0 && r < (h() - 4) / 18) { sel_ = r; redraw(); }
        return 1;
    }
    return Fl_Widget::handle(ev);
}

/* ---- status bar ------------------------------------------------------- */

void MueStatus::draw()
{
    draw_box(FL_FLAT_BOX, x(), y(), w(), h(), mue_face());
    /* Measured panel edges in cap4/win2.png's 722px client: four boxes with
     * borders at x 1, 75, 172 and 366; the last one runs to 646 and carries
     * the hint text, which sits around x 424..552 rather than the box's
     * true centre. */
    struct { int x, w; const char *t; } p[4];
    static char l[32], c[32];
    snprintf(l, sizeof l, "line: %d", line_);
    snprintf(c, sizeof c, "col: %d", col_);
    p[0].x = 1;   p[0].w = 75;  p[0].t = l;
    p[1].x = 76;  p[1].w = 97;  p[1].t = c;
    p[2].x = 173; p[2].w = 94;  p[2].t = "";
    p[3].x = 366; p[3].w = 281; p[3].t = hint_;

    for (int i = 0; i < 4; i++) {
        int px = x() + p[i].x;
        draw_box(FL_DOWN_BOX, px, y() + 6, p[i].w, h() - 7, mue_face());
        mue_ui_font(11);
        fl_color(FL_BLACK);
        int iw = (i == 3) ? 244 : p[i].w - 8;
        fl_draw(p[i].t, px + 4, y() + 6, iw, h() - 7,
                (i == 3 ? FL_ALIGN_CENTER : FL_ALIGN_LEFT) | FL_ALIGN_CLIP);
    }
    /* size grip: three diagonal ridges in the bottom-right corner */
    fl_color(rgb(0xA0A0A0));
    for (int i = 0; i < 3; i++)
        fl_line(x() + w() - 4 - i * 4, y() + h() - 3,
                x() + w() - 3, y() + h() - 4 - i * 4);
}

/* ---- guest screen ----------------------------------------------------- */

static const unsigned char VGA[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},
    {170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},
    {255,85,85},{255,85,255},{255,255,85},{255,255,255},
};

ScreenGrid::ScreenGrid(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H, 0)
{
    box(FL_NO_BOX);
}

void ScreenGrid::draw()
{
    fl_color(FL_BLACK);
    fl_rectf(x(), y(), w(), h());

    unsigned char chars[80 * 25], attrs[80 * 25];
    core_screen_cells(chars, attrs);
    int cw = w() / 80, ch = h() / 25;
    if (cw < 4) cw = 4;
    if (ch < 8) ch = 8;
    int cr = 0, cc = 0;
    core_screen_cursor(&cr, &cc);

    mue_mono_font(ch - 3 > 8 ? ch - 3 : 8);
    for (int r = 0; r < 25; r++) {
        for (int c = 0; c < 80; c++) {
            unsigned char v = chars[r * 80 + c];
            unsigned char a = attrs[r * 80 + c];
            if (v == 0 || v == 32) continue;
            unsigned fg = a & 15, bg = (a >> 4) & 15;
            if (fg == bg) continue;
            fl_color(VGA[fg][0], VGA[fg][1], VGA[fg][2]);
            char s[2] = { (char)v, 0 };
            fl_draw(s, x() + c * cw, y() + r * ch, cw, ch, FL_ALIGN_LEFT);
        }
    }
    (void)cc;
}

/* ---- ALU grid --------------------------------------------------------- */

void AluGrid::draw()
{
    draw_box(FL_NO_BOX, x(), y(), w(), h(), mue_face());
    unsigned short r[8], sr[4], ip, f;
    long ic;
    core_regs(r, sr, &ip, &f, &ic);
    unsigned short vals[3] = { r[0], r[3], (unsigned short)(r[0] + r[3]) };
    int cw = 18, c0 = x() + 14;
    mue_mono_font(13);
    for (int i = 0; i < 16; i++) {
        int bit = 15 - i;
        char s[2] = { (char)('0' + ((bit >> 4) ? 0 : 0)), 0 };
        (void)s;
        char hx[2] = { "0123456789ABCDEF"[bit], 0 };
        fl_color(bit >= 8 ? rgb(0x800000) : FL_BLACK);
        fl_draw(hx, c0 + i * cw, y() + 8, cw, 16, FL_ALIGN_CENTER);
    }
    for (int row = 0; row < 3; row++) {
        int yy = y() + 26 + row * 18 + (row == 2 ? 8 : 0);
        for (int i = 0; i < 16; i++) {
            int bit = 15 - i;
            char s[2] = { (char)('0' + ((vals[row] >> bit) & 1)), 0 };
            fl_color(mue_blue());
            fl_draw(s, c0 + i * cw, yy, cw, 16, FL_ALIGN_CENTER);
        }
    }
}
