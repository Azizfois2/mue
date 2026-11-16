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
#define IC_BRIGHT  0, 208,  0
#define IC_STEPGRAY 172, 172, 172
#define IC_GRAY  154, 154, 154
#define IC_DARK   32,  32,  32
#define IC_WHITE 255, 255, 255
#define IC_LGRAY 216, 216, 216
#define IC_BG_ON_TOOL 255, 255, 255

/* Every icon starts on the toolbar face so it reads as a flat glyph on the
 * grey toolbar, and ends by grabbing the surface. Pushing/popping per icon
 * keeps them independent. */
#define ICON_BEGIN()                                                   \
    Fl_Image_Surface s(24, 24);                                        \
    Fl_Surface_Device::push_current(&s);                               \
    fl_color(240, 240, 240); fl_rectf(0, 0, 24, 24)

/* Copy the surface into an image that OWNS its pixels, then materialise it at
 * the 16px toolbar size.  Fl_Image_Surface hands back an image pointing at
 * the surface's own buffer, which is released when the surface goes out of
 * scope; and drawing a 24x24 surface with draw(x,y,16,16) CROPS it to the
 * top-left 16x16 instead of scaling it, which is what clipped the icons. */
#define ICON_END()                                                     \
    do {                                                               \
        Fl_RGB_Image *tmp_ = s.image();                                \
        Fl_RGB_Image *full_ = tmp_ ? (Fl_RGB_Image *)tmp_->copy() : 0; \
        Fl_Surface_Device::pop_current();                              \
        if (!full_) return 0;                                          \
        Fl_RGB_Image *img_ = (Fl_RGB_Image *)full_->copy(16, 16);      \
        delete full_;                                                  \
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

static Fl_RGB_Image *pix_art(const char *const *rows);

Fl_RGB_Image *icon_open(void)
{
    /* The original's "Load"/"open" glyph: a dithered yellow folder with a
     * dark olive body and a black outline, and the little white-and-black
     * arrow that leaves its top-right corner.  Transcribed 1:1 from the
     * 16x16 art in the reference capture. */
    static const char *const art[16] = {
        "................",
        "................",
        ".........TTT....",
        "........TPPPT.T.",
        "........P...PTK.",
        ".TTT........TKK.",
        "TIRITTTTTTT.PPP.",
        "KCACIRIRIRK.....",
        "KACACACACAK.....",
        "KCACAFUFUFKTTTTT",
        "KACAFEEEEEEEEEKP",
        "KCAFEDDDDDDDDHP.",
        "KAFEDDDDDDDDHP..",
        "KFEDDDDDDDDHP...",
        "KKHHHHHHHHHP....",
        "PPPPPPPPPPP.....",
    };
    return pix_art(art);
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
    /* Blue body, pale screen, six keys - the original's calculator. */
    fl_color(IC_BLUE);  fl_rectf(4, 3, 16, 18);
    fl_color(198, 232, 200);
    fl_rectf(6, 5, 12, 5);
    fl_color(IC_WHITE);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            fl_rectf(6 + c * 4, 12 + r * 3, 3, 2);
    fl_color(IC_DARK);
    fl_rect(4, 3, 16, 18);
    fl_rect(6, 5, 12, 5);
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

/* ---- pixel-art icons ---------------------------------------------------
 * The options/help/about glyphs are literal 16x16 bitmaps.  Drawing them on
 * the shared 24x24 surface and shrinking to 16 with copy(16,16) turned their
 * 1px outlines into grey smudges and, with the old font-drawn "?"/"M", put
 * parts of a glyph outside the surface.  The originals are crisp 16px art, so
 * these are authored at exactly that size and handed over untouched. */
static Fl_RGB_Image *pix_art_n(const char *const *rows, int scale)
{
    int w = 16 * scale;
    unsigned char *buf = new unsigned char[w * w * 3];
    if (!buf) return 0;
    for (int y = 0; y < w; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = buf + (y * w + x) * 3;
            int r = 240, g = 240, b = 240;           /* toolbar face */
            switch (rows[y / scale][x / scale]) {
            case '#': r =  32; g =  32; b =  32; break;   /* outline      */
            case 'Y': r = 245; g = 197; b =  24; break;   /* yellow       */
            case 'O': r = 212; g = 160; b =  23; break;   /* yellow shade */
            case 'G': r =  46; g = 184; b =  46; break;   /* chip green   */
            case 'w': r = 154; g = 154; b = 154; break;   /* wrench grey  */
            case 'r': r = 150; g =  30; b =  30; break;   /* hammer red   */
            case 'o': r = 140; g = 140; b =  40; break;   /* hammer olive */
            /* The emulator's five action icons are transcribed pixel for
             * pixel from the original's 16x16 art, so its own 8-bit palette
             * has to be reproduced exactly rather than approximated. */
            case 'K': r =   0; g =   0; b =   0; break;   /* black        */
            case 'A': r = 255; g = 255; b =  51; break;   /* folder light */
            case 'C': r = 255; g = 255; b = 204; break;   /* folder pale  */
            case 'D': r = 128; g = 128; b =   0; break;   /* folder body  */
            case 'E': r = 102; g = 102; b =   0; break;   /* folder edge  */
            case 'F': r =  51; g =  51; b =   0; break;
            case 'H': r =  25; g =  25; b =   0; break;
            case 'I': r = 204; g = 204; b =   0; break;
            case 'B': r =   0; g =   0; b = 255; break;   /* reload blue  */
            case 'J': r =  48; g =  48; b = 252; break;
            case 'L': r = 192; g = 192; b = 243; break;
            case 'M': r = 160; g = 160; b = 160; break;   /* disabled grey*/
            case 'N': r = 176; g = 176; b = 176; break;
            case 'P': r = 192; g = 192; b = 192; break;
            case 'Q': r = 224; g = 224; b = 224; break;
            case 'R': r = 204; g = 204; b = 204; break;
            case 'S': r = 179; g = 179; b = 179; break;
            case 'T': r =  48; g =  48; b =  48; break;
            case 'U': r =  51; g =  51; b =  51; break;
            case 'V': r =   0; g = 128; b = 128; break;   /* teal         */
            case 'W': r =  48; g = 150; b = 150; break;
            case 'X': r = 192; g = 218; b = 218; break;
            case 'Z': r =   0; g = 255; b =   0; break;   /* bright green */
            case 'z': r =   0; g = 230; b =  25; break;
            case 'y': r =   0; g = 153; b = 102; break;
            }
            p[0] = (unsigned char)r;
            p[1] = (unsigned char)g;
            p[2] = (unsigned char)b;
        }
    }
    Fl_RGB_Image *img = new Fl_RGB_Image(buf, w, w, 3);
    if (img->array == (const uchar *)buf) img->alloc_array = 1;
    else delete[] buf;
    return img;
}

static Fl_RGB_Image *pix_art(const char *const *rows)
{
    return pix_art_n(rows, 1);
}

Fl_RGB_Image *icon_options(void)
{
    /* A grey open-end wrench and a hammer with a dark red handle and an
     * olive head, crossing in an X. */
    static const char *const art[16] = {
        "...........###..",
        "..........#ooo##",
        ".####.....#oooo#",
        "#wwww#....#ooo##",
        "#w##w#...#ooo##.",
        "#w#.#w#..#oo##..",
        ".#w#ww#.#oo##...",
        "..#www#ww#......",
        "....#rrw#.......",
        "...#rr#w#.......",
        "..#rr#.w#.......",
        ".#rr#..#w#......",
        ".#r#....#w#.....",
        "#rr#.....#w#....",
        "#r#.......#w#...",
        ".#........###...",
    };
    return pix_art(art);
}

Fl_RGB_Image *icon_help(void)
{
    /* A yellow question mark with a dark outline, as the original draws it:
     * a thick hook, a narrow stem and a detached dot. */
    static const char *const art[16] = {
        "................",
        ".....######.....",
        "....#YYYYYY#....",
        "...#YY####YO#...",
        "...#Y##..##YO#..",
        "...#Y#....#YO#..",
        "....##....#YO#..",
        ".........#YYO#..",
        "........#YYO#...",
        ".......#YYO#....",
        ".......#YY#.....",
        ".......####.....",
        "................",
        ".......#YY#.....",
        ".......#YY#.....",
        ".......####.....",
    };
    return pix_art(art);
}

/* Green chip with pins: emu8086's logo chip.  The die carries an "M" for mue
 * instead of the original's "E".  This is also the program's icon, so
 * re/make_icon.py reads this very table to build src/mue.ico. */
static const char *const about_art[16] = {
    "................",
    "...#..#..#..#...",
    ".##############.",
    ".#GGGGGGGGGGGG#.",
    ".#GG#GGGGG#GGG#.",
    ".#GG##GGG##GGG#.",
    ".#GG#G#G#G#GGG#.",
    ".#GG#GG#GG#GGG#.",
    ".#GG#GGGGG#GGG#.",
    ".#GG#GGGGG#GGG#.",
    ".#GG#GGGGG#GGG#.",
    ".#GGGGGGGGGGGG#.",
    ".##############.",
    "...#..#..#..#...",
    "................",
    "................",
};

Fl_RGB_Image *icon_about(void)
{
    return pix_art(about_art);
}

/* The window/exe icon: the same chip at 32x32, which is the large-icon size
 * Windows asks for. */
Fl_RGB_Image *icon_app(void)
{
    return pix_art_n(about_art, 2);
}

Fl_RGB_Image *icon_reload(void)
{
    /* Blue rounded loop, open at the top-left, with the arrow head turning
     * in at that corner. */
    static const char *const art[16] = {
        ".......J........",
        "......JB........",
        ".....JBBJJJJ....",
        "....JBBBBBBBJ...",
        "....LBBBBBBBBJ..",
        "JJJ..LBBLLLBBB..",
        "BBB...LB...BBB..",
        "BBB....L...BBB..",
        "BBB........BBB..",
        "BBB........BBB..",
        "BBB........BBB..",
        "BBB........BBB..",
        "BBB........BBB..",
        "BBBJJJJJJJJBBB..",
        "LBBBBBBBBBBBBL..",
        ".LBBBBBBBBBBL...",
    };
    return pix_art(art);
}

Fl_RGB_Image *icon_step_back(void)
{
    /* A hollow grey triangle pointing left with the stop bar after it: the
     * original draws step-back in the disabled grey, because the button only
     * becomes live after an instruction has executed. */
    static const char *const art[16] = {
        "................",
        ".......NNN..NN..",
        "......NMMM..MM..",
        ".....NMMMM..MM..",
        "....NMM.MM..MM..",
        "...NMM..MM..MM..",
        "..NMM...MM..MM..",
        ".NMM....MM..MM..",
        ".QMMS...MM..MM..",
        "..QMMN..MM..MM..",
        "...QMMN.MM..MM..",
        "....QMMNMM..MM..",
        ".....QMMMM..MM..",
        "......QMMM..MM..",
        ".......Q....Q...",
        "................",
    };
    return pix_art(art);
}

Fl_RGB_Image *icon_step(void)
{
    /* Teal bar, then a bright green right triangle outlined in teal. */
    static const char *const art[16] = {
        "................",
        "..WW..WWW.......",
        "..VV..VVVW......",
        "..VV..VVVVW.....",
        "..VV..VVzVVW....",
        "..VV..VVZzVVW...",
        "..VV..VVZZzVVW..",
        "..VV..VVZZZzVVW.",
        "..VV..VVZZZyVVX.",
        "..VV..VVZZyVVX..",
        "..VV..VVZyVVX...",
        "..VV..VVyVVX....",
        "..VV..VVVVX.....",
        "..VV..VVVX......",
        "..XX..XXX.......",
        "................",
    };
    return pix_art(art);
}

Fl_RGB_Image *icon_ffwd(void)
{
    /* run: two bright green right triangles, each outlined in teal. */
    static const char *const art[16] = {
        "................",
        ".WWW..WWW.......",
        ".VVVW.XVVW......",
        ".VVVVW.XVVW.....",
        ".VVzVVW.XVVW....",
        ".VVZzVVW.XVVW...",
        ".VVZZzVVW.XVVW..",
        ".VVZZZzVVW.XVVW.",
        ".VVZZZyVVX.WVVX.",
        ".VVZZyVVX.WVVX..",
        ".VVZyVVX.WVVX...",
        ".VVyVVX.WVVX....",
        ".VVVVX.WVVX.....",
        ".VVVX.WVVX......",
        ".XXX..XXX.......",
        "................",
    };
    return pix_art(art);
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
