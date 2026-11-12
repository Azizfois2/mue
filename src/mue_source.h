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

    void relayout();
    int  line_count() const;
    int  line_start(int ln) const;
    int  line_len(int ln) const;
    int  line_of(int pos) const;
    int  col_of(int pos) const;
    int  pos_of(int ln, int col) const;
    int  cols_visible() const;
    int  rows_visible() const;
    void clamp_scroll();
    void ensure_visible();
    void insert(const char *s, int n);
    void erase_sel();
    int  sel_start() const { return cur_ < anchor_ ? cur_ : anchor_; }
    int  sel_end()   const { return cur_ < anchor_ ? anchor_ : cur_; }
    void draw_line(int ln, int y);
};

#endif
