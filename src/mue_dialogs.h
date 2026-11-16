/* mue_dialogs.h - the floating windows of emu8086, rebuilt.
 *
 * The measured sizes and contents are in re/UI_SPEC.md section 8. These are
 * separate windows, not panels: emu8086 opens each with its own menu or
 * bottom button.
 */
#ifndef MUE_DIALOGS_H
#define MUE_DIALOGS_H

#include <FL/Fl_Widget.H>

/* Called after every step/refresh so open dialogs show live state. */
void dlg_refresh(void);

/* Toolbar / menu entry points. */
void cb_open_calc(Fl_Widget *, void *);
void cb_open_conv(Fl_Widget *, void *);
void cb_open_opts(Fl_Widget *, void *);

void cb_ascii_codes(Fl_Widget *, void *);
void cb_about(Fl_Widget *, void *);
void cb_ram(Fl_Widget *, void *);
void cb_alu(Fl_Widget *, void *);
void cb_fpu(Fl_Widget *, void *);
void cb_stop_condition(Fl_Widget *, void *);
void cb_symbol_table(Fl_Widget *, void *);
void cb_listing(Fl_Widget *, void *);
void cb_extended_value(Fl_Widget *, void *);
void cb_lexical_flags(Fl_Widget *, void *);

/* Windows opened from the emulator's bottom row. */
void dlg_flags(void);
void dlg_screen(void);
void dlg_assembler_status(const char *message);

/* The assembler status window's `run` button: mue_ui supplies the callback
 * that loads the built image in the emulator. */
void dlg_status_on_run(void (*cb)(void));

/* emu8086 greys `run` out until the Save-As step has created the image. */
void dlg_status_run_enabled(int on);

/* The text shown by the listing/source views; set by the UI on load. */
void dlg_set_source(const char *src, const char *path);

#endif
