/* mue_icons.h - toolbar icons drawn in code (see mue_icons.c). */
#ifndef MUE_ICONS_H
#define MUE_ICONS_H

#include <FL/Fl_Image.H>
#include <FL/Fl_Button.H>

Fl_RGB_Image *icon_new(void);
Fl_RGB_Image *icon_open(void);
Fl_RGB_Image *icon_star(void);
Fl_RGB_Image *icon_save(void);
Fl_RGB_Image *icon_compile(void);
Fl_RGB_Image *icon_run(void);
Fl_RGB_Image *icon_calc(void);
Fl_RGB_Image *icon_convert(void);
Fl_RGB_Image *icon_options(void);
Fl_RGB_Image *icon_help(void);
Fl_RGB_Image *icon_about(void);
Fl_RGB_Image *icon_reload(void);
Fl_RGB_Image *icon_step_back(void);
Fl_RGB_Image *icon_step(void);
Fl_RGB_Image *icon_ffwd(void);

/* A desaturated, lightened copy, for disabled buttons. */
Fl_RGB_Image *icon_grayed(Fl_RGB_Image *src);

/* Button with the icon above the label, thin bevel. */
Fl_Button *make_tool(int x, int y, int w, int h, const char *label,
                     Fl_Image *icon);

#endif
