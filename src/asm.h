/* mue — assembler: shared declarations
 * asm.h
 *
 * Mirrors the original's two-pass structure (FINDINGS sec. 7): a scan pass
 * that fixes label addresses, then an emit pass that produces bytes.
 */
#ifndef MUE_ASM_H
#define MUE_ASM_H

#include <stdint.h>
#include <stdio.h>

#define ASM_MAX_LABELS 4096
#define ASM_MAX_LINE   1024
#define ASM_MAX_TOK    32

typedef struct {
    char     name[64];
    uint16_t value;
    int      size;          /* bytes this label stands for (0 = unknown) */
    int      is_proc;
    int      seg;           /* LCODE / LDATA / LEXT */
    int      pass;
    int      saw_org;      /* an ORG directive was seen      */
    int      saw_code;     /* ".code" already aligned the ip   */          /* pass in which it was defined */
} label_t;

enum { LCODE = 0, LDATA = 1, LEXT = 2 };

#define ASM_MAX_MACROS 64
#define ASM_MAX_MPARAM 32   /* macro params; also LOCAL names per expansion */
#define ASM_MAX_MLINES 256  /* emu8086.inc's DEFINE_SCAN_NUM is 124 lines */

typedef struct {
    char name[64];
    char param[ASM_MAX_MPARAM][32];
    int  nparam;
    char body[ASM_MAX_MLINES][ASM_MAX_LINE];
    int  nlines;
} macro_t;

typedef struct {
    label_t lab[ASM_MAX_LABELS];
    int     nlab;

    uint8_t *out;
    int      outlen;
    int      outcap;

    int      org;           /* ORG value (0x100 for .COM) */
    int      base;          /* load base added to labels (== org) */
    int      ip;            /* location counter */
    int      pass;
    int      sizing;        /* non-zero while measuring: labels may move */
    int      saw_org;      /* an ORG directive was seen      */
    int      saw_code;     /* ".code" already aligned the ip   */
    int      saw_model;    /* a .model directive was seen (EXE target) */
    int      is_boot;      /* `#make_boot#` seen: load at 0000:7C00   */
    int      stack_size;   /* bytes reserved by `.stack N` (0 = none) */
    int      reloc_off;    /* offset of the `mov ax,@data` immediate  */
    int      code_start;   /* outlen when .code began (EXE layout)    */
    int      errcount;

    char     err[512];
    int      errline;

    /* statement currently being assembled */
    char     line[ASM_MAX_LINE];
    char     last_label[64];
    char    *tok[ASM_MAX_TOK];
    int      ntok;

    macro_t  mac[ASM_MAX_MACROS];
    int      nmac;
    int      in_macro;      /* collecting a macro body */
} asm_t;

/* diagnostics */
void asm_error(asm_t *a, const char *fmt, ...);
void asm_line_error(asm_t *a, int lineno, const char *fmt, ...);

/* emission */
void emit8 (asm_t *a, uint8_t  v);
void emit16(asm_t *a, uint16_t v);
void emit_bytes(asm_t *a, const uint8_t *p, int n);

/* labels */
label_t *find_label(asm_t *a, const char *name);
int      add_label(asm_t *a, const char *name, uint16_t value, int size, int is_proc);

/* lexing / expression */
int  split_operands(char *s, char **out, int max);
int  eval_expr(asm_t *a, const char *s, int *ok);

/* main entries */
int  asm_assemble_file(const char *path, int want_com, uint8_t **out, int *outlen,
                       char *errbuf, int errbufsz);

/* 1 if the last asm_assemble_file() source declared `.model`, i.e. it targets
 * an MZ EXE rather than a flat .COM image. */
int  asm_last_was_exe(void);
int  asm_last_is_boot(void);   /* source carried `#make_boot#` */

/* Layout facts about the most recent assembly, for the MZ writer. */
int  asm_last_stack_size(void);   /* bytes reserved by `.stack N`          */
int  asm_last_reloc_off(void);    /* file offset of the @data immediate    */
int  asm_last_code_start(void);   /* payload offset where code begins      */

#endif
