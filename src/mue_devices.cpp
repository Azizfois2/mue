/* mue_devices.cpp - the virtual devices from emu8086's DEVICES\ folder.
 *
 * The original devices are separate VB6 programs that exchange bytes through
 * C:\emu8086.io.  Every one of them has the same shape: one form, a handful
 * of paints, and a timer that reads the port file.  Here the form is redrawn
 * by FLTK and the port traffic arrives through devices_io(), which io.c calls
 * on every IN/OUT.  The port contracts are taken from the shipped examples
 * and the device documentation:
 *
 *   LED_Display     199 (word)      decimal on a 5-digit seven-segment display
 *   Traffic_Lights  4   (word)      12 lamps, four groups of red/yellow/green
 *   Stepper_motor   7   (byte)      bit7 ready, low nibble coil phase
 *   Thermometer     125 in / 127 out air temperature / heater on-off
 *   Robot           9 out, 10 in, 11 in
 *   Printer         130 (byte)      one character per write, port cleared
 *   Simple          110 byte, 112 word, both directions
 *   VGA_STATE       no ports: the emulator's VGA mode indicator
 *
 * Sizes are the measured client rectangles from live captures (re/DEVICES.md).
 */
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <vector>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

#include "mue_devices.h"
#include "mue_paths.h"
#include "mue_widgets.h"
#include "ioport.h"

/* ---------------------------------------------------------------------- */
/* shared state                                                            */

static int dev_open[DEV_COUNT];
static Fl_Double_Window *dev_win[DEV_COUNT];
static Fl_Widget *dev_canvas[DEV_COUNT];

static unsigned led_value;                 /* port 199 word            */
static unsigned traffic_value;             /* port 4 word              */
static unsigned char stepper_phase;        /* port 7 low nibble        */
static int thermo_temp, thermo_heater;     /* ports 125 / 127          */
static unsigned char simple_byte;        /* last OUT 110, shown on the page */
static unsigned short simple_word;       /* last OUT 112, shown on the page */
static unsigned char simple_in_byte;     /* user-typed value returned by IN 110 */
static unsigned short simple_in_word;    /* user-typed value returned by IN 112 */

static unsigned char robot_map[54];
static int robot_x, robot_y, robot_heading;
static unsigned char robot_cmd, robot_data, robot_status;
static int robot_busy_ticks;
static unsigned char printer_data;

struct PrinterPage {
    std::vector<std::string> lines;
    int x, y;
    PrinterPage() { clear(); }
    void clear() { lines.assign(1, std::string()); x = y = 0; }
};
static PrinterPage printer;

/* ---------------------------------------------------------------------- */
/* palette and small drawing helpers                                       */

static Fl_Color rgb(int r, int g, int b) { return fl_rgb_color((uchar)r, (uchar)g, (uchar)b); }
static Fl_Color c_face()   { return rgb(240, 240, 240); }
static Fl_Color c_white()  { return rgb(255, 255, 255); }
static Fl_Color c_black()  { return rgb(0, 0, 0); }
static Fl_Color c_grey()   { return rgb(128, 128, 128); }
static Fl_Color c_dgrey()  { return rgb(64, 64, 64); }
static Fl_Color c_lgrey()  { return rgb(192, 192, 192); }
static Fl_Color c_blue()   { return rgb(0, 0, 255); }
static Fl_Color c_navy()   { return rgb(0, 0, 128); }
static Fl_Color c_red()    { return rgb(255, 0, 0); }
static Fl_Color c_yellow() { return rgb(255, 255, 0); }
static Fl_Color c_green()  { return rgb(0, 255, 0); }
static Fl_Color c_dgreen() { return rgb(0, 128, 0); }
static Fl_Color c_magenta(){ return rgb(255, 0, 255); }
static Fl_Color c_purple() { return rgb(128, 0, 128); }

static void text_c(int x, int y, const char *s, Fl_Color col, int size, int bold = 0)
{
    fl_font(bold ? FL_HELVETICA_BOLD : FL_HELVETICA, size);
    fl_color(col);
    fl_draw(s, x, y);
}

static void text_r(int right, int y, const char *s, Fl_Color col, int size, int bold = 0)
{
    fl_font(bold ? FL_HELVETICA_BOLD : FL_HELVETICA, size);
    fl_color(col);
    fl_draw(s, right - (int)fl_width(s), y);
}

static void text_ctr(int cx, int y, const char *s, Fl_Color col, int size, int bold = 0)
{
    fl_font(bold ? FL_HELVETICA_BOLD : FL_HELVETICA, size);
    fl_color(col);
    fl_draw(s, cx - (int)(fl_width(s) / 2), y);
}

/* A binary register display: the hexadecimal bit names on one line, the bits
 * on the next, set bits red and clear bits blue - exactly the colouring the VB
 * forms use.  The names run MSB to LSB, so bit 15 is "F" and bit 0 is "0".
 * Every form uses a different type size, so the pitch and font are arguments:
 * traffic has sixteen 8px digits, the robot eight 8px digits, the stepper
 * eight and the thermometer eight at 9px. */
static void draw_bit_labels_p(int x, int y, int n, int pitch, int font,
                              Fl_Color col)
{
    char digit[2] = { 0, 0 };
    int i;
    fl_font(FL_COURIER, font);
    fl_color(col);
    for (i = 0; i < n; i++) {
        digit[0] = "0123456789ABCDEF"[n - 1 - i];
        fl_draw(digit, x + i * pitch, y);
    }
}

static void draw_bit_values_p(int x, int y, int n, unsigned value,
                              int pitch, int font)
{
    char digit[2] = { 0, 0 };
    int i;
    fl_font(FL_COURIER_BOLD, font);
    for (i = 0; i < n; i++) {
        digit[0] = (value >> (n - 1 - i)) & 1 ? '1' : '0';
        fl_color(digit[0] == '1' ? c_red() : c_blue());
        fl_draw(digit, x + i * pitch, y);
    }
}

static void draw_bit_labels(int x, int y, int n)
{
    draw_bit_labels_p(x, y, n, 9, 12, c_grey());
}

static void draw_bit_values(int x, int y, int n, unsigned value)
{
    draw_bit_values_p(x, y, n, value, 9, 14);
}

static void draw_bits(int x, int y, int n, unsigned value)
{
    draw_bit_labels(x, y, n);
    draw_bit_values(x, y + 16, n, value);
}

static void sunken(int x, int y, int w, int h)
{
    fl_color(c_white());
    fl_rectf(x, y, w, h);
    fl_color(c_white());
    fl_line(x, y + h - 1, x + w - 1, y + h - 1);
    fl_line(x + w - 1, y, x + w - 1, y + h - 1);
    fl_color(c_dgrey());
    fl_line(x, y, x + w - 1, y);
    fl_line(x, y, x, y + h - 1);
}

static void raised(int x, int y, int w, int h, int pressed = 0)
{
    fl_color(c_face());
    fl_rectf(x, y, w, h);
    fl_color(pressed ? c_dgrey() : c_white());
    fl_line(x, y, x + w - 1, y);
    fl_line(x, y, x, y + h - 1);
    fl_color(pressed ? c_white() : c_dgrey());
    fl_line(x, y + h - 1, x + w - 1, y + h - 1);
    fl_line(x + w - 1, y, x + w - 1, y + h - 1);
}

/* ---------------------------------------------------------------------- */
/* LED display                                                             */

static const unsigned char SEG_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

/* The digits are a pixel font measuring 12x17 at a pitch of 24, starting at
 * x=38/y=1; "port 199 (2 bytes)" is a grey label at x=36/baseline 46. */
static void draw_seg_digit(int x, int y, int seg)
{
    const int t = 3;
    if (seg & 0x01) fl_rectf(x + 1, y,     10, t);      /* a */
    if (seg & 0x02) fl_rectf(x + 9, y + 2,  t,  6);     /* b */
    if (seg & 0x04) fl_rectf(x + 9, y + 9,  t,  6);     /* c */
    if (seg & 0x08) fl_rectf(x + 1, y + 14, 10, t);     /* d */
    if (seg & 0x10) fl_rectf(x,     y + 9,  t,  6);     /* e */
    if (seg & 0x20) fl_rectf(x,     y + 2,  t,  6);     /* f */
    if (seg & 0x40) fl_rectf(x + 1, y + 7,  10, t);     /* g */
}

static void draw_led()
{
    char s[8];
    int i;
    snprintf(s, sizeof s, "%05u", led_value % 100000);
    fl_color(c_black());
    fl_rectf(0, 0, 206, 90);
    fl_color(c_green());
    for (i = 0; i < 5; i++)
        draw_seg_digit(38 + i * 24, 1, SEG_DIGIT[s[i] - '0']);
    text_c(36, 46, "port 199 (2 bytes)", rgb(128, 128, 128), 11, 1);
}

/* ---------------------------------------------------------------------- */
/* traffic lights                                                          */

/* The form is a black sheet with a 210x172 road picture at (8,2), four
 * traffic-light groups, four cars and a 12-bit register display at (20,194).
 * Only bits 0..11 are wired; the upper four digits are grey. */
static void lamp(int cx, int cy, int bit, unsigned v, Fl_Color on)
{
    fl_color((v >> bit) & 1 ? on : rgb(49, 49, 49));
    fl_pie(cx - 6, cy - 6, 11, 11, 0, 360);
    fl_color(rgb(24, 24, 24));
    fl_arc(cx - 6, cy - 6, 11, 11, 0, 360);
}

static void label_xy(int x, int y, const char *t)
{
    text_c(x, y, t, c_yellow(), 9, 1);
}

static void traffic_car(int x, int y, int w, int h, int vertical)
{
    fl_color(c_white());
    fl_rectf(x, y, w, h);
    fl_color(c_grey());
    fl_rect(x, y, w, h);
    fl_color(rgb(224, 224, 224));
    if (vertical) {
        fl_rectf(x + 3, y + 3, w - 6, 6);
        fl_rectf(x + 3, y + h - 9, w - 6, 5);
    } else {
        fl_rectf(x + 4, y + 3, 6, h - 6);
        fl_rectf(x + w - 11, y + 3, 5, h - 6);
    }
    fl_color(c_black());
    fl_rectf(x + w / 2 - 4, y + h / 2 - 3, 8, 6);
    fl_color(c_red());
    if (vertical) {
        fl_rectf(x + 2, y + h - 4, 4, 3);
        fl_rectf(x + w - 6, y + h - 4, 4, 3);
    } else {
        fl_rectf(x + 2, y + 2, 3, 4);
        fl_rectf(x + 2, y + h - 6, 3, 4);
    }
    fl_color(c_yellow());
    if (vertical) {
        fl_rectf(x + 2, y + 1, 4, 3);
        fl_rectf(x + w - 6, y + 1, 4, 3);
    } else {
        fl_rectf(x + w - 5, y + 2, 3, 4);
        fl_rectf(x + w - 5, y + h - 6, 3, 4);
    }
}

static void traffic_arrow(int x0, int y0, int x1, int y1)
{
    fl_color(rgb(160, 160, 160));
    fl_line(x0, y0, x1, y1);
    if (y1 > y0) {                       /* down */
        fl_line(x1 - 5, y1 - 6, x1, y1); fl_line(x1 + 5, y1 - 6, x1, y1);
    } else if (y1 < y0) {                /* up */
        fl_line(x1 - 5, y1 + 6, x1, y1); fl_line(x1 + 5, y1 + 6, x1, y1);
    } else if (x1 > x0) {                /* right */
        fl_line(x1 - 6, y1 - 5, x1, y1); fl_line(x1 - 6, y1 + 5, x1, y1);
    } else {
        fl_line(x1 + 6, y1 - 5, x1, y1); fl_line(x1 + 6, y1 + 5, x1, y1);
    }
}

static void draw_traffic()
{
    unsigned v = traffic_value & 0x0FFF;
    int i;

    fl_color(c_black());
    fl_rectf(0, 0, 281, 320);
    fl_color(c_white());
    fl_rect(8, 2, 209, 172);
    fl_color(rgb(248, 248, 248));
    fl_rectf(9, 3, 207, 169);

    /* Green corners. */
    fl_color(c_dgreen());
    fl_rectf(15, 3, 55, 39);
    fl_rectf(159, 3, 50, 39);
    fl_rectf(15, 130, 55, 39);
    fl_rectf(159, 130, 50, 39);

    /* White wires from the lamp groups to thebit display. */
    fl_color(c_white());
    fl_line(10, 12, 10, 34);
    for (i = 0; i < 3; i++) fl_line(10, 12 + i * 11, 62, 12 + i * 11);
    fl_line(211, 13, 211, 32);
    for (i = 0; i < 3; i++) fl_line(159, 13 + i * 9, 211, 13 + i * 9);
    for (i = 0; i < 3; i++) fl_line(32 + i * 12, 139, 32 + i * 12, 168);
    fl_line(211, 139, 211, 158);
    for (i = 0; i < 3; i++) fl_line(160, 139 + i * 9, 211, 139 + i * 9);

    /* Lane dashes. */
    fl_color(rgb(200, 200, 200));
    for (i = 0; i < 6; i++) {
        fl_rectf(96, 50 + i * 12, 1, 6);
        fl_rectf(14 + i * 9, 117, 5, 1);
        fl_rectf(158 + i * 9, 117, 5, 1);
        fl_rectf(96, 158 + i * 4, 1, 3);
    }
    fl_color(c_dgreen());
    fl_rectf(119, 89, 2, 19);

    /* The three-lamp groups.  Each group counts red/yellow/green per bit%3. */
    lamp(80, 33, 6, v, c_red());
    lamp(80, 25, 7, v, c_yellow());
    lamp(80, 17, 8, v, c_green());
    label_xy(62, 24, "8"); label_xy(62, 32, "7"); label_xy(62, 40, "6");

    lamp(166, 58, 3, v, c_red());
    lamp(177, 58, 4, v, c_yellow());
    lamp(188, 58, 5, v, c_green());
    label_xy(162, 50, "3"); label_xy(173, 50, "4"); label_xy(184, 50, "5");

    lamp(68, 122, 9, v, c_red());
    lamp(57, 122, 10, v, c_yellow());
    lamp(46, 122, 11, v, c_green());
    label_xy(67, 141, "9"); label_xy(56, 141, "A"); label_xy(45, 141, "B");

    lamp(150, 134, 0, v, c_red());
    lamp(150, 144, 1, v, c_yellow());
    lamp(150, 154, 2, v, c_green());
    label_xy(158, 138, "0"); label_xy(158, 148, "1"); label_xy(158, 158, "2");

    traffic_car(114, 4, 24, 22, 1);
    traffic_car(172, 64, 29, 26, 0);
    traffic_car(54, 95, 25, 21, 0);
    traffic_car(91, 147, 22, 27, 1);
    traffic_arrow(104, 15, 104, 36);
    traffic_arrow(58, 103, 74, 103);
    traffic_arrow(161, 78, 145, 78);
    traffic_arrow(121, 145, 121, 125);

    /* Register panel: names F..0 and 16 values at an 8px pitch. */
    fl_color(c_white());
    fl_rectf(20, 194, 183, 37);
    draw_bit_labels_p(48, 204, 16, 8, 8, rgb(100, 100, 100));
    fl_font(FL_COURIER_BOLD, 8);
    for (i = 0; i < 16; i++) {
        char d[2] = { (char)('0' + ((v >> (15 - i)) & 1)), 0 };
        fl_color(i < 4 ? rgb(170, 170, 170) :
                 (d[0] == '1' ? c_red() : c_blue()));
        fl_draw(d, 48 + i * 8, 221);
    }
    text_ctr(111, 245, "port 4 (word - 16 bits)", c_white(), 11, 1);
}

/* ---------------------------------------------------------------------- */
/* stepper motor                                                           */

static const int STEP_ORDER[4] = { 6, 4, 3, 2 };

static void draw_stepper()
{
    int cx = 113, cy = 48, i, idx = 0;
    double ang;
    char s[8];

    fl_color(c_black());
    fl_rectf(0, 0, 288, 237);
    fl_color(c_white());
    fl_rectf(0, 0, 231, 184);

    /* The pale mounting plate and the ring of eight coils. */
    fl_color(c_face());
    fl_rectf(70, 4, 88, 88);
    fl_color(c_white());
    fl_pie(cx - 31, cy - 31, 63, 63, 0, 360);
    fl_color(c_grey());
    fl_arc(cx - 31, cy - 31, 63, 63, 0, 360);
    for (i = 0; i < 8; i++) {
        double a = i * 45.0 * 3.14159265358979 / 180.0;
        int x = cx + (int)(sin(a) * 38.0) - 7;
        int y = cy - (int)(cos(a) * 38.0) - 7;
        fl_color(c_face());
        fl_rectf(x, y, 14, 14);
        fl_color(c_grey());
        fl_rect(x, y, 14, 14);
    }

    /* The shaft follows the four half-step patterns used by the example. */
    for (i = 0; i < 4; i++) if ((stepper_phase & 0x0F) == STEP_ORDER[i]) idx = i;
    ang = idx * 45.0 * 3.14159265358979 / 180.0;
    fl_color(c_green());
    fl_line(cx, cy, cx + (int)(sin(ang) * 43.0), cy + (int)(cos(ang) * 43.0));
    fl_line(cx + 1, cy, cx + 1 + (int)(sin(ang) * 43.0),
            cy + (int)(cos(ang) * 43.0));
    fl_line(cx - 1, cy, cx - 1 + (int)(sin(ang) * 43.0),
            cy + (int)(cos(ang) * 43.0));
    fl_color(c_dgreen());
    fl_pie(cx - 2, cy - 2, 5, 5, 0, 360);

    text_c(11, 106, "ready", (stepper_phase & 0x80) ? c_red() : c_grey(), 11, 1);
    text_c(28, 120, "7", c_black(), 11, 1);

    /* Bits 2, 1 and 0: the coils, drawn as blue lamps. */
    for (i = 0; i < 3; i++) {
        int bit = 2 - i;
        int x = 82 + i * 27;
        fl_color(c_blue());
        fl_rectf(x, 98, 13, 13);
        fl_color(c_black());
        fl_rect(x, 98, 13, 13);
        snprintf(s, sizeof s, "%d", bit);
        text_c(x + 7, 127, s, c_black(), 11, 1);
    }
    draw_bit_labels_p(84, 153, 8, 8, 8, rgb(100, 100, 100));
    draw_bit_values_p(84, 167, 8, stepper_phase, 8, 8);
}

/* ---------------------------------------------------------------------- */
/* thermometer                                                             */

/* 0 C sits at y=281 and each degree moves the mercury 1.7 pixels, matching
 * the nine printed graduations (120 down to -40). */
static int temp_y(int t)
{
    return 281 - (int)(t * 1.7 + 0.5);
}

static void draw_thermo()
{
    char s[32];
    int y, i, top;
    int levels[9] = { 120, 100, 80, 60, 40, 20, 0, -20, -40 };

    fl_color(c_black());
    fl_rectf(0, 0, 411, 666);
    fl_color(c_white());
    fl_rectf(0, 0, 328, 526);

    text_ctr(136, 11, "Thermometer & Heater", c_black(), 11, 1);
    text_ctr(136, 22, "Ports 125 - 127", c_navy(), 10, 1);

    /* Graduations and the tube. */
    text_c(44, 53, "\260 C", c_navy(), 10, 1);
    for (i = 0; i < 9; i++) {
        y = temp_y(levels[i]);
        snprintf(s, sizeof s, "%s%d", levels[i] > 0 ? "+ " : "", abs(levels[i]));
        text_r(115, y + 3, s, c_black(), 11, 1);
    }
    text_c(151, 84, "0 \260", c_red(), 11, 1);

    fl_color(c_white());
    fl_rectf(20, 66, 52, 278);
    fl_color(c_grey());
    fl_rect(20, 66, 52, 278);
    fl_arc(20, 66, 52, 22, 180, 360);
    fl_arc(20, 322, 52, 22, 0, 180);
    fl_color(c_navy());
    fl_pie(49, 71, 12, 12, 0, 360);

    top = temp_y(thermo_temp);
    if (top > 344) top = 344;
    if (top < 70) top = 70;
    fl_color(c_red());
    fl_rectf(36, top, 20, 397 - top);
    fl_pie(32, 383, 28, 28, 0, 360);
    fl_color(c_black());
    fl_arc(32, 383, 28, 28, 0, 360);

    /* Data port 125: signed byte temperature on a white/navy field. */
    fl_color(c_white());
    fl_rectf(192, 66, 125, 29);
    fl_color(c_navy());
    fl_rect(192, 66, 125, 29);
    draw_bit_values_p(226, 89, 8, (unsigned)(signed char)thermo_temp, 9, 9);
    text_ctr(254, 110, "Data port 125", c_navy(), 10, 1);

    /* Air temperature and control port. */
    text_c(170, 253, "air temperature:", c_black(), 11, 1);
    sunken(185, 265, 82, 21);
    snprintf(s, sizeof s, "%d", thermo_temp);
    text_ctr(226, 282, s, c_black(), 11, 1);

    fl_color(c_white());
    fl_rectf(190, 375, 125, 25);
    fl_color(c_navy());
    fl_rect(190, 375, 125, 25);
    draw_bit_values_p(226, 393, 8, thermo_heater ? 1 : 0, 9, 9);
    text_ctr(257, 424, "Control port 127", c_black(), 11, 1);
    raised(235, 434, 37, 21);
    text_ctr(253, 450, "On", (thermo_heater ? c_red() : c_black()), 9, 1);

    /* Magenta schematic wiring to both ports and to the heater. */
    fl_color(c_magenta());
    fl_line(28, 66, 28, 38); fl_line(28, 38, 255, 38); fl_line(255, 38, 255, 66);
    fl_line(139, 352, 250, 352); fl_line(250, 352, 250, 375);
    fl_line(139, 470, 139, 510); fl_line(139, 510, 68, 510);

    /* Heater block and stand. */
    fl_color(c_purple());
    fl_rectf(10, 410, 75, 10);
    fl_color(c_navy());
    fl_pie(14, 470, 77, 28, 180, 360);
    fl_rectf(14, 483, 77, 39);
    fl_rectf(34, 476, 20, 8);
    fl_color(c_yellow());
    fl_pie(62, 504, 12, 12, 0, 360);
    fl_color(c_black());
    fl_arc(62, 504, 12, 12, 0, 360);

    {
        int w2;
        fl_font(FL_HELVETICA_BOLD, 10);
        w2 = (int)fl_width("emu8086");
        text_r(327, 497, "emu8086", c_navy(), 10, 1);
        text_r(323 - w2, 497, "designed for ", c_grey(), 10, 1);
    }
}

/* ---------------------------------------------------------------------- */
/* robot                                                                   */

static const unsigned char ROBOT_MAP0[54] = {
    0,0,0,0,0,0,0,11,0,
    4,10,0,0,7,0,0,0,0,
    0,0,0,0,0,0,0,0,0,
    0,0,7,0,0,0,0,11,0,
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0
};

static void save_robot_map()
{
    char path[MUE_MAXP];
    FILE *f;
    mue_home_path(path, sizeof path, "DEVICES\\robot_map.dat");
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(robot_map, 1, sizeof robot_map, f);
    fclose(f);
}

static void load_robot_map()
{
    char path[MUE_MAXP];
    FILE *f;
    mue_home_path(path, sizeof path, "DEVICES\\robot_map.dat");
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(robot_map, 1, sizeof robot_map, f) != sizeof robot_map)
        memcpy(robot_map, ROBOT_MAP0, sizeof robot_map);
    fclose(f);
}

static void robot_reset_map()
{
    int r, c;
    load_robot_map();
    robot_x = robot_y = 0;
    robot_heading = 2;                 /* facing down, as the form starts */
    for (r = 0; r < 6; r++) for (c = 0; c < 9; c++)
        if (robot_map[r * 9 + c] == 4) { robot_x = c; robot_y = r; }
    robot_cmd = robot_data = robot_status = 0;
    robot_busy_ticks = 0;
}

static void robot_wall_tile(int x, int y, int w, int h)
{
    int i;
    fl_color(c_lgrey());
    fl_rectf(x, y, w, h);
    fl_color(c_black());
    fl_rect(x, y, w, h);
    for (i = 6; i < w + h; i += 6) {
        if (i < w) { fl_line(x, y + i, x + i, y); fl_line(x + w - i, y + h, x + w, y + h - i); }
        if (i < h) { fl_line(x + i, y + h, x + w, y + h - i); }
    }
}

static void robot_lamp_tile(int x, int y, int d, int on)
{
    fl_color(on ? c_yellow() : rgb(192, 192, 192));
    fl_pie(x, y, d, d, 0, 360);
    fl_color(c_black());
    fl_arc(x, y, d, d, 0, 360);
    fl_line(x + 3, y + 3, x + d - 3, y + d - 3);
    fl_line(x + d - 3, y + 3, x + 3, y + d - 3);
}

static void draw_robot_icon(int x, int y, int heading, int small)
{
    int s = small ? 18 : 28;
    int pad = small ? 2 : 4;
    int cx = x + pad + s / 2, cy = y + pad + s / 2;
    int len = s / 2 - 2;
    int dx = 0, dy = 0;

    if (heading == 0) dy = -len; else if (heading == 2) dy = len;
    else if (heading == 1) dx = len; else dx = -len;

    fl_color(c_blue());
    fl_rectf(x + pad, y + pad, s, s);
    fl_color(c_black());
    fl_rect(x + pad, y + pad, s, s);
    fl_color(c_yellow());
    fl_line(cx, cy, cx + dx, cy + dy);
    fl_line(cx - 2, cy - 2, cx + dx, cy + dy);
    fl_line(cx + 2, cy + 2, cx + dx, cy + dy);
    fl_line(cx - 2, cy + 2, cx + dx, cy + dy);
    fl_line(cx + 2, cy - 2, cx + dx, cy + dy);
}

static void draw_robot()
{
    int r, c;

    fl_color(c_black());
    fl_rectf(0, 0, 372, 333);
    fl_color(c_white());
    fl_rectf(0, 0, 297, 261);
    fl_color(rgb(160, 160, 160));
    fl_rect(0, 0, 297, 186);

    /* Nine columns by six rows of 32px cells, with a 4px left margin. */
    for (r = 0; r < 6; r++) {
        for (c = 0; c < 9; c++) {
            int v = robot_map[r * 9 + c];
            int x = 4 + c * 32, y = r * 32;
            if (v == 11) robot_wall_tile(x + 1, y + 1, 30, 30);
            else if (v == 10) robot_lamp_tile(x + 2, y + 2, 27, 1);
            else if (v == 7) robot_lamp_tile(x + 2, y + 2, 27, 0);
        }
    }
    draw_robot_icon(4 + robot_x * 32, robot_y * 32, robot_heading, 0);

    /* Legend icons: robot, wall, switched-on lamp. */
    draw_robot_icon(6, 232, 1, 1);
    robot_wall_tile(38, 232, 28, 22);
    robot_lamp_tile(72, 230, 28, 1);

    draw_bit_labels_p(183, 204, 8, 8, 8, rgb(100, 100, 100));
    text_c(260, 204, "port", c_black(), 11, 1);

    text_c(110, 225, "command:", c_grey(), 11, 1);
    draw_bit_values_p(183, 225, 8, robot_cmd, 8, 8);
    text_c(265, 225, "9", c_black(), 11, 1);

    text_c(110, 237, "data:", c_grey(), 11, 1);
    draw_bit_values_p(183, 237, 8, robot_data, 8, 8);
    text_c(265, 237, "10", c_black(), 11, 1);

    text_c(110, 249, "status:", c_grey(), 11, 1);
    draw_bit_values_p(183, 249, 8, robot_status, 8, 8);
    text_c(265, 249, "11", c_black(), 11, 1);
}

static void robot_command(unsigned char v)
{
    int front = 0, fx = robot_x, fy = robot_y;
    robot_cmd = v;
    robot_busy_ticks = 3;
    robot_status = 0x02;
    switch (v) {
    case 1: /* forward */
        if (robot_heading == 0) fy--;
        else if (robot_heading == 2) fy++;
        else if (robot_heading == 1) fx++;
        else fx--;
        if (fx >= 0 && fx < 9 && fy >= 0 && fy < 6 &&
            robot_map[fy * 9 + fx] != 11) {
            robot_map[robot_y * 9 + robot_x] = 0;
            robot_x = fx; robot_y = fy;
            robot_map[robot_y * 9 + robot_x] = 4;
        }
        break;
    case 2: robot_heading = (robot_heading + 3) & 3; break;
    case 3: robot_heading = (robot_heading + 1) & 3; break;
    case 4: /* examine */
        if (robot_heading == 0) fy--; else if (robot_heading == 2) fy++;
        else if (robot_heading == 1) fx++; else fx--;
        if (fx < 0 || fx >= 9 || fy < 0 || fy >= 6)
            front = 255;
        else {
            front = robot_map[fy * 9 + fx];
            if (front == 11) front = 255;
            else if (front == 10) front = 7;
            else if (front == 7) front = 6;
            else front = 0;
        }
        robot_data = (unsigned char)front;
        robot_status = 0x01;
        break;
    case 5: /* lamp on */
    case 6: /* lamp off */
        if (robot_heading == 0) fy--; else if (robot_heading == 2) fy++;
        else if (robot_heading == 1) fx++; else fx--;
        if (fx >= 0 && fx < 9 && fy >= 0 && fy < 6) {
            int k = fy * 9 + fx;
            if (robot_map[k] == 7 || robot_map[k] == 10)
                robot_map[k] = (v == 5) ? 10 : 7;
        }
        robot_status = 0x00;
        break;
    default:
        break;
    }
    if (v == 1 || v == 5 || v == 6) save_robot_map();
}

/* ---------------------------------------------------------------------- */
/* printer                                                                 */

static void printer_new_page() { printer.clear(); }

static void printer_put(unsigned char ch)
{
    if (ch > 31) {
        if (printer.y >= (int)printer.lines.size())
            printer.lines.push_back(std::string());
        std::string &ln = printer.lines[printer.y];
        if (printer.x < 0) printer.x = 0;
        if ((int)ln.size() <= printer.x) ln.resize((size_t)printer.x + 1, ' ');
        ln[(size_t)printer.x] = (char)ch;
        printer.x++;
    } else if (ch == 8) {
        if (printer.x > 0) printer.x--;
    } else if (ch == 10) {
        printer.y++;
        if ((int)printer.lines.size() <= printer.y)
            printer.lines.push_back(std::string());
    } else if (ch == 13) {
        printer.x = 0;
    } else if (ch == 7) {
        /* bell: nothing visible on a silent virtual printer */
    }
}

static void draw_printer()
{
    int i;
    fl_color(c_black());
    fl_rectf(0, 0, 333, 425);
    fl_color(c_face());
    fl_rectf(0, 0, 265, 31);
    /* One grey divider line, then the page immediately: the original has no
     * black gap between the button strip and the paper. */
    fl_color(rgb(128, 128, 128));
    fl_rectf(0, 31, 264, 1);
    fl_color(c_white());
    fl_rectf(0, 32, 264, 299);

    raised(2, 6, 81, 25);
    text_ctr(42, 24, "new page", c_black(), 11, 0);
    text_c(100, 23, "printer data port: 130d", c_black(), 11, 1);

    fl_font(FL_COURIER, 12);
    fl_color(c_black());
    for (i = 0; i < (int)printer.lines.size() && i < 18; i++) {
        fl_draw(printer.lines[i].c_str(), 6, 51 + i * 16);
    }
    /* The caret is the printer head: a faint mark where the next character
     * will land, which is how the original's page reads while it prints. */
    fl_color(rgb(220, 220, 220));
    fl_rectf(6 + printer.x * 7, 41 + printer.y * 16, 6, 12);
}

/* ---------------------------------------------------------------------- */
/* simple i/o                                                              */

static void draw_simple()
{
    char s[32];
    fl_color(c_black());
    fl_rectf(0, 0, 337, 206);
    fl_color(c_face());
    fl_rectf(0, 0, 269, 158);

    fl_font(FL_HELVETICA_ITALIC, 10);
    fl_color(c_black());
    fl_draw("for hexadecimal digits add h suffix", 25, 7);

    text_c(11, 32, "write byte to port 110:", c_black(), 11, 1);
    text_c(11, 64, "write word to port 112:", c_black(), 11, 1);
    sunken(193, 17, 67, 19);
    sunken(193, 49, 67, 19);

    sunken(10, 75, 219, 18);
    text_c(15, 92, " updating input ports every 100 milliseconds ", c_black(), 9, 0);

    text_c(11, 120, "read byte from port 110:", c_black(), 11, 1);
    text_c(11, 148, "read word from port 112:", c_black(), 11, 1);
    sunken(176, 105, 84, 19);
    sunken(176, 133, 84, 19);

    snprintf(s, sizeof s, "%02Xh", simple_byte);
    text_c(203, 120, s, c_black(), 11, 1);
    snprintf(s, sizeof s, "%Xh", simple_word);
    text_c(203, 148, s, c_black(), 11, 1);
}

/* ---------------------------------------------------------------------- */
/* VGA state                                                               */

static void draw_vga()
{
    /* The original's status form is only 133x16, so its caption is clipped:
     * a five-pixel grey strip with the tops of "EMULATION" and black below. */
    fl_color(c_black());
    fl_rectf(0, 0, 133, 16);
    fl_color(c_face());
    fl_rectf(0, 0, 133, 5);
    text_c(4, 2, "EMULATION", c_black(), 10, 0);
}

/* ---------------------------------------------------------------------- */
/* windows                                                                 */

class DevCanvas : public Fl_Widget {
public:
    int dev;
    DevCanvas(int x, int y, int w, int h, int d)
        : Fl_Widget(x, y, w, h), dev(d) {}
    void draw() override {
        switch (dev) {
        case DEV_LED: draw_led(); break;
        case DEV_TRAFFIC: draw_traffic(); break;
        case DEV_STEPPER: draw_stepper(); break;
        case DEV_THERMO: draw_thermo(); break;
        case DEV_ROBOT: draw_robot(); break;
        case DEV_PRINTER: draw_printer(); break;
        case DEV_SIMPLE: draw_simple(); break;
        default: draw_vga(); break;
        }
    }
    int handle(int ev) override {
        if (ev == FL_PUSH && dev == DEV_PRINTER) {
            if (Fl::event_x() >= 10 && Fl::event_x() <= 80 &&
                Fl::event_y() >= 8 && Fl::event_y() <= 33) {
                printer_new_page();
                redraw();
                return 1;
            }
        }
        if (ev == FL_PUSH && dev == DEV_THERMO) {
            if (Fl::event_x() >= 235 && Fl::event_x() <= 275 &&
                Fl::event_y() >= 432 && Fl::event_y() <= 456) {
                thermo_heater = !thermo_heater;
                redraw();
                return 1;
            }
        }
        return Fl_Widget::handle(ev);
    }
};

static void dev_hidden(Fl_Widget *, void *v)
{
    int id = (int)(intptr_t)v;
    if (id >= 0 && id < DEV_COUNT) dev_open[id] = 0;
}

class DevWindow : public Fl_Double_Window {
public:
    int dev;
    DevWindow(int w, int h, const char *t, int d)
        : Fl_Double_Window(w, h, t), dev(d) {}
    void hide() override {
        dev_open[dev] = 0;
        Fl_Double_Window::hide();
    }
};

static MueField *simple_wb, *simple_ww;

static void cb_simple_edit(Fl_Widget *w, void *which)
{
    unsigned v = (unsigned)strtoul(((Fl_Input *)w)->value(), NULL, 16);
    if ((intptr_t)which == 0) simple_in_byte = (unsigned char)v;
    else simple_in_word = (unsigned short)v;
}

static const struct {
    const char *title, *menu;
    int w, h;
} DEV_INFO[DEV_COUNT] = {
    { "LED display",                 "LED_Display.exe",   206,  90 },
    { "Traffic Lights",              "Traffic_Lights.exe",281, 320 },
    { "stepper-motor on port 7",     "Stepper_motor.exe", 288, 237 },
    { "thermometer & heater - io device", "Thermometer.exe", 411, 666 },
    { "robot on port 9         (3 bytes)", "Robot.exe",   372, 333 },
    { "printer emulation",           "Printer.exe",       333, 425 },
    { "simple io test",              "Simple.exe",        337, 206 },
    { "VGA STATE",                   "VGA_STATE.exe",     133,  16 },
};

void devices_open(int id)
{
    if (id < 0 || id >= DEV_COUNT) return;
    if (!dev_win[id]) {
        DevWindow *w = new DevWindow(DEV_INFO[id].w, DEV_INFO[id].h,
                                     DEV_INFO[id].title, id);
        w->color(c_face());
        w->size_range(DEV_INFO[id].w, DEV_INFO[id].h,
                      DEV_INFO[id].w, DEV_INFO[id].h);
        DevCanvas *c = new DevCanvas(0, 0, DEV_INFO[id].w, DEV_INFO[id].h, id);
        w->end();
        w->resizable(0);
        w->callback(dev_hidden, (void *)(intptr_t)id);
        dev_win[id] = w;
        dev_canvas[id] = c;
        if (id == DEV_SIMPLE) {
            simple_wb = new MueField(193, 17, 67, 19, 0);
            simple_wb->value("0");
            simple_wb->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
            simple_wb->callback(cb_simple_edit, (void *)0);
            w->add(simple_wb);
            simple_ww = new MueField(193, 49, 67, 19, 0);
            simple_ww->value("0");
            simple_ww->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
            simple_ww->callback(cb_simple_edit, (void *)1);
            w->add(simple_ww);
        }
    }
    dev_open[id] = 1;
    dev_win[id]->show();
    dev_win[id]->take_focus();
    dev_win[id]->redraw();
}

int devices_find(const char *filename)
{
    int i;
    if (!filename) return -1;
    for (i = 0; i < DEV_COUNT; i++)
        if (!strcasecmp(filename, DEV_INFO[i].menu)) return i;
    return -1;
}

void devices_open_name(const char *filename)
{
    int id = devices_find(filename);
    if (id >= 0) { devices_open(id); return; }
#ifdef _WIN32
    if (filename && *filename) {
        char path[MUE_MAXP];
        mue_home_path(path, sizeof path, "DEVICES");
        snprintf(path + strlen(path), sizeof path - strlen(path), "\\%s", filename);
        ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
    }
#else
    (void)filename;
#endif
}

/* ---------------------------------------------------------------------- */
/* port model                                                              */

int devices_io(int is_out, unsigned port, unsigned char *value)
{
    unsigned char v = *value;

    if (is_out) {
        if (port == 199 && dev_open[DEV_LED]) {
            led_value = (led_value & 0xFF00) | v;
            if (dev_canvas[DEV_LED]) dev_canvas[DEV_LED]->redraw();
            return 1;
        }
        if (port == 200 && dev_open[DEV_LED]) {
            led_value = (led_value & 0x00FF) | ((unsigned)v << 8);
            if (dev_canvas[DEV_LED]) dev_canvas[DEV_LED]->redraw();
            return 1;
        }
        if (port == 4 && dev_open[DEV_TRAFFIC]) {
            traffic_value = (traffic_value & 0xFF00) | v;
            if (dev_canvas[DEV_TRAFFIC]) dev_canvas[DEV_TRAFFIC]->redraw();
            return 1;
        }
        if (port == 5 && dev_open[DEV_TRAFFIC]) {
            traffic_value = (traffic_value & 0x00FF) | ((unsigned)v << 8);
            if (dev_canvas[DEV_TRAFFIC]) dev_canvas[DEV_TRAFFIC]->redraw();
            return 1;
        }
        if (port == 7 && dev_open[DEV_STEPPER]) {
            stepper_phase = (unsigned char)(0x80 | (v & 0x0F));
            if (dev_canvas[DEV_STEPPER]) dev_canvas[DEV_STEPPER]->redraw();
            return 1;
        }
        if (port == 127 && dev_open[DEV_THERMO]) {
            thermo_heater = v != 0;
            if (dev_canvas[DEV_THERMO]) dev_canvas[DEV_THERMO]->redraw();
            return 1;
        }
        if (port == 9 && dev_open[DEV_ROBOT]) {
            robot_command(v);
            if (dev_canvas[DEV_ROBOT]) dev_canvas[DEV_ROBOT]->redraw();
            return 1;
        }
        if (port == 130 && dev_open[DEV_PRINTER]) {
            if (v != 0) printer_put(v);
            *value = 0;                    /* documented: port clears itself */
            if (dev_canvas[DEV_PRINTER]) dev_canvas[DEV_PRINTER]->redraw();
            return 1;
        }
        if (port == 110 && dev_open[DEV_SIMPLE]) {
            simple_byte = v;
            if (dev_canvas[DEV_SIMPLE]) dev_canvas[DEV_SIMPLE]->redraw();
            return 1;
        }
        if (port == 112 && dev_open[DEV_SIMPLE]) {
            simple_word = (simple_word & 0xFF00) | v;
            if (dev_canvas[DEV_SIMPLE]) dev_canvas[DEV_SIMPLE]->redraw();
            return 1;
        }
        if (port == 113 && dev_open[DEV_SIMPLE]) {
            simple_word = (simple_word & 0x00FF) | ((unsigned)v << 8);
            if (dev_canvas[DEV_SIMPLE]) dev_canvas[DEV_SIMPLE]->redraw();
            return 1;
        }
    } else {
        if (port == 199 && dev_open[DEV_LED]) { *value = (unsigned char)(led_value & 0xFF); return 1; }
        if (port == 200 && dev_open[DEV_LED]) { *value = (unsigned char)(led_value >> 8); return 1; }
        if (port == 4 && dev_open[DEV_TRAFFIC]) { *value = (unsigned char)(traffic_value & 0xFF); return 1; }
        if (port == 5 && dev_open[DEV_TRAFFIC]) { *value = (unsigned char)(traffic_value >> 8); return 1; }
        if (port == 7 && dev_open[DEV_STEPPER]) { *value = stepper_phase; return 1; }
        if (port == 125 && dev_open[DEV_THERMO]) { *value = (unsigned char)thermo_temp; return 1; }
        if (port == 10 && dev_open[DEV_ROBOT]) { *value = robot_data; return 1; }
        if (port == 11 && dev_open[DEV_ROBOT]) { *value = robot_status; return 1; }
        if (port == 110 && dev_open[DEV_SIMPLE]) { *value = simple_in_byte; return 1; }
        if (port == 112 && dev_open[DEV_SIMPLE]) { *value = (unsigned char)(simple_in_word & 0xFF); return 1; }
        if (port == 113 && dev_open[DEV_SIMPLE]) { *value = (unsigned char)(simple_in_word >> 8); return 1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                               */

void devices_reset()
{
    led_value = 0;
    traffic_value = 0;
    stepper_phase = 0x80;              /* the motor answers "ready" */
    thermo_temp = 0;
    thermo_heater = 0;
    simple_byte = 0;
    simple_word = 0;
    printer_data = 0;
    printer.clear();
    robot_reset_map();
}

static void dev_tick(void *)
{
    int i;
    if (dev_open[DEV_THERMO]) {
        if (thermo_heater && thermo_temp < 120) thermo_temp++;
        /* The virtual room is at 0 C: the heater warms it, but switching the
         * heater off only returns it to room temperature.  This is what makes
         * the shipped thermostat example settle instead of falling forever. */
        else if (!thermo_heater && thermo_temp > 0) thermo_temp--;
    }
    if (robot_busy_ticks > 0) {
        if (--robot_busy_ticks == 0) robot_status &= (unsigned char)~0x02;
    }
    for (i = 0; i < DEV_COUNT; i++)
        if (dev_open[i] && dev_canvas[i]) dev_canvas[i]->redraw();
    Fl::repeat_timeout(0.2, dev_tick);
}

void devices_boot()
{
    robot_reset_map();
    stepper_phase = 0x80;
    io_set_hook(devices_io);
    Fl::add_timeout(0.2, dev_tick);
}

int devices_any_open()
{
    int i;
    for (i = 0; i < DEV_COUNT; i++) if (dev_open[i]) return 1;
    return 0;
}

/* emu8086's documented activation rule: a device is started when its file
 * name appears anywhere in the source, comments and string buffers included. */
void devices_scan_source(const char *src)
{
    int i;
    if (!src) return;
    for (i = 0; i < DEV_COUNT; i++) {
        const char *p = src;
        size_t n = strlen(DEV_INFO[i].menu);
        while ((p = strchr(p, DEV_INFO[i].menu[0])) != NULL) {
            if (!strncasecmp(p, DEV_INFO[i].menu, n)) {
                devices_open(i);
                break;
            }
            p++;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* menu inventory                                                          */

static std::vector<std::string> menu_names;
static int menu_scanned;

static void scan_menu()
{
    if (menu_scanned) return;
    menu_scanned = 1;
#ifdef _WIN32
    char dir[MUE_MAXP], pat[MUE_MAXP];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    mue_home_path(dir, sizeof dir, "DEVICES");
    snprintf(pat, sizeof pat, "%s\\*.exe", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            menu_names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    if (menu_names.empty()) {
        int i;
        for (i = 0; i < DEV_COUNT; i++) menu_names.push_back(DEV_INFO[i].menu);
    }
}

int devices_menu_count()
{
    scan_menu();
    return (int)menu_names.size();
}

const char *devices_menu_name(int i)
{
    scan_menu();
    if (i < 0 || i >= (int)menu_names.size()) return 0;
    return menu_names[(size_t)i].c_str();
}
