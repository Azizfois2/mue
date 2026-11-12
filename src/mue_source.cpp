/* mue_source.cpp - implementation of the self-drawing source editor. */
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Enumerations.H>
#include <string.h>
#include <stdlib.h>

#include "mue_source.h"
#include "mue_highlight.h"

SourceView::SourceView(int X, int Y, int W, int H, const char *L)
    : Fl_Widget(X, Y, W, H, L)
{
    box(FL_DOWN_FRAME);
    color(FL_WHITE);
    cur_ = anchor_ = 0;
    scroll_line_ = scroll_x_ = 0;
    fs_ = 14;
    relayout();
}

void SourceView::relayout()
{
    fl_font(FL_COURIER_BOLD, fs_);
    cw_ = (int)(fl_width("M") + 0.5f);
    if (cw_ < 1) cw_ = 8;
    lh_ = fs_ + 4;
    clamp_scroll();
}

void SourceView::set_text(const char *t)
{
    buf_ = t ? t : "";
    cur_ = anchor_ = 0;
    scroll_line_ = scroll_x_ = 0;
    redraw();
}

/* ---- geometry helpers ------------------------------------------------- */

int SourceView::line_count() const
{
    int n = 1;
    for (size_t i = 0; i < buf_.size(); i++)
        if (buf_[i] == '\n') n++;
    return n;
}

int SourceView::line_start(int ln) const
{
    int n = 0;
    for (size_t i = 0; i < buf_.size(); i++) {
        if (n == ln) return (int)i;
        if (buf_[i] == '\n') n++;
    }
    return (int)buf_.size();
}

int SourceView::line_len(int ln) const
{
    int s = line_start(ln);
    int e = s;
    while (e < (int)buf_.size() && buf_[e] != '\n') e++;
    return e - s;
}

int SourceView::line_of(int pos) const
{
    int n = 0;
    for (int i = 0; i < pos && i < (int)buf_.size(); i++)
        if (buf_[i] == '\n') n++;
    return n;
}

int SourceView::col_of(int pos) const
{
    int s = line_start(line_of(pos));
    return pos - s;
}

int SourceView::pos_of(int ln, int col) const
{
    int s = line_start(ln);
    if (col > line_len(ln)) col = line_len(ln);
    return s + col;
}

int SourceView::cols_visible() const
{
    int n = (w() - 6) / cw_;
    return n > 0 ? n : 1;
}

int SourceView::rows_visible() const
{
    int n = (h() - 4) / lh_;
    return n > 0 ? n : 1;
}

void SourceView::clamp_scroll()
{
    int maxl = line_count() - rows_visible();
    if (scroll_line_ > maxl) scroll_line_ = maxl;
    if (scroll_line_ < 0) scroll_line_ = 0;
    if (scroll_x_ < 0) scroll_x_ = 0;
}

void SourceView::ensure_visible()
{
    int ln = line_of(cur_);
    int col = col_of(cur_);
    if (ln < scroll_line_) scroll_line_ = ln;
    if (ln >= scroll_line_ + rows_visible())
        scroll_line_ = ln - rows_visible() + 1;
    if (col < scroll_x_) scroll_x_ = col;
    if (col >= scroll_x_ + cols_visible())
        scroll_x_ = col - cols_visible() + 1;
    clamp_scroll();
}

/* ---- editing ---------------------------------------------------------- */

void SourceView::insert(const char *s, int n)
{
    erase_sel();
    buf_.insert((size_t)cur_, s, (size_t)n);
    cur_ += n;
    anchor_ = cur_;
    redraw();
}

void SourceView::erase_sel()
{
    int a = sel_start(), b = sel_end();
    if (a == b) return;
    buf_.erase((size_t)a, (size_t)(b - a));
    cur_ = anchor_ = a;
}

/* ---- drawing ---------------------------------------------------------- */

void SourceView::draw_line(int ln, int y)
{
    int s = line_start(ln);
    int len = line_len(ln);
    if (len <= 0) return;

    /* Tokenise this one line into a reusable class array. */
    static char *cls = 0;
    static int cap = 0;
    if (len + 2 > cap) { cap = len + 64; cls = (char *)realloc(cls, cap); }
    mue_highlight_line(buf_.data() + s, cls, len);

    /* One character at a time, advancing the pen by the glyph's real
     * advance width. Two things went wrong when this drew whole runs:
     * fl_draw() with a width argument clips the text to that width rather
     * than measuring it, and the run's x was computed both inside and
     * outside the call, so every run after the first landed in the wrong
     * place and the line came out as overlapping mojibake. Advancing per
     * glyph also keeps positions correct for any font, proportional or not.
     */
    fl_font(FL_COURIER_BOLD, fs_);
    int pen = x() + 3 - scroll_x_ * cw_;
    int base = y + lh_ - 4;
    char one[2];
    one[1] = 0;
    for (int i = 0; i < len; i++) {
        unsigned char r, g, b;
        mue_token_rgb(cls[i], &r, &g, &b);
        fl_color(r, g, b);
        one[0] = buf_[(size_t)(s + i)];
        if (one[0] == '\t') { pen += cw_ * 4; continue; }
        fl_draw(one, pen, base);
        pen += cw_;
    }
}

void SourceView::draw()
{
    draw_box();
    fl_push_clip(x() + 2, y() + 2, w() - 4, h() - 4);

    /* Selection highlight behind the text. */
    int a = sel_start(), b = sel_end();
    if (a != b) {
        fl_color(FL_SELECTION_COLOR);
        for (int ln = line_of(a); ln <= line_of(b) && ln < line_count(); ln++) {
            int ls = line_start(ln), ll = line_len(ln);
            int from = (ln == line_of(a)) ? a - ls : 0;
            int to   = (ln == line_of(b)) ? b - ls : ll;
            if (to < from) continue;
            int vy = y() + 2 + (ln - scroll_line_) * lh_;
            if (vy + lh_ < y() || vy > y() + h()) continue;
            fl_rectf(x() + 2 + (from - scroll_x_) * cw_, vy,
                     (to - from) * cw_, lh_);
        }
    }

    fl_font(FL_COURIER_BOLD, fs_);
    int n = line_count();
    for (int ln = scroll_line_; ln < n && ln < scroll_line_ + rows_visible();
         ln++) {
        int vy = y() + 2 + (ln - scroll_line_) * lh_;
        if (vy > y() + h() - 2) break;
        draw_line(ln, vy);
    }

    /* Caret. */
    if (Fl::focus() == this) {
        int ln = line_of(cur_), col = col_of(cur_);
        int vy = y() + 2 + (ln - scroll_line_) * lh_;
        int vx = x() + 2 + (col - scroll_x_) * cw_;
        if (vy >= y() && vy < y() + h() && vx >= x() && vx <= x() + w()) {
            fl_color(FL_BLACK);
            fl_line(vx, vy, vx, vy + lh_ - 2);
        }
    }
    fl_pop_clip();
}

void SourceView::resize(int X, int Y, int W, int H)
{
    Fl_Widget::resize(X, Y, W, H);
    clamp_scroll();
}

/* ---- events ----------------------------------------------------------- */

int SourceView::handle(int ev)
{
    switch (ev) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        redraw();
        return 1;

    case FL_PUSH: {
        Fl::focus(this);
        int ln = scroll_line_ + (Fl::event_y() - y() - 2) / lh_;
        if (ln < 0) ln = 0;
        if (ln >= line_count()) ln = line_count() - 1;
        int col = scroll_x_ + (Fl::event_x() - x() - 2) / cw_;
        if (col < 0) col = 0;
        cur_ = anchor_ = pos_of(ln, col);
        if (Fl::event_clicks()) {
            /* double click selects the word */
            int s = line_start(ln), e = s + line_len(ln);
            int p = cur_;
            while (p > s && (isalnum((unsigned char)buf_[p - 1]) ||
                             buf_[p - 1] == '_')) p--;
            int q = cur_;
            while (q < e && (isalnum((unsigned char)buf_[q]) ||
                             buf_[q] == '_')) q++;
            anchor_ = p; cur_ = q;
        }
        redraw();
        return 1;
    }

    case FL_DRAG: {
        int ln = scroll_line_ + (Fl::event_y() - y() - 2) / lh_;
        if (ln < 0) ln = 0;
        if (ln >= line_count()) ln = line_count() - 1;
        int col = scroll_x_ + (Fl::event_x() - x() - 2) / cw_;
        if (col < 0) col = 0;
        cur_ = pos_of(ln, col);
        redraw();
        return 1;
    }

    case FL_KEYBOARD: {
        int k = Fl::event_key();
        int shift = Fl::event_state(FL_SHIFT);
        switch (k) {
        case FL_Left:
            if (shift) { if (cur_ > 0) cur_--; }
            else if (sel_start() != sel_end()) cur_ = anchor_ = sel_start();
            else if (cur_ > 0) cur_ = anchor_ = cur_ - 1;
            break;
        case FL_Right:
            if (shift) { if (cur_ < (int)buf_.size()) cur_++; }
            else if (sel_start() != sel_end()) cur_ = anchor_ = sel_end();
            else if (cur_ < (int)buf_.size()) cur_ = anchor_ = cur_ + 1;
            break;
        case FL_Up: {
            int ln = line_of(cur_), col = col_of(cur_);
            if (ln > 0) cur_ = pos_of(ln - 1, col);
            if (!shift) anchor_ = cur_;
            break;
        }
        case FL_Down: {
            int ln = line_of(cur_), col = col_of(cur_);
            if (ln < line_count() - 1) cur_ = pos_of(ln + 1, col);
            if (!shift) anchor_ = cur_;
            break;
        }
        case FL_Home:
            cur_ = line_start(line_of(cur_));
            if (!shift) anchor_ = cur_;
            break;
        case FL_End:
            cur_ = line_start(line_of(cur_)) + line_len(line_of(cur_));
            if (!shift) anchor_ = cur_;
            break;
        case FL_BackSpace:
            if (sel_start() != sel_end()) erase_sel();
            else if (cur_ > 0) {
                buf_.erase((size_t)(cur_ - 1), 1);
                cur_--; anchor_ = cur_;
            }
            break;
        case FL_Delete:
            if (sel_start() != sel_end()) erase_sel();
            else if (cur_ < (int)buf_.size()) buf_.erase((size_t)cur_, 1);
            break;
        case FL_Enter:
        case FL_KP_Enter:
            insert("\n", 1);
            break;
        case FL_Tab:
            insert("    ", 4);
            break;
        default: {
            /* printable characters, and Ctrl-A/ctrl-C style is left to FLTK */
            const char *t = Fl::event_text();
            int n = Fl::event_length();
            if (n > 0 && t) insert(t, n);
            break;
        }
        }
        ensure_visible();
        redraw();
        return 1;
    }

    case FL_MOUSEWHEEL:
        scroll_line_ -= Fl::event_dy() * 3;
        clamp_scroll();
        redraw();
        return 1;
    }
    return Fl_Widget::handle(ev);
}
