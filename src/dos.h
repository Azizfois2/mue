#ifndef MUE_DOS_H
#define MUE_DOS_H
#include <stdio.h>

extern int dos_exit_requested;
extern int dos_exit_code;

int  dos_interrupt(int n);      /* 1 = handled, 0 = forward to guest IVT */
void dos_putc(int ch);
void dos_puts(const char *s);
void dos_flush(FILE *f);
void dos_push_key(int ch);
void dos_boot_psp(void);        /* PSP:0x16 and the job file table of the
                                 * program main.c just loaded */
void dos_feed_stdin(const char *text);
#endif
