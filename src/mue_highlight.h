/* mue_highlight.h - 8086 source syntax highlighting (see mue_highlight.c). */
#ifndef MUE_HIGHLIGHT_H
#define MUE_HIGHLIGHT_H

#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>

extern Fl_Text_Display::Style_Table_Entry mue_style_table[256];

#define mue_style_table_ptr mue_style_table

/* Rebuild the whole style buffer from the text buffer. */
void mue_rehighlight(Fl_Text_Buffer *buf, Fl_Text_Buffer *style);

/* Style one line; returns a malloc'd string the caller frees. */
char *mue_highlight_one(const char *line, int len);

#endif

/* Token classes, as the characters highlight_line() writes. */
enum {
    MUE_TKN_PLAIN = 'A',
    MUE_TKN_MNEM  = 'a',
    MUE_TKN_REG   = 'b',
    MUE_TKN_NUM   = 'c',
    MUE_TKN_CMT   = 'd',
    MUE_TKN_STR   = 'e'
};

/* Style one line in place: fills cls[] (len bytes) with token classes. */
void mue_highlight_line(const char *src, char *cls, int len);

/* The RGB for a token class. */
void mue_token_rgb(int cls, unsigned char *r, unsigned char *g, unsigned char *b);
