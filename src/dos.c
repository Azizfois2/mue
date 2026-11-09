/* mue — DOS and BIOS interrupt services
 * dos.c
 *
 * The original routes software interrupts through dedicated VB methods
 * (FINDINGS sec. 8).  Here they are ordinary C handlers that inspect the
 * register file and either service the call or forward it to the guest IVT.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "alu.h"
#include "dos.h"

static char outbuf[1 << 16];
static int  outlen;
int  dos_exit_requested;
int  dos_exit_code;

/* ---- keyboard queue -------------------------------------------------- */
static unsigned char kb[256];
static int kb_head, kb_tail;
static const char *scripted = NULL;      /* redirected stdin text */
static size_t scripted_pos;

void dos_push_key(int ch)
{
    int nt = (kb_tail + 1) & 0xFF;
    if (nt == kb_head) return;
    kb[kb_tail] = (unsigned char)ch;
    kb_tail = nt;
}

void dos_feed_stdin(const char *text)
{
    scripted = text;
    scripted_pos = 0;
    while (*text) { dos_push_key(*text); text++; }
}

static int kb_pop(void)
{
    int v;
    if (kb_head == kb_tail) return -1;
    v = kb[kb_head];
    kb_head = (kb_head + 1) & 0xFF;
    return v;
}

/* ---- screen ---------------------------------------------------------- */
void dos_putc(int ch)
{
    if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = (char)ch;
    if (ch == '\n' && outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '\r';
}

void dos_puts(const char *s) { while (*s) dos_putc((unsigned char)*s++); }

void dos_flush(FILE *f)
{
    if (outlen) { fwrite(outbuf, 1, (size_t)outlen, f); outlen = 0; }
}

/* ---- helpers --------------------------------------------------------- */
static void setAX(int v) { cpu.r[0] = (uint16_t)v; }
static void setAXhi(int v) { cpu.r[0] = (uint16_t)((cpu.r[0] & 0x00FF) | ((v & 0xFF) << 8)); }
static void setAThi(int v) { cpu.r[0] = (uint16_t)((cpu.r[0] & 0xFF00) | (v & 0xFF)); }

/* Build a $-terminated string starting at DS:DX and print it. */
static void print_dollar_string(void)
{
    uint32_t p = seg_off_to_phys(cpu.sr[DS], cpu.r[2]);
    int guard = 0;
    while (peek8(p) != '$' && guard++ < 65535) {
        dos_putc(peek8(p));
        p++;
    }
}

/* Read a line from the keyboard queue into DS:DX (DOS 0Ah). */
static void read_buffered(uint16_t buf_off)
{
    uint32_t p = seg_off_to_phys(cpu.sr[DS], buf_off);
    int maxlen = peek8(p);
    int i = 0;
    (void)maxlen;

    for (;;) {
        int c = kb_pop();
        if (c < 0) break;
        if (c == '\r' || c == '\n') { dos_putc('\n'); break; }
        if (c == 8) { if (i > 0) { i--; dos_puts("\b \b"); } continue; }
        poke8(p + 2 + (uint32_t)i, (uint8_t)c);
        i++;
        dos_putc(c);
        if (i > 250) break;
    }
    poke8(p + 1, (uint8_t)i);
    poke8(p + 2 + (uint32_t)i, '\r');
}

/* ---- INT 21h --------------------------------------------------------- */
static void int21(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;

    switch (ah) {
    case 0x00:                                  /* terminate */
        dos_exit_requested = 1;
        dos_exit_code = 0;
        cpu.halted = 1;
        break;

    case 0x01: {                                /* read char with echo */
        int c = kb_pop();
        if (c < 0) c = 0x1A;
        dos_putc(c);
        setAThi(c);
        break; }

    case 0x02:                                  /* display output */
        if ((cpu.r[2] & 0xFF) == 9) dos_putc('\t');
        else dos_putc(cpu.r[2] & 0xFF);
        break;

    case 0x06: {                                /* direct console I/O */
        int dl = cpu.r[2] & 0xFF;
        if (dl == 0xFF) {
            int c = kb_pop();
            setAThi(c < 0 ? 0 : c);
            if (c >= 0 && !(cpu.flags & F_ZF)) { cpu.flags &= ~F_ZF; }
        } else {
            dos_putc(dl);
        }
        break; }

    case 0x07: case 0x08: {                     /* read char, no echo */
        int c = kb_pop();
        if (c < 0) c = 0x1A;
        setAThi(c);
        break; }

    case 0x09:                                  /* print $-string */
        print_dollar_string();
        break;

    case 0x0A:                                  /* buffered input */
        read_buffered(cpu.r[2]);
        break;

    case 0x0B:                                  /* check input status */
        setAThi(kb_head != kb_tail ? 0xFF : 0x00);
        break;

    case 0x19:                                  /* current drive */
        setAThi(2);                             /* C: */
        break;

    case 0x1A:                                  /* set DTA */
        break;

    case 0x25:                                  /* set interrupt vector */
        {
            uint32_t v = (uint32_t)(cpu.r[0] & 0xFF) * 4;
            poke16(v, cpu.r[2]);
            poke16(v + 2, cpu.sr[DS]);
        }
        break;

    case 0x2A:                                  /* get date */
        cpu.r[1] = 2024;
        cpu.r[2] = 0x0101;
        setAThi(1);
        break;

    case 0x2C:                                  /* get time */
        cpu.r[1] = 0x1200;
        cpu.r[2] = 0x0000;
        break;

    case 0x2F:                                  /* get DTA */
        cpu.sr[ES] = cpu.sr[DS];
        cpu.r[3] = 0x0080;
        break;

    case 0x30:                                  /* DOS version */
        cpu.r[0] = 0x0005;                     /* MS-DOS 5.0 */
        cpu.r[3] = 0;
        cpu.r[1] = 0;
        break;

    case 0x35:                                  /* get interrupt vector */
        {
            uint32_t v = (uint32_t)(cpu.r[0] & 0xFF) * 4;
            cpu.r[3] = peek16(v);
            cpu.sr[ES] = peek16(v + 2);
        }
        break;

    case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42:
        /* file services: the original implements a vdrive; report failure */
        setAX(0x0000);
        cpu.flags |= F_CF;
        break;

    case 0x4C:                                  /* exit with code */
        dos_exit_requested = 1;
        dos_exit_code = cpu.r[0] & 0xFF;
        cpu.halted = 1;
        break;

    case 0x4D:                                  /* get return code */
        setAX(0);
        break;

    case 0x4E: case 0x4F:                       /* find first / next */
        setAX(0x0012);                          /* no more files */
        cpu.flags |= F_CF;
        break;

    default:
        break;
    }
}

/* ---- INT 10h (video) ------------------------------------------------- */
/* The clone renders through dos_putc, so the text cursor is bookkeeping
 * only.  It has to exist because AH=02/AH=03 expose it and AH=13h writes
 * relative to it and advances it (FINDINGS 5.2).  Only mode 3 is reported
 * (AH=0F below), i.e. 80x25.
 *
 * Behind it is a char+attribute cell buffer, because AH=08/09/0A read and
 * write cells rather than print, and a character attribute has nowhere else
 * to live on a stream console.  Everything that reaches the screen also
 * updates its cell, so AH=08 reports what AH=09/0A/0E/13h put there. */
#define VIDEO_COLS 80
#define VIDEO_ROWS 25
static int cur_row, cur_col;

/* CGA order: even byte = character, odd byte = attribute. */
static unsigned char vram[VIDEO_COLS * VIDEO_ROWS * 2];
static int vram_ready;

static void vram_init(void)
{
    int i;
    if (vram_ready) return;
    vram_ready = 1;
    for (i = 0; i < VIDEO_COLS * VIDEO_ROWS; i++) {
        vram[i * 2]     = ' ';              /* mode 3 clears to blank/0x07 */
        vram[i * 2 + 1] = 0x07;
    }
}

/* Cell at row/col.  A guest can set the cursor out of range through AH=02, so
 * clamp rather than wander off the array. */
static unsigned char *vram_cell(int row, int col)
{
    vram_init();
    if (row < 0) row = 0; else if (row >= VIDEO_ROWS) row = VIDEO_ROWS - 1;
    if (col < 0) col = 0; else if (col >= VIDEO_COLS) col = VIDEO_COLS - 1;
    return &vram[(row * VIDEO_COLS + col) * 2];
}

/* AH=06/07: blank `lines` rows of the window (0 = all of it), shifting the
 * rest up for AH=06 and down for AH=07. */
static void vram_scroll(int top, int left, int bot, int right, int lines,
                        int attr, int up)
{
    int r, c, t;
    vram_init();
    if (top > bot) { t = top; top = bot; bot = t; }
    if (left > right) { t = left; left = right; right = t; }
    if (top < 0) top = 0;
    if (bot >= VIDEO_ROWS) bot = VIDEO_ROWS - 1;
    if (left < 0) left = 0;
    if (right >= VIDEO_COLS) right = VIDEO_COLS - 1;
    if (lines <= 0 || lines > bot - top + 1) lines = bot - top + 1;
    for (r = 0; r <= bot - top; r++) {
        int dst = up ? top + r : bot - r;
        int src = up ? top + r + lines : bot - r - lines;
        for (c = left; c <= right; c++) {
            unsigned char *d = vram_cell(dst, c);
            if (src >= top && src <= bot) {
                unsigned char *s = vram_cell(src, c);
                d[0] = s[0];
                d[1] = s[1];
            } else {
                d[0] = ' ';
                d[1] = (unsigned char)attr;
            }
        }
    }
}

/* Teletype output, shared with AH=13h (which prints its cells the same way). */
static void video_putc(int c)
{
    if (c == 0x0D) dos_putc('\r');
    else if (c == 0x0A) dos_putc('\n');
    else if (c == 0x08) dos_putc('\b');
    else dos_putc(c);
}

/* Move the modelled cursor n columns, wrapping at the mode-3 width. */
static void cursor_advance_cols(int n)
{
    cur_col += n;
    while (cur_col >= VIDEO_COLS) {
        cur_col -= VIDEO_COLS;
        cur_row  = (cur_row + 1) % VIDEO_ROWS;
    }
}

/* AH=13h size guard.  The original logs "INT 10h, AH=13h - " + message and
 * then halts through [obj+0x7AC], setting the 0x62141C error flag
 * (0x52301E-0x523079).  Same outcome here: diagnostic, then stop. */
static void int10_write_string_fail(const char *why)
{
    fprintf(stderr, "INT 10h, AH=13h - %s\n", why);
    cpu.halted = 1;
    cpu.exit_code = -5;                 /* BIOS call rejected */
}

static void int10(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;

    switch (ah) {
    case 0x00:                                  /* set video mode */
        break;
    case 0x01:                                  /* set cursor shape */
        break;
    case 0x02:                                  /* set cursor position */
        cur_row = (cpu.r[DX] >> 8) & 0xFF;
        cur_col = cpu.r[DX] & 0xFF;
        break;
    case 0x03:                                  /* read cursor position */
        cpu.r[CX] = 0x0607;
        cpu.r[DX] = (uint16_t)((cur_row << 8) | cur_col);
        break;
    case 0x06: case 0x07:                       /* scroll up / down */
        {
            int lines = cpu.r[AX] & 0xFF;
            int attr  = (cpu.r[BX] >> 8) & 0xFF;
            int top   = (cpu.r[CX] >> 8) & 0xFF, left  = cpu.r[CX] & 0xFF;
            int bot   = (cpu.r[DX] >> 8) & 0xFF, right = cpu.r[DX] & 0xFF;

            vram_scroll(top, left, bot, right, lines, attr, ah == 0x06);
            if (top == 0 && left == 0 && bot >= VIDEO_ROWS - 1 &&
                right >= VIDEO_COLS - 1)
                dos_putc('\n');         /* a scroll looks like a line here */
        }
        break;
    case 0x08:                                  /* read char/attr at cursor */
        {
            unsigned char *cell = vram_cell(cur_row, cur_col);
            cpu.r[AX] = (uint16_t)((cell[1] << 8) | cell[0]);
        }
        break;
    case 0x09: case 0x0A:                       /* write char (+attr) */
        {
            int ch   = cpu.r[AX] & 0xFF;
            int attr = cpu.r[BX] & 0xFF;        /* BL */
            int n    = cpu.r[CX];
            int i;

            /* In text mode AH=0A writes the character only: the cell keeps
             * the attribute it already had, and BL is ignored. */
            for (i = 0; i < n && cur_col + i < VIDEO_COLS; i++) {
                unsigned char *cell = vram_cell(cur_row, cur_col + i);
                cell[0] = (unsigned char)ch;
                if (ah == 0x09) cell[1] = (unsigned char)attr;
                video_putc(ch);
            }
        }
        break;
    case 0x0E:                                  /* teletype output */
        {
            int c = cpu.r[AX] & 0xFF;
            if (c >= 0x20) {                    /* teletype draws, attr 0x07 */
                unsigned char *cell = vram_cell(cur_row, cur_col);
                cell[0] = (unsigned char)c;
                cell[1] = 0x07;
            }
            video_putc(c);
            /* Teletype moves the cursor: CR to column 0, LF down a row, BS
             * back one column, anything else one column forward. */
            if (c == 0x0D)                        cur_col = 0;
            else if (c == 0x0A)                   cur_row = (cur_row + 1) % VIDEO_ROWS;
            else if (c == 0x08) { if (cur_col > 0) cur_col--; }
            else                                  cursor_advance_cols(1);
        }
        break;
    case 0x0F:                                  /* get video mode */
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0x00FF) | (0x03 << 8));
        cpu.r[3] = 0;
        break;
    case 0x13:                                  /* write string at ES:BP */
        {
            int al      = cpu.r[AX] & 0xFF;
            int cells   = cpu.r[CX];            /* characters requested */
            int pairs   = (al & 0x02) != 0;     /* cell = char + attr byte */
            int advance = (al & 0x01) != 0;     /* move cursor afterwards */
            uint32_t p  = seg_off_to_phys(cpu.sr[ES], cpu.r[BP]);
            int i, ch, written = 0;

            /* DX is the starting position, as on the real BIOS. */
            cur_row = (cpu.r[DX] >> 8) & 0xFF;
            cur_col = cpu.r[DX] & 0xFF;

            if (cells > 0x7D0) {                /* FINDINGS 5.2 guard */
                int10_write_string_fail(
                    "CX value (string size) is above the allowed limit!");
                break;
            }
            for (i = 0; i < cells; i++) {
                int attr = 0x07;
                unsigned char *cell;
                ch = peek8(p++);
                /* With bit1 set every second byte is an attribute: consume it
                 * and give it to the cell instead of printing it. */
                if (pairs) attr = peek8(p++);
                cell = vram_cell(cur_row, cur_col + written);
                cell[0] = (unsigned char)ch;
                cell[1] = (unsigned char)attr;
                video_putc(ch);
                written++;
            }
            if (advance) cursor_advance_cols(written);
        }
        break;
    default:
        break;
    }
}

/* ---- INT 16h (keyboard) --------------------------------------------- */
static void int16(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;
    switch (ah) {
    case 0x00: case 0x10: {
        int c = kb_pop();
        if (c < 0) c = 0;
        cpu.r[0] = (uint16_t)((c << 8) | 0);
        cpu.flags &= ~F_ZF;
        break; }
    case 0x01: case 0x11:
        if (kb_head != kb_tail) { cpu.flags &= ~F_ZF;
            cpu.r[0] = (uint16_t)((kb[kb_head] << 8) | 0); }
        else { cpu.flags |= F_ZF; cpu.r[0] = 0; }
        break;
    default: break;
    }
}

/* ---- INT 13h (floppy disk) -------------------------------------------- */
/* Original (FINDINGS 5.3, 15.5): AH=00 resets; AH=02/03 build "FLOPPY_" &
 * DL, open it with a 512-byte record length, Get/Put record
 * C*36 + H*18 + (S-1) + i for each sector, and close - all inside the one
 * call.  The clone does the same, so a guest write reaches the image file
 * before the interrupt returns. */
#define FLOPPY_TRACKS 80
#define FLOPPY_HEADS   2
#define FLOPPY_SPT    18
#define FLOPPY_BPS   512
#define FLOPPY_SECTORS (FLOPPY_TRACKS * FLOPPY_HEADS * FLOPPY_SPT)

/* Open FLOPPY_<drive>, the name the original builds from DL.  MUE_FLOPPY
 * overrides drive 0 so the regressions can drive a scratch copy. */
static FILE *floppy_open(int drive, const char *mode)
{
    FILE *f;
    char buf[64];

    if (drive == 0) {
        const char *env = getenv("MUE_FLOPPY");
        if (env && env[0] && (f = fopen(env, mode)) != NULL) return f;
    }
    snprintf(buf, sizeof buf, "C:/emu8086/FLOPPY_%d", drive);
    if ((f = fopen(buf, mode)) != NULL) return f;
    if (drive == 0) {
        const char *env = getenv("EMUB086_FLOPPY");
        if (env && env[0] && (f = fopen(env, mode)) != NULL) return f;
    }
    snprintf(buf, sizeof buf, "FLOPPY_%d", drive);   /* relative, as original */
    return fopen(buf, mode);
}
static void int13(void)
{
    int ah = (cpu.r[AX] >> 8) & 0xFF;
    int al = cpu.r[AX] & 0xFF;
    int ch = (cpu.r[CX] >> 8) & 0xFF;
    int cl = cpu.r[CX] & 0xFF;
    int dh = (cpu.r[DX] >> 8) & 0xFF;
    int dl = cpu.r[DX] & 0xFF;
    uint32_t p;
    long cyl, head, sec, linear, i;
    switch (ah) {
    case 0x00:                                  /* reset: success */
        cpu.flags &= ~F_CF;
        setAThi(0);
        return;
    case 0x02: case 0x03: {                      /* read/write sectors */
        unsigned char buf[FLOPPY_BPS];
        FILE *f;
        if (al == 0) { cpu.flags &= ~F_CF; setAThi(0); return; }
        cyl = ch; head = dh; sec = cl & 0x3F;
        if (sec == 0 || head >= FLOPPY_HEADS || cyl >= FLOPPY_TRACKS) {
            cpu.flags |= F_CF; setAThi(1); return;
        }
        linear = (cyl * FLOPPY_HEADS + head) * FLOPPY_SPT + (sec - 1);
        if (linear < 0 || linear + al > FLOPPY_SECTORS) {
            cpu.flags |= F_CF; setAThi(1); return;
        }
        p = seg_off_to_phys(cpu.sr[ES], cpu.r[BX]);
        if (p + (uint32_t)al * FLOPPY_BPS > (uint32_t)0x100000) {
            cpu.flags |= F_CF; setAThi(1); return;
        }
        /* One open per call, both directions: writes must exist to be
         * written, reads zero-fill a short image. */
        f = floppy_open(dl, ah == 0x02 ? "rb" : "r+b");
        if (!f) { cpu.flags |= F_CF; setAThi(0xAA); return; }
        for (i = 0; i < al; i++) {
            long off = (long)(linear + i) * FLOPPY_BPS;
            uint32_t mem = p + (uint32_t)i * FLOPPY_BPS;
            int k;
            if (fseek(f, off, SEEK_SET) != 0) break;
            if (ah == 0x02) {
                size_t n = fread(buf, 1, FLOPPY_BPS, f);
                for (; n < FLOPPY_BPS; n++) buf[n] = 0;
                for (k = 0; k < FLOPPY_BPS; k++) poke8(mem + (uint32_t)k, buf[k]);
            } else {
                for (k = 0; k < FLOPPY_BPS; k++) buf[k] = peek8(mem + (uint32_t)k);
                if (fwrite(buf, 1, FLOPPY_BPS, f) != FLOPPY_BPS) break;
            }
        }
        fclose(f);
        if (i < al) { cpu.flags |= F_CF; setAThi(1); return; }
        cpu.flags &= ~F_CF;
        cpu.r[AX] = (uint16_t)(al & 0xFF);      /* AH=0, AL=sectors done */
        return; }
    default:
        cpu.flags |= F_CF;                      /* cf. fail path 5.3 */
        setAThi(1);
        return;
    }
}

/* ---- INT 1Ah (time) -------------------------------------------------- */
static void int1a(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;
    switch (ah) {
    case 0x00:
        cpu.r[1] = 0x1234;
        cpu.r[2] = 0x5678;
        setAThi(0);
        break;
    case 0x02:
        cpu.r[1] = 0x2024;
        cpu.r[2] = 0x0101;
        setAThi(1);
        break;
    default:
        break;
    }
}

/* ---- dispatcher ------------------------------------------------------ */
/* Returns 1 if the interrupt was serviced here, 0 to forward to the IVT. */
int dos_interrupt(int n)
{
    switch (n) {
    case 0x00: return 0;                /* divide error: let the guest see it */
    case 0x10: int10(); return 1;
    case 0x11: cpu.r[0] = 0x0001; return 1;   /* equipment list */
    case 0x12: cpu.r[0] = 0x0280; return 1;   /* conventional memory size */
    case 0x16: int16(); return 1;
    case 0x17: setAThi(0x90); return 1;        /* printer not ready */
    case 0x1A: int1a(); return 1;
    case 0x19: return 0;                /* reboot: unsupported */
    case 0x20:                          /* terminate */
        dos_exit_requested = 1;
        cpu.halted = 1;
        return 1;
    case 0x21: int21(); return 1;
    case 0x13: int13(); return 1;    /* floppy: FLOPPY_0 image */
    case 0x23: break;
    case 0x24: return 1;                /* critical error handler */
    case 0x28: return 1;                /* DOS idle */
    case 0x33: cpu.r[2] = 0; return 1; /* mouse reset */
    case 0x03:                          /* debugger breakpoint */
    case 0x04:                          /* overflow */
        return 0;
    default:
        break;
    }
    return 0;
}
