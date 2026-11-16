/* mue_source.h - a small text editor that draws its own coloured text.
 *
 * Why this exists: FLTK's Fl_Text_Editor offers per-character styling through
 * highlight_data(), but in the 1.4.4 build here the style table is accepted
 * and then never consulted when drawing. Verified three ways: the style
 * buffer matches the text byte for byte, position_style() returns the style
 * character that was registered, and the table's colour resolves to the
 * expected RGB - yet setting a style to bright magenta on cyan produced zero
 * magenta pixels, while plain textcolor() on the same widget drew 448. So the
 * toolkit can colour text; only the style table is ignored.
 *
 * This widget therefore tokenises each visible line and draws each run with
 * fl_draw() under its own fl_color(). It owns its text as a simple growable
 * buffer, which keeps the editing model small: insert, delete, cursor motion,
 * and selection.
 *
 * Two emu8086 shapes are served by the same widget:
 *
 *   editor     gutter on, digits dark grey with the current line in red,
 *              no highlight band, editable.
 *   source     gutter on, a yellow (#FFFF00) band behind the current
 *              instruction, read only.
 */
#ifndef MUE_SOURCE_H
#define MUE_SOURCE_H

#include <FL/Fl_Widget.H>
#include <string>

class SourceView : public Fl_Widget {
public:
    SourceView(int X, int Y, int W, int H, const char *L = 0);

    /* Whole-buffer access. */
    void set_text(const char *t);
    const std::string &text() const { return buf_; }

    int font_size() const { return fs_; }
    void font_size(int s) { fs_ = s; relayout(); }

    /* emu8086 draws a line-number column; the source-code window draws one on
     * a grey panel, and the editor draws one too. */
    void gutter(int on) { gutter_ = on ? 1 : 0; relayout(); }
    int  gutter() const { return gutter_; }

    /* The original marks the line that is about to execute with a bright
     * yellow band in the "original source code" window. Line numbers are
     * 0-based; -1 means no band. */
    void highlight_line(int ln) { hl_ = ln; redraw(); }
    int  highlight_line() const { return hl_; }

    /* The source window is a viewer, not an editor. */
    void read_only(int ro) { ro_ = ro ? 1 : 0; }
    int  read_only() const { return ro_; }

    /* Which line the caret is on, 0-based (for the status bar). */
    int  caret_line() const { return line_of(cur_); }
    int  caret_col()  const { return col_of(cur_); }

    int handle(int ev) override;
    void draw() override;
    void resize(int X, int Y, int W, int H) override;

private:
    std::string buf_;
    int cur_;               /* byte offset of the caret  */
    int anchor_;            /* selection anchor          */
    int scroll_line_;       /* first visible line        */
    int scroll_x_;
    int fs_;                /* font size                 */
    int lh_;                /* line height               */
    int cw_;                /* character width           */
    int gutter_;            /* draw the line-number column */
    int gw_;                /* computed gutter width      */
    int hl_;                /* highlighted line, -1 = none */
    int ro_;                /* read only                  */

    void relayout();
    int  line_count() const;
    int  line_start(int ln) const;
    int  line_len(int ln) const;
    int  line_of(int pos) const;
    int  col_of(int pos) const;
    int  pos_of(int ln, int col) const;
    int  cols_visible() const;
    int  rows_visible() const;
    int  text_x() const;          /* left edge of the text area  */
    int  gutter_w() const { return gutter_ ? gw_ : 0; }
    void clamp_scroll();
    void ensure_visible();
    void insert(const char *s, int n);
    void erase_sel();
    int  sel_start() const { return cur_ < anchor_ ? cur_ : anchor_; }
    int  sel_end()   const { return cur_ < anchor_ ? anchor_ : cur_; }
    void draw_line(int ln, int y);
    void draw_gutter();
};

#endif
