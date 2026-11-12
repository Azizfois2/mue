/* mue_tools.h - the three floating tool windows. */
#ifndef MUE_TOOLS_H
#define MUE_TOOLS_H

#include <FL/Fl_Widget.H>

void tools_open_options(void);
void tools_open_convertor(void);
void tools_open_calculator(void);

/* Close handler for the options window's `close` button. */
void tools_close_options_cb(Fl_Widget *, void *);

#endif
