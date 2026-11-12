/* screen.c - read-only accessors onto the guest's text video memory.
 *
 * See screen.h.  Nothing here allocates or mutates: these are pure reads of
 * the VRAM that dos.c's INT 10h implementation owns.
 */
#include "screen.h"
#include "dos.h"

int screen_active_page(void)
{
    return dos_screen_page();
}

void screen_cursor(int page, int *row, int *col)
{
    int r = 0, c = 0;
    dos_screen_cursor(page, &r, &c);
    if (row) *row = r;
    if (col) *col = c;
}

unsigned char screen_cell(int page, int row, int col, unsigned char *attr)
{
    return dos_screen_cell(page, row, col, attr);
}

int screen_touched(void)
{
    return dos_screen_touched();
}

int screen_mode(void)
{
    return dos_screen_mode();
}
