/* mue — DOS and BIOS interrupt services
 * dos.c
 *
 * The original routes software interrupts through dedicated VB methods
 * (FINDINGS sec. 8).  Here they are ordinary C handlers that inspect the
 * register file and either service the call or forward it to the guest IVT.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define DOS_IREAD   _S_IREAD
#define DOS_IWRITE  _S_IWRITE
#define dos_chmod   _chmod
#else
#define DOS_IREAD   S_IRUSR
#define DOS_IWRITE  S_IWUSR
#define dos_chmod   chmod
#endif
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

/* One byte from the host, so `mue prog.asm < input` works.
 *
 * On a terminal this waits for a key: the console calls are supposed to block
 * until something is typed, not report "no key" the instant the buffer is
 * empty.  A redirected stream returns EOF once exhausted, so a guest run from
 * a file or a pipe is never left waiting. */
static int host_stdin_byte(void)
{
    static int binary;
    int c;

#ifdef _WIN32
    /* Text mode would eat the CR before the guest ever sees it. */
    if (!binary) { _setmode(_fileno(stdin), _O_BINARY); binary = 1; }
#endif
    c = fgetc(stdin);
    return c == EOF ? -1 : c;
}

/* Queue one host byte when the buffer is empty.  Returns 0 at end of input,
 * which only a redirected stdin can reach. */
static int kb_refill(void)
{
    int c;
    if (kb_head != kb_tail) return 1;
    c = host_stdin_byte();
    if (c < 0) return 0;
    dos_push_key(c);
    return kb_head != kb_tail;
}

/* Pop a key, waiting for one when nothing is buffered. */
static int kb_pop(void)
{
    int v;
    if (!kb_refill()) return -1;
    v = kb[kb_head];
    kb_head = (kb_head + 1) & 0xFF;
    return v;
}

/* Look at the next key without consuming it, waiting for one first. */
static int kb_peek(void)
{
    if (!kb_refill()) return -1;
    return kb[kb_head];
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

/* ---- INT 21h file handles -------------------------------------------- */
/* The original routes 3C/3D into 0x5DDF80, 3E into 0x5DE530 and 42 into
 * 0x5DED70, which are ordinary opens on host files and a DOS file table
 * (FINDINGS 5.1).  The clone keeps the same guest-visible numbering: handles
 * start at 5, because 0-4 are the character devices, and CF=1 carries a DOS
 * error code in AX. */
#define DOS_FILES        16
#define DOS_FIRST_HANDLE  5
static FILE *dos_files[DOS_FILES];
static int   dos_fmode[DOS_FILES];       /* 0 read-only, 1 write-only, 2 both */

static void dos_file_error(int code)
{
    cpu.flags |= F_CF;
    setAX(code);
}

/* Copy an ASCIIZ name out of guest memory.  Returns 0 on success, -1 on an
 * empty name or one longer than a DOS path can be. */
static int dos_name_at(uint16_t seg, uint16_t off, char *out, size_t n)
{
    uint32_t p = seg_off_to_phys(seg, off);
    size_t i = 0;

    for (;;) {
        int c = peek8(p + (uint32_t)i);
        if (c == 0) break;
        if (i + 1 >= n) return -1;
        out[i++] = (char)c;
    }
    out[i] = 0;
    return i == 0 ? -1 : 0;
}

static int dos_guest_name(char *out, size_t n)
{
    return dos_name_at(cpu.sr[DS], cpu.r[DX], out, n);
}

/* Open FILE for handle h, or NULL if h is not an open file. */
static FILE *dos_slot_file(int h, int *idx)
{
    int i = h - DOS_FIRST_HANDLE;
    if (h < DOS_FIRST_HANDLE || i >= DOS_FILES || !dos_files[i]) return NULL;
    if (idx) *idx = i;
    return dos_files[i];
}

static int dos_slot_alloc(void)
{
    int i;
    for (i = 0; i < DOS_FILES; i++)
        if (!dos_files[i]) return i;
    return -1;
}

/* ---- INT 21h find-first / find-next ---------------------------------- */
/* The original keeps the DTA in one flat address (0x621C48), defaulting it to
 * 0x7080 = its PSP alias 0x700:0x0080 (0x5E2532), and fills the DOS 2.x
 * layout: +0x15 attribute, +0x16 time, +0x18 date, +0x1A size, +0x1E ASCIZ
 * name (0x5E2A23-0x5E2B2C).  Same layout here, with the clone's PSP
 * (0x0100:0x0080, see main.c) as the default. */
#define DOS_NO_MORE_FILES 0x12
static uint32_t dta;
static int dta_valid;

static uint32_t dta_addr(void)
{
    if (!dta_valid) {
        dta = seg_off_to_phys(cpu.sr[DS], 0x80);   /* PSP default */
        dta_valid = 1;
    }
    return dta;
}

static char find_dir[160];
static char find_pat[12];                   /* 11-char 8.3, '?' wildcards */
static int  find_attr, find_index;

/* Expand a DOS wildcard to an 11-char 8.3 pattern.  '*' means "the rest of
 * this field", so it becomes '?' padding rather than matching across the
 * dot: `*.c` must not match `A.TXT`. */
static void make_pattern(const char *spec, char out[12])
{
    char base[9], ext[4];
    int i, f;

    memset(base, ' ', 8);
    memset(ext, ' ', 3);
    for (i = 0; i < 8 && spec[i] && spec[i] != '.'; i++) base[i] = spec[i];
    for (f = i; spec[f] && spec[f] != '.'; f++) ;
    if (spec[f] == '.')
        for (i = 0; i < 3 && spec[f + 1 + i]; i++) ext[i] = spec[f + 1 + i];
    for (i = 0; i < 8; i++) base[i] = (char)toupper((unsigned char)base[i]);
    for (i = 0; i < 3; i++) ext[i] = (char)toupper((unsigned char)ext[i]);
    for (i = 0; i < 8; i++)
        if (base[i] == '*') { for (f = i; f < 8; f++) base[f] = '?'; break; }
    for (i = 0; i < 3; i++)
        if (ext[i] == '*') { for (f = i; f < 3; f++) ext[f] = '?'; break; }
    memcpy(out, base, 8);
    memcpy(out + 8, ext, 3);
    out[11] = 0;
}

/* 8.3 form of a host name, upper case, space padded. */
static void to_83(const char *name, char out[12])
{
    int i, f;

    memset(out, ' ', 11);
    out[11] = 0;
    for (i = 0; i < 8 && name[i] && name[i] != '.'; i++)
        out[i] = (char)toupper((unsigned char)name[i]);
    for (f = i; name[f] && name[f] != '.'; f++) ;
    if (name[f] == '.')
        for (i = 0; i < 3 && name[f + 1 + i]; i++)
            out[8 + i] = (char)toupper((unsigned char)name[f + 1 + i]);
}

static int pat_match(const char *pat, const char *name)
{
    int i;
    for (i = 0; i < 11; i++)
        if (pat[i] != '?' && pat[i] != name[i]) return 0;
    return 1;
}

/* Split `dir\*.c` into directory and pattern; a bare pattern means ".". */
static void split_spec(const char *spec, char *dir, size_t dsz, char *pat)
{
    const char *slash = NULL, *p;

    for (p = spec; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) {
        strcpy(dir, ".");
        make_pattern(spec, pat);
        return;
    }
    {
        size_t n = (size_t)(slash - spec);
        if (n >= dsz) n = dsz - 1;
        memcpy(dir, spec, n);
        dir[n] = 0;
        if (n == 0) { dir[0] = '/'; dir[1] = 0; }
    }
    make_pattern(slash + 1, pat);
}

/* Fill the DTA for one match: attribute, packed time/date, size, name. */
static void dta_fill(const char *name, const struct stat *st, int attr)
{
    uint32_t d = dta_addr();
    const struct tm *tm = localtime(&st->st_mtime);
    unsigned t = 0, dt = 0;
    int i;

    if (tm) {
        t  = (unsigned)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
        dt = (unsigned)(((tm->tm_year + 1900 - 1980) << 9) |
                        ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    }
    poke8(d + 0x15, (uint8_t)attr);
    poke16(d + 0x16, (uint16_t)t);
    poke16(d + 0x18, (uint16_t)dt);
    poke16(d + 0x1A, (uint16_t)((uint32_t)st->st_size & 0xFFFF));
    poke16(d + 0x1C, (uint16_t)(((uint32_t)st->st_size >> 16) & 0xFFFF));
    for (i = 0; i < 12 && name[i]; i++)
        poke8(d + 0x1E + (uint32_t)i, (uint8_t)toupper((unsigned char)name[i]));
    poke8(d + 0x1E + (uint32_t)i, 0);
}

/* Pick the find_index-th match in find_dir.  Returns 1 and fills the DTA. */
static int find_match(void)
{
    DIR *dp = opendir(find_dir[0] ? find_dir : ".");
    struct dirent *de;
    int seen = 0, hit = 0;

    if (!dp) return 0;
    while (!hit && (de = readdir(dp)) != NULL) {
        struct stat st;
        char full[400], name83[12];
        int isdir;

        if (de->d_name[0] == '.' &&
            (de->d_name[1] == 0 ||
             (de->d_name[1] == '.' && de->d_name[2] == 0)))
            continue;
        to_83(de->d_name, name83);
        if (!pat_match(find_pat, name83)) continue;
        if (snprintf(full, sizeof full, "%s/%s", find_dir, de->d_name) >=
            (int)sizeof full)
            continue;
        if (stat(full, &st) != 0) continue;
        isdir = (st.st_mode & S_IFDIR) != 0;
        if (isdir && !(find_attr & 0x10)) continue;   /* dirs need 0x10 */
        if (!isdir && (find_attr & 0x08)) continue;   /* volume labels only */
        if (seen++ < find_index) continue;
        /* Same attribute bits AH=43 reports, so the two calls agree. */
        dta_fill(de->d_name, &st,
                 isdir ? 0x10
                       : (0x20 | ((st.st_mode & S_IWRITE) ? 0 : 0x01)));
        hit = 1;
    }
    closedir(dp);
    return hit;
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
        dta = seg_off_to_phys(cpu.sr[DS], cpu.r[DX]);
        dta_valid = 1;
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

    case 0x3C: {                                /* create file, DS:DX = name */
        char name[128];
        FILE *f;
        int i;

        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        i = dos_slot_alloc();
        if (i < 0) { dos_file_error(4); break; }
        f = fopen(name, "wb+");                 /* DOS creates and truncates */
        if (!f) { dos_file_error(5); break; }
        dos_files[i] = f;
        dos_fmode[i] = 2;
        cpu.flags &= ~F_CF;
        setAX(DOS_FIRST_HANDLE + i);
        break; }

    case 0x3D: {                                /* open file, AL = access */
        char name[128];
        FILE *f;
        int i, mode = cpu.r[AX] & 0xFF;

        if (mode > 2) { dos_file_error(1); break; }     /* invalid access */
        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        i = dos_slot_alloc();
        if (i < 0) { dos_file_error(4); break; }
        f = fopen(name, mode == 0 ? "rb" : "r+b");     /* open never creates */
        if (!f) { dos_file_error(2); break; }
        dos_files[i] = f;
        dos_fmode[i] = mode;
        cpu.flags &= ~F_CF;
        setAX(DOS_FIRST_HANDLE + i);
        break; }

    case 0x3E: {                                /* close handle BX */
        int i;
        FILE *f = dos_slot_file(cpu.r[BX], &i);

        if (!f) { dos_file_error(6); break; }
        fclose(f);
        dos_files[i] = NULL;
        cpu.flags &= ~F_CF;
        break; }

    case 0x3F: {                                /* read BX -> DS:DX, CX bytes */
        unsigned char buf[512];
        uint32_t p, n, done = 0;
        int i, h = cpu.r[BX];
        FILE *f;

        /* Handle 0 is the console: DOS stops at CR and keeps it in the
         * buffer, and echoes what it read. */
        if (h == 0) {
            p = seg_off_to_phys(cpu.sr[DS], cpu.r[DX]);
            n = cpu.r[CX];
            if (p + n > (uint32_t)0x100000) { dos_file_error(5); break; }
            while (done < n) {
                int c = kb_pop();
                if (c < 0) break;
                poke8(p + done, (uint8_t)c);
                done++;
                if (c == 0x0D) { dos_putc('\n'); break; }
                dos_putc(c);
            }
            cpu.flags &= ~F_CF;
            setAX((int)done);               /* 0 = end of input */
            break;
        }
        f = dos_slot_file(h, &i);
        if (!f) { dos_file_error(6); break; }
        if (dos_fmode[i] == 1) { dos_file_error(5); break; }
        p = seg_off_to_phys(cpu.sr[DS], cpu.r[DX]);
        n = cpu.r[CX];
        if (p + n > (uint32_t)0x100000) { dos_file_error(5); break; }
        while (done < n) {
            size_t want = n - done > sizeof buf ? sizeof buf : n - done;
            size_t got = fread(buf, 1, want, f), k;
            for (k = 0; k < got; k++) poke8(p + done + (uint32_t)k, buf[k]);
            done += (uint32_t)got;
            if (got < want) break;              /* EOF */
        }
        cpu.flags &= ~F_CF;
        setAX((int)done);                       /* 0 means end of file */
        break; }

    case 0x40: {                                /* write DS:DX, CX -> BX */
        unsigned char buf[512];
        uint32_t p, n, done = 0, k;
        int i, h = cpu.r[BX];
        FILE *f;

        p = seg_off_to_phys(cpu.sr[DS], cpu.r[DX]);
        n = cpu.r[CX];
        if (p + n > (uint32_t)0x100000) { dos_file_error(5); break; }
        if (h == 1 || h == 2) {                 /* stdout / stderr */
            for (k = 0; k < n; k++) dos_putc(peek8(p + k));
            dos_flush(h == 1 ? stdout : stderr);
            cpu.flags &= ~F_CF;
            setAX((int)n);
            break;
        }
        f = dos_slot_file(h, &i);
        if (!f) { dos_file_error(6); break; }
        if (dos_fmode[i] == 0) { dos_file_error(5); break; }
        while (done < n) {
            size_t want = n - done > sizeof buf ? sizeof buf : n - done;
            size_t put;
            for (put = 0; put < want; put++) buf[put] = peek8(p + done + (uint32_t)put);
            put = fwrite(buf, 1, want, f);
            done += (uint32_t)put;
            if (put < want) break;              /* disk full */
        }
        fflush(f);                              /* visible to the next reader */
        if (done < n) { dos_file_error(0); break; }
        cpu.flags &= ~F_CF;
        setAX((int)done);
        break; }

    case 0x42: {                                /* lseek BX, CX:DX from AL */
        int i, whence = cpu.r[AX] & 0xFF;
        uint32_t off;
        long pos;
        FILE *f = dos_slot_file(cpu.r[BX], &i);

        if (!f) { dos_file_error(6); break; }
        if (whence > 2) { dos_file_error(1); break; }   /* AL = 0/1/2 only */
        off = ((uint32_t)cpu.r[CX] << 16) | cpu.r[DX];
        if (fseek(f, (long)(int32_t)off,
                  whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END)) {
            dos_file_error(1);
            break;
        }
        pos = ftell(f);
        if (pos < 0) { dos_file_error(1); break; }
        cpu.flags &= ~F_CF;
        cpu.r[DX] = (uint16_t)((uint32_t)pos & 0xFFFF);
        cpu.r[AX] = (uint16_t)(((uint32_t)pos >> 16) & 0xFFFF);
        break; }

    case 0x41: {                                /* delete file, DS:DX = name */
        char name[128];

        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        if (remove(name) != 0) { dos_file_error(2); break; }
        cpu.flags &= ~F_CF;
        break; }

    case 0x43: {                                /* get/set attributes (AL) */
        char name[128];
        struct stat st;
        int sub = cpu.r[AX] & 0xFF;

        /* Original: 43 -> 0x5E16C0, success marks +0x3A (0x529A14), failure
         * returns AX=0x0001 (0x529A26/0x529A59). */
        if (sub > 1) { dos_file_error(1); break; }
        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        if (stat(name, &st) != 0) { dos_file_error(2); break; }
        if (sub == 0) {                         /* get -> CX */
            int attr = (st.st_mode & S_IFDIR) ? 0x10 : 0x20;
            if (!(st.st_mode & S_IWRITE)) attr |= 0x01;
            cpu.r[CX] = (uint16_t)attr;
            cpu.flags &= ~F_CF;
            break;
        }
        /* Set: the only DOS bit a host file can carry is read-only. */
        if (dos_chmod(name, (cpu.r[CX] & 0x01) ? DOS_IREAD
                                               : (DOS_IREAD | DOS_IWRITE))) {
            dos_file_error(5);
            break;
        }
        cpu.flags &= ~F_CF;
        break; }

    case 0x56: {                                /* rename DS:DX -> ES:DI */
        char from[128], to[128];
        struct stat st;

        /* Original: 56 -> 0x5DF2D0, success marks +0x3A (0x529EFC), failure
         * returns AX=0x0002 (0x529F0E/0x529F41). */
        if (dos_guest_name(from, sizeof from) != 0) { dos_file_error(3); break; }
        if (dos_name_at(cpu.sr[ES], cpu.r[DI], to, sizeof to) != 0) {
            dos_file_error(3);
            break;
        }
        if (stat(from, &st) != 0) { dos_file_error(2); break; }
        if (rename(from, to) != 0) { dos_file_error(5); break; }
        cpu.flags &= ~F_CF;
        break; }

    case 0x4C:                                  /* exit with code */
        dos_exit_requested = 1;
        dos_exit_code = cpu.r[0] & 0xFF;
        cpu.halted = 1;
        break;

    case 0x4D:                                  /* get return code */
        setAX(0);
        break;

    case 0x4E: {                                /* find first */
        char spec[160];

        if (dos_guest_name(spec, sizeof spec) != 0) { dos_file_error(3); break; }
        split_spec(spec, find_dir, sizeof find_dir, find_pat);
        find_attr = cpu.r[CX];                  /* 0x10 = include directories */
        find_index = 0;
        if (!find_match()) { dos_file_error(DOS_NO_MORE_FILES); break; }
        cpu.flags &= ~F_CF;
        setAX(0);
        break; }

    case 0x4F: {                                /* find next */
        find_index++;
        if (!find_match()) {
            if (find_index) find_index--;       /* stay on the last match */
            dos_file_error(DOS_NO_MORE_FILES);
            break;
        }
        cpu.flags &= ~F_CF;
        setAX(0);
        break; }

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

/* AH=13h size guard.  The original logs "INT 10h, AH=13h - " + message,
 * calls [obj+0x7AC], sets the 0x62141C error word and returns through the
 * dispatcher epilogue (0x522F7D-0x523079, jmp 0x52ABDF).  It never writes
 * the halt marker ds:0x621218 or the +0x4C halt word, and the run loop at
 * 0x5A3FCB stops on nothing else, so the guest keeps running: the call is
 * refused, not fatal.  See FINDINGS 24. */
static void int10_write_string_reject(const char *why)
{
    fprintf(stderr, "INT 10h, AH=13h - %s\n", why);
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

            /* The size guard is checked before DX is latched, so a refused
             * call leaves the cursor where it was (the original jumps
             * straight to the epilogue and never reaches the cursor sink). */
            if (cells > 0x7D0) {                /* FINDINGS 5.2 guard */
                int10_write_string_reject(
                    "CX value (string size) is above the allowed limit!");
                break;
            }

            /* DX is the starting position, as on the real BIOS. */
            cur_row = (cpu.r[DX] >> 8) & 0xFF;
            cur_col = cpu.r[DX] & 0xFF;
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
    case 0x01: case 0x11: {                 /* peek: wait, but do not consume */
        int c = kb_peek();                  /* blocks on a terminal */
        if (c >= 0) { cpu.flags &= ~F_ZF;
            cpu.r[0] = (uint16_t)((c << 8) | 0); }
        else { cpu.flags |= F_ZF; cpu.r[0] = 0; }
        break; }
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
