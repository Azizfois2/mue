/* mue_icons.c - toolbar icons, drawn in code.
 *
 * Original artwork, no external assets. Each icon is rendered into an
 * offscreen RGB surface and handed to its button as an Fl_RGB_Image, so it
 * scales cleanly and every pixel colour is exact.
 *
 * Style, applied to every icon so the set looks like one set:
 *   24x24 logical canvas, 2px padding
 *   a 1px #202020 outline on every shape
 *   flat fills only, at most one lighter and one darker shade row
 *   one shared palette
 *
 * IMPORTANT: FLTK's color() takes a table INDEX, but fl_color(r,g,b) takes an
 * actual RGB triple. Drawing here uses the latter throughout, so the hex
 * values are used literally. Passing 0xFFFFFF to color() instead is what
 * painted the whole UI cyan earlier - it overruns the 256-entry table.
 */
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Device.H>
#include <math.h>
#include <stdlib.h>

#include "mue_icons.h"

#define IC_YEL   245, 197,  24
#define IC_GOLD  212, 160,  23
#define IC_GREEN  46, 184,  46
#define IC_BLUE   47, 111, 216
#define IC_TEAL   30, 159, 168
#define IC_GRAY  154, 154, 154
#define IC_DARK   32,  32,  32
#define IC_WHITE 255, 255, 255
#define IC_LGRAY 216, 216, 216
#define IC_BG_ON_TOOL 255, 255, 255

/* Every icon starts on white so it reads on the grey toolbar, and ends by
 * grabbing the surface. Pushing/popping per icon keeps them independent. */
#define ICON_BEGIN()                                                   \
    Fl_Image_Surface s(24, 24);                                        \
    Fl_Surface_Device::push_current(&s);                               \
    fl_color(IC_WHITE); fl_rectf(0, 0, 24, 24)

/* Copy the surface into an image that OWNS its pixels. Fl_Image_Surface
 * hands back an image pointing at the surface's own buffer, which is released
 * when the surface goes out of scope; the icons appeared at the wrong place
 * and size until they were copied here. */
#define ICON_END()                                                     \
    do {                                                               \
        Fl_RGB_Image *tmp_ = s.image();                                \
        Fl_RGB_Image *img_ = tmp_ ? (Fl_RGB_Image *)tmp_->copy() : 0;  \
        Fl_Surface_Device::pop_current();                              \
        return img_;                                                   \
    } while (0)

static void outline(int x, int y, int w, int h)
{
    fl_color(IC_DARK);
    fl_rect(x, y, w, h);
}

Fl_RGB_Image *icon_new(void)
{
    ICON_BEGIN();
    fl_color(IC_WHITE); fl_rectf(4, 3, 15, 18);
    fl_color(IC_GRAY);  fl_rectf(15, 3, 4, 5);
    fl_color(IC_DARK);
    for (int i = 0; i < 4; i++) fl_point(15 + i, 3 + i);
    outline(4, 3, 15, 18);
    fl_color(IC_GRAY);
    for (int i = 0; i < 4; i++) fl_rectf(7, 9 + i * 3, 9, 1);
    ICON_END();
}

Fl_RGB_Image *icon_open(void)
{
    ICON_BEGIN();
    fl_color(IC_YEL);  fl_polygon(2, 8, 9, 8, 11, 5, 4, 5);
    fl_color(IC_YEL);  fl_polygon(5, 11, 22, 11, 19, 19, 2, 19);
    fl_color(IC_GOLD); fl_rectf(5, 17, 14, 2);
    outline(3, 7, 17, 12);
    ICON_END();
}

Fl_RGB_Image *icon_star(void)
{
    ICON_BEGIN();
    /* A five-pointed star: outer and inner radii alternating every 36deg.
     * Built with fl_vertex because fl_polygon() takes variadic coordinates,
     * not an array (and a 10-point star needs more args than it accepts). */
    for (int pass = 0; pass < 2; pass++) {
        if (pass) fl_color(IC_YEL); else fl_color(IC_GOLD);
        fl_begin_polygon();
        for (int i = 0; i < 10; i++) {
            double a = -1.5707963 + i * 0.6283185;
            double rr = (i & 1) ? (pass ? 2.8 : 3.8) : (pass ? 7.0 : 9.0);
            fl_vertex(12 + rr * cos(a), 12 + rr * sin(a));
        }
        fl_end_polygon();
    }
    ICON_END();
}

Fl_RGB_Image *icon_save(void)
{
    ICON_BEGIN();
    fl_color(IC_DARK);  fl_rectf(3, 3, 18, 18);
    fl_color(IC_LGRAY); fl_rectf(6, 4, 8, 6);
    fl_color(IC_WHITE); fl_rectf(5, 13, 14, 8);
    fl_color(IC_GRAY);
    for (int i = 0; i < 3; i++) fl_rectf(7, 15 + i * 2, 10, 1);
    outline(3, 3, 18, 18);
    ICON_END();
}

Fl_RGB_Image *icon_compile(void)
{
    ICON_BEGIN();
    /* A blue grid with dotted pins along the TOP edge only.  The about icon
     * is a solid chip with pins on all four sides - the two were too similar
     * when both were chips with pins. */
    fl_color(IC_GRAY); fl_rectf(4, 8, 16, 12);
    fl_color(IC_BLUE); fl_rectf(6, 10, 12, 8);
    fl_color(IC_LGRAY);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 5; c++)
            fl_rectf(7 + c * 2, 11 + r * 2, 1, 1);
    outline(4, 8, 16, 12);
    fl_color(IC_DARK);
    for (int i = 0; i < 8; i++) fl_rectf(5 + i * 2, 4, 1, 4);
    ICON_END();
}

Fl_RGB_Image *icon_run(void)
{
    ICON_BEGIN();
    fl_color(IC_GREEN); fl_polygon(7, 4, 20, 12, 7, 20);
    fl_color(IC_DARK);  fl_rectf(6, 4, 1, 17);
    ICON_END();
}

Fl_RGB_Image *icon_calc(void)
{
    ICON_BEGIN();
    fl_color(IC_DARK);  fl_rectf(4, 3, 16, 18);
    fl_color(IC_TEAL);  fl_rectf(6, 5, 12, 5);
    fl_color(IC_LGRAY);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            fl_rectf(6 + c * 4, 12 + r * 4, 3, 3);
    outline(4, 3, 16, 18);
    ICON_END();
}

Fl_RGB_Image *icon_convert(void)
{
    ICON_BEGIN();
    fl_color(IC_BLUE);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            fl_rectf(5 + c * 5, 5 + r * 5, 4, 4);
    fl_color(IC_DARK);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            fl_rect(5 + c * 5, 5 + r * 5, 4, 4);
    ICON_END();
}

Fl_RGB_Image *icon_options(void)
{
    ICON_BEGIN();
    /* Hammer head across the top, wrench handle below it: two distinct
     * shapes rather than a bare X, which read as a cancel symbol. */
    fl_color(IC_GRAY); fl_rectf(4, 6, 16, 5);          /* hammer head */
    outline(4, 6, 16, 5);
    fl_color(IC_LGRAY); fl_rectf(11, 11, 3, 10);       /* handle      */
    outline(11, 11, 3, 10);
    fl_color(IC_GRAY); fl_rectf(5, 13, 4, 6);          /* wrench jaw  */
    outline(5, 13, 4, 6);
    fl_color(IC_BG_ON_TOOL); fl_rectf(6, 14, 2, 2);
    ICON_END();
}

Fl_RGB_Image *icon_help(void)
{
    ICON_BEGIN();
    fl_color(IC_YEL);  fl_pie(3, 2, 18, 18, 0, 360);
    fl_color(IC_GOLD); fl_arc(3, 2, 18, 18, 200, 340);
    fl_color(IC_DARK);
    fl_line_style(FL_SOLID, 2);
    fl_arc(8, 6, 8, 8, 200, 20);
    fl_line(12, 13, 12, 15);
    fl_rectf(11, 16, 2, 2);
    fl_line_style(0);
    ICON_END();
}

Fl_RGB_Image *icon_about(void)
{
    ICON_BEGIN();
    fl_color(IC_GREEN); fl_rectf(6, 6, 12, 12);
    outline(6, 6, 12, 12);
    fl_color(IC_DARK);
    for (int i = 0; i < 3; i++) {
        fl_rectf(8 + i * 4, 2, 1, 4);
        fl_rectf(8 + i * 4, 18, 1, 4);
        fl_rectf(2, 8 + i * 4, 4, 1);
        fl_rectf(18, 8 + i * 4, 4, 1);
    }
    ICON_END();
}

Fl_RGB_Image *icon_reload(void)
{
    ICON_BEGIN();
    fl_color(IC_BLUE);
    fl_line_style(FL_SOLID, 3);
    fl_arc(4, 4, 16, 16, 40, 320);
    fl_line_style(0);
    fl_polygon(17, 3, 22, 8, 15, 9);
    ICON_END();
}

Fl_RGB_Image *icon_step_back(void)
{
    ICON_BEGIN();
    fl_color(IC_TEAL);
    fl_polygon(17, 4, 6, 12, 17, 20);
    fl_rectf(4, 4, 2, 16);
    ICON_END();
}

Fl_RGB_Image *icon_step(void)
{
    ICON_BEGIN();
    fl_color(IC_TEAL);
    fl_polygon(7, 4, 18, 12, 7, 20);
    fl_rectf(18, 4, 2, 16);
    ICON_END();
}

Fl_RGB_Image *icon_ffwd(void)
{
    ICON_BEGIN();
    fl_color(IC_BLUE); fl_polygon(3, 4, 13, 12, 3, 20);
    fl_color(IC_TEAL); fl_polygon(12, 4, 22, 12, 12, 20);
    ICON_END();
}

/* A greyed copy for disabled buttons. FLTK's own inactive rendering is not
 * enough: `step back` must look visibly dead before anything has executed. */
Fl_RGB_Image *icon_grayed(Fl_RGB_Image *src)
{
    if (!src) return 0;
    int w = src->w(), h = src->h(), d = src->d();
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * d);
    if (!buf) return 0;
    const unsigned char *in = (const unsigned char *)src->data()[0];
    for (int i = 0; i < w * h; i++) {
        unsigned v = (in[i * d] * 30u + in[i * d + 1] * 59u + in[i * d + 2] * 11u) / 100u;
        v = 156 + v * 99 / 255;
        buf[i * d] = buf[i * d + 1] = buf[i * d + 2] = (unsigned char)v;
    }
    Fl_RGB_Image *g = new Fl_RGB_Image(buf, w, h, d);
    g->alloc_array = 1;
    return g;
}

Fl_Button *make_tool(int x, int y, int w, int h, const char *label,
                     Fl_Image *icon)
{
    Fl_Button *b = new Fl_Button(x, y, w, h, label);
    b->box(FL_THIN_UP_BOX);
    b->labelsize(10);
    b->labelfont(FL_HELVETICA);
    if (icon) {
        /* Uniform padding: the image sits a fixed 4px from the top and the
         * label is pinned to the bottom, so every icon shares one baseline.
         * Letting FLTK centre image+label together made "Load" and "reload"
         * sit visibly lower than their neighbours. */
        b->image(icon);
        b->align(FL_ALIGN_IMAGE_OVER_TEXT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    } else {
        b->align(FL_ALIGN_CENTER);
    }
    return b;
}
