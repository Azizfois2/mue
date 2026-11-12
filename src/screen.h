#ifndef MUE_SCREEN_H
#define MUE_SCREEN_H
#include <stdint.h>

/* The text screen, as a front end sees it.
 *
 * emu8086's `frmScreen` is a view onto whatever the guest has written into
 * video memory.  The clone's VRAM lives inside dos.c and was entirely
 * private, so nothing outside could draw it.  These accessors are the whole
 * of the surface a GUI needs: no GUI code touches VRAM directly, and the
 * terminal path keeps using the same buffer, so the two cannot disagree.
 *
 * This is deliberately a READ-ONLY window onto state that INT 10h owns.  A
 * front end must never write here - the guest is the only writer, exactly as
 * on real hardware.  Anything else would let the GUI and the emulator drift.
 */

#define SCREEN_COLS 80
#define SCREEN_ROWS 25
#define SCREEN_PAGES 8

/* Which page the guest is currently displaying (INT 10h AH=05h sets it). */
int  screen_active_page(void);

/* Cursor position on a page, for drawing the caret. */
void screen_cursor(int page, int *row, int *col);

/* One character cell: returns the character and stores the attribute
 * (foreground in the low nibble, background in the high nibble, bit 7 =
 * blink) in *attr if attr is not NULL.  Out-of-range indices return ' '. */
unsigned char screen_cell(int page, int row, int col, unsigned char *attr);

/* True once the guest has used INT 10h at all.  A front end uses this to
 * decide whether to paint a screen region or leave it empty: before the first
 * video call there is no meaningful display. */
int screen_touched(void);

/* Video mode the guest selected (INT 10h AH=00h): 3 = text 80x25, 13h = the
 * 320x200 graphics mode.  Text rendering is defined only for the text modes;
 * this lets a front end say so rather than paint garbage. */
int screen_mode(void);

#endif
