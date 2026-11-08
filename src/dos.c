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
static void int10(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;

    switch (ah) {
    case 0x00:                                  /* set video mode */
        break;
    case 0x01:                                  /* set cursor shape */
        break;
    case 0x02:                                  /* set cursor position */
        break;
    case 0x03:                                  /* read cursor position */
        cpu.r[1] = 0x0607;
        cpu.r[2] = 0x0000;
        break;
    case 0x06: case 0x07:                       /* scroll */
        if ((cpu.r[1] & 0xFF) == 0 && (cpu.r[1] >> 8) == 0 &&
            (cpu.r[2] & 0xFF) == 79 && (cpu.r[2] >> 8) == 24) {
            dos_putc('\n');
        }
        break;
    case 0x08:                                  /* read char/attr at cursor */
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0xFF00) | ' ');
        break;
    case 0x09: case 0x0A:                       /* write char/attr */
        {
            int n = cpu.r[1] & 0xFF;
            if (n <= 0) n = 1;
            while (n--) dos_putc(cpu.r[0] & 0xFF);
        }
        break;
    case 0x0E:                                  /* teletype output */
        {
            int c = cpu.r[0] & 0xFF;
            if (c == 0x0D) dos_putc('\r');
            else if (c == 0x0A) dos_putc('\n');
            else if (c == 0x08) dos_putc('\b');
            else dos_putc(c);
        }
        break;
    case 0x0F:                                  /* get video mode */
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0x00FF) | (0x03 << 8));
        cpu.r[3] = 0;
        break;
    case 0x13:                                  /* write string */
        {
            uint32_t p = seg_off_to_phys(cpu.sr[ES], cpu.r[5]);
            int n = cpu.r[1];
            while (n--) { dos_putc(peek8(p)); p++; }
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
