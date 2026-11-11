/* mue — DOS and BIOS interrupt services
 * dos.c
 *
 * The original routes software interrupts through dedicated VB methods
 * (FINDINGS sec. 8).  Here they are ordinary C handlers that inspect the
 * register file and either service the call or forward it to the guest IVT.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
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
/* BIOS set-1 scan codes for the ASCII characters a terminal can produce.
 *
 * The original derives a scan code for every buffered key: [obj+0x770] reads
 * the character, 0x4ACC90 narrows it and [obj+0x774] fills the scan/ASCII
 * pair that ds:0x621264 holds at two bytes per entry (scan at +0, ASCII at
 * +1: 0x5628A5 and 0x5628C9 write them, 0x524B5D/0x524BA7 read them back for
 * INT 16h AH=00/01).  Its own documentation states the contract plainly:
 * "INT 16h / AH = 00h ... return: AH = BIOS scan code. AL = ASCII
 * character."  The clone used to leave AH = 00 for every key.
 *
 * The table is the standard set-1 map, which is what a real BIOS returns, so
 * a guest that tests AH (arrow keys, Enter vs Ctrl-M) sees familiar values. */
static unsigned char bios_scan(unsigned char ch)
{
    /* letters and digits are contiguous in set 1 */
    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return (unsigned char)(0x1E + (ch - 'A'));

    switch (ch) {
    case '1': return 0x02;  case '2': return 0x03;  case '3': return 0x04;
    case '4': return 0x05;  case '5': return 0x06;  case '6': return 0x07;
    case '7': return 0x08;  case '8': return 0x09;  case '9': return 0x0A;
    case '0': return 0x0B;
    case '-': return 0x0C;  case '=': return 0x0D;
    case '[': return 0x1A;  case ']': return 0x1B;
    case ';': return 0x27;  case '\'': return 0x28; case '`': return 0x29;
    case '\\': return 0x2B; case ',': return 0x33;
    case '.': return 0x34;  case '/': return 0x35;
    case ' ': return 0x39;
    case '\r': case '\n': return 0x1C;         /* Enter */
    case 0x08: return 0x0E;                    /* Backspace */
    case 0x09: return 0x0F;                    /* Tab */
    case 0x1B: return 0x01;                    /* Esc */
    default:   return 0x00;                    /* no scan code known */
    }
}

static unsigned char kb[256];
static unsigned char kb_scan[256];             /* parallel BIOS scan codes */
static int kb_head, kb_tail;
static const char *scripted = NULL;      /* redirected stdin text */
static size_t scripted_pos;

void dos_push_key(int ch)
{
    int nt = (kb_tail + 1) & 0xFF;
    if (nt == kb_head) return;
    kb[kb_tail] = (unsigned char)ch;
    kb_scan[kb_tail] = bios_scan((unsigned char)ch);
    kb_tail = nt;
}

/* Point stdin at a literal string instead of the host stream, so a check can
 * feed a guest exactly what it means to without plumbing a pipe.  The text is
 * not copied, so it has to stay alive for the rest of the run; NULL goes back
 * to the host stream.  Anything already typed is dropped, because a script
 * describes stdin from the start of the run. */
/* The original's key buffer holds 16 entries and silently drops anything
 * beyond that: 0x56287C loads the count, `cmp ax,0x10 / jge 0x562906` and the
 * arm it skips to waits then returns without storing.  Each entry is a
 * scan/ASCII pair at ds:0x621264 (two bytes each). */
#define KB_DEPTH 16

void dos_feed_stdin(const char *text)
{
    scripted = text;
    scripted_pos = 0;
    kb_head = kb_tail = 0;
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
static int kb_count(void)
{
    return (kb_tail - kb_head) & 0xFF;
}

static int kb_refill(void)
{
    int c;
    if (kb_head != kb_tail) return 1;
    if (kb_count() >= KB_DEPTH) return 0;
    if (scripted) {
        c = scripted[scripted_pos] ? (unsigned char)scripted[scripted_pos++] : -1;
    } else {
        c = host_stdin_byte();
    }
    if (c < 0) return 0;
    dos_push_key(c);
    return kb_head != kb_tail;
}

/* Is stdin a terminal?  DOS echoes console input and does not echo a file, so
 * every read path has to know which it has.  Cached: a run's redirection
 * cannot change under the guest. */
static int dos_stdin_console(void)
{
    static int on = -1;
    if (scripted) return 0;                 /* a script is a file, not a console */
    if (on < 0) {
#ifdef _WIN32
        on = _isatty(_fileno(stdin)) ? 1 : 0;
#else
        on = isatty(fileno(stdin)) ? 1 : 0;
#endif
    }
    return on;
}

/* Pop a key, waiting for one when nothing is buffered. */
/* Last key's BIOS scan code, kept beside the character so INT 16h can report
 * both halves of AX. */
static int kb_last_scan;

static int kb_pop(void)
{
    int v;
    if (!kb_refill()) return -1;
    v = kb[kb_head];
    kb_last_scan = kb_scan[kb_head];
    kb_head = (kb_head + 1) & 0xFF;
    return v;
}

/* Look at the next key without consuming it, waiting for one first. */
static int kb_peek(void)
{
    if (!kb_refill()) return -1;
    kb_last_scan = kb_scan[kb_head];
    return kb[kb_head];
}

/* ---- screen ---------------------------------------------------------- */
void dos_putc(int ch)
{
    if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = (char)ch;
    if (ch == '\n' && outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '\r';
}

void dos_puts(const char *s) { while (*s) dos_putc((unsigned char)*s++); }

/* ---- standard output -------------------------------------------------- */
/* `> file` replaces DOS's standard output: handle 1 and the functions that
 * write to it (02h, 09h) no longer reach the screen.  The sink is global, so a
 * child started by 4Bh writes to the same file; that is what inheriting a
 * redirection means (FINDINGS 37).  Console-only output (INT 10h, 06h,
 * keyboard echo) still goes to the screen through dos_putc. */
static FILE *dos_out_file;

void dos_stdout_close(void);            /* also the atexit() hook */

static void stdout_byte(int ch)
{
    if (dos_out_file) {
        fputc(ch, dos_out_file);           /* atexit() closes and flushes */
    } else {
        dos_putc(ch);
    }
}

int dos_redirect_stdout(const char *path)
{
    static int hooked;
    FILE *f = fopen(path, "wb");

    if (!f) return -1;
    if (dos_out_file) fclose(dos_out_file);
    dos_out_file = f;
    if (!hooked) {
        hooked = 1;
        atexit(dos_stdout_close);
    }
    return 0;
}

void dos_stdout_close(void)
{
    if (dos_out_file) {
        fclose(dos_out_file);
        dos_out_file = NULL;
    }
}

void dos_flush(FILE *f)
{
    if (outlen) { fwrite(outbuf, 1, (size_t)outlen, f); outlen = 0; }
}

/* ---- helpers --------------------------------------------------------- */
static void setAX(int v) { cpu.r[0] = (uint16_t)v; }

/* Declared here because INT 21h's own fail arm needs it long before the
 * definition further down next to the video and floppy services. */
static void unsupported_ah(const char *service, int ah);
/* INT 21h AH=0Ch re-enters the dispatcher with AH=AL (see the 0x527749 call),
 * so the handler needs its own prototype. */
static void int21(void);
/* Writes AL.  The name is historical - it does NOT touch AH, which is why
 * every AL-returning service calls it. */
static void setAL(int v) { cpu.r[0] = (uint16_t)((cpu.r[0] & 0xFF00) | (v & 0xFF)); }
#define setAThi setAL

#define DOS_DRIVES 27

/* Current drive, A=0.  The original keeps it in ds:0x621C5C, set by 0Eh and
 * read by 19h; C: is what emu8086 starts on. */
static int dos_drive = 2;

/* Write-verify flag.  The original keeps exactly one byte at ds:0x621CD0:
 * FUN_005e0860 stores AL into it (INT 21h AH=2Eh) and FUN_005e0870 loads it
 * back into AL (INT 21h AH=54h).  emu8086 never consults it when writing, so
 * neither does the clone - the point is that the round trip works. */
static unsigned char dos_verify_flag = 0;

/* Each drive remembers its own current directory, as DOS does, as a path
 * relative to the drive root ("" = root).  The host cwd is the live directory
 * of dos_drive, so switching drives (0Eh) re-enters that drive's directory. */
static char dos_dir[DOS_DRIVES][160];
static int dos_seeded;

/* 47h wants the current directory the way DOS shows it: no drive, no leading
 * separator ("EMU8086\EXAMPLE").  The original keeps one virtual
 * subdirectory per drive ("GET_VIRTUAL_SUBDIRECTORY"); the clone tracks a
 * single host directory, so the drive that is not the current one reports its
 * root, and moving between directories (3Bh) is not modelled. */
static void dos_current_dir(char *out, size_t n)
{
    char buf[512];
    char *p;
    size_t i;

    out[0] = 0;
    if (getcwd(buf, sizeof buf) == NULL) return;
    p = buf;
    if (p[0] != 0 && p[1] == ':') p += 2;       /* drop the drive letter */
    while (*p == '/' || *p == '\\') p++;
    for (i = 0; p[i] && i + 1 < n; i++) out[i] = p[i];
    out[i] = 0;
#ifdef _WIN32
    for (i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
#endif
}

/* Seed the current drive's directory from the host cwd the first time
 * anything asks, so 47h answers what the emulator was started in. */
static void dos_seed(void)
{
    if (dos_seeded) return;
    dos_seeded = 1;
    dos_current_dir(dos_dir[dos_drive], sizeof dos_dir[0]);
}

/* Host path of a drive's directory: drive root plus the DOS-visible path. */
static void dos_host_dir(int drive, const char *dir, char *out, size_t n)
{
#ifdef _WIN32
    snprintf(out, n, "%c:\\%s", 'A' + drive, dir);
#else
    snprintf(out, n, "/%s", dir);
#endif
}

static int dos_chdir(const char *dir)
{
    char host[512];
    dos_host_dir(dos_drive, dir, host, sizeof host);
    return chdir(host);
}

/* Apply a DOS path to a drive's directory string: absolute paths start at the
 * drive root, ".." climbs (clamped at the root), and "C:" is accepted when it
 * names the current drive.  Returns 0, or -1 for a path we cannot model. */
static int dos_apply_path(int drive, const char *path, char *out, size_t n)
{
    const char *p = path;
    size_t o = 0;

    if (p[0] && p[1] == ':') {
        int d = toupper((unsigned char)p[0]) - 'A';
        if (d != drive) return -1;          /* one host cwd, see 28 */
        p += 2;
    }
    if (*p == '\\' || *p == '/') {
        p++;                                /* absolute: from the drive root */
    } else {
        while (dos_dir[drive][o] && o + 1 < n) { out[o] = dos_dir[drive][o]; o++; }
    }
    out[o] = 0;

    while (*p) {
        char comp[64];
        size_t cl = 0;
        const char *start;

        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        while (start < p && cl + 1 < sizeof comp) comp[cl++] = *start++;
        comp[cl] = 0;

        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            while (o && out[o - 1] != '\\') o--;
            if (o) o--;
            out[o] = 0;
            continue;
        }
        if (o) {
            if (o + 1 >= n) return -1;
            out[o++] = '\\';
        }
        if (o + cl >= n) return -1;
        memcpy(out + o, comp, cl);
        o += cl;
        out[o] = 0;
    }
    return 0;
}

/* ---- INT 21h 48h/49h/4Ah: memory blocks ----------------------------- */
/* The original's handlers (0x60A910, 0x60AE30, 0x60B0B0) are VB wrappers
 * around its internal block allocator: they hand out 0x500-byte chunks from a
 * private range that starts at 0x3000 and is capped at 0x9000 (globals
 * ds:0x621DAC base / 0x621DB0 / 0x621DB4 count / 0x621DB8 block list), reusing
 * freed entries.  They return their result in EAX to an arm that discards it,
 * so a guest sees *no* segment and *no* error code at all - the arms
 * 0x52A266/0x52A291/0x52A2BC just call and jump to the shared epilogue.
 *
 * That is unusable for a program that really allocates, so the clone answers
 * the DOS way from an arena of its own, well above the guest's PSP at 0x0100.
 * Blocks are paragraphs and never move; 4Ah only grows a block in place. */
#define DOS_HEAP_FIRST 0x2000
#define DOS_HEAP_END   0xA000
#define DOS_BLOCKS     64

/* A child started by 4Bh gets its own 64K window, counting down from the top
 * of the megabyte (0xF000, 0xE000, ...) so that children can nest and none of
 * them overlaps the 48h arena, which ends at 0xA000 paragraphs.  Video memory
 * is not in MEM (it is a separate array in this file), and nothing else of
 * ours lives above 0xA000. */
#define DOS_CHILD_TOP    0xF000
#define DOS_CHILD_LEVELS 6
static int dos_exec_depth;

static uint16_t dos_child_seg(void)
{
    return (uint16_t)(DOS_CHILD_TOP - 0x1000 * dos_exec_depth);
}

/* The original answers 48h/49h/4Ah with nothing observable (FINDINGS 29).
 * MUE_INT21_MEM_NOOP=1 reproduces that exactly - the handler returns without
 * touching a register - so both models can be tested. */
static int dos_mem_noop(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MUE_INT21_MEM_NOOP");
        on = (e != NULL && *e != 0 && *e != '0');
    }
    return on;
}

static struct {
    uint16_t seg, paras;
    int used;
} dos_block[DOS_BLOCKS];

/* Is [seg, seg+paras) clear of every live block? */
static int dos_gap_free(uint32_t seg, uint32_t paras)
{
    int i;
    for (i = 0; i < DOS_BLOCKS; i++) {
        uint32_t b = dos_block[i].seg;
        if (!dos_block[i].used) continue;
        if (seg < b + dos_block[i].paras && b < seg + paras) return 0;
    }
    return 1;
}

/* Largest run of free paragraphs in the arena - what 48h/4Ah report in BX
 * when they fail, so the guest can retry with a smaller request. */
static uint32_t dos_largest_free(void)
{
    uint32_t seg, run = 0, best = 0;
    for (seg = DOS_HEAP_FIRST; seg < DOS_HEAP_END; seg++) {
        if (dos_gap_free(seg, 1)) {
            if (++run > best) best = run;
        } else {
            run = 0;
        }
    }
    return best;
}

static int dos_block_find(uint32_t seg)
{
    int i;
    for (i = 0; i < DOS_BLOCKS; i++)
        if (dos_block[i].used && dos_block[i].seg == seg) return i;
    return -1;
}

/* Allocate PARAS paragraphs; 0 when they do not fit.  Shared by 48h and by the
 * 4Bh environment copy, which needs a block but must answer itself. */
static uint32_t dos_arena_alloc(uint32_t paras)
{
    uint32_t seg = DOS_HEAP_FIRST;
    int i;

    if (!paras) return 0;
    for (i = 0; i < DOS_BLOCKS; i++)
        if (!dos_block[i].used) break;
    if (i == DOS_BLOCKS) return 0;
    for (; seg + paras <= DOS_HEAP_END; seg++) {
        if (!dos_gap_free(seg, paras)) continue;
        dos_block[i].seg = (uint16_t)seg;
        dos_block[i].paras = (uint16_t)paras;
        dos_block[i].used = 1;
        return seg;
    }
    return 0;
}

static void dos_arena_free(uint32_t seg)
{
    int i = dos_block_find(seg);
    if (i >= 0) dos_block[i].used = 0;
}

/* Build a $-terminated string starting at DS:DX and print it. */
static void print_dollar_string(void)
{
    uint32_t p = seg_off_to_phys(cpu.sr[DS], cpu.r[2]);
    int guard = 0;
    while (peek8(p) != '$' && guard++ < 65535) {
        stdout_byte(peek8(p));
        p++;
    }
}

/* Read a line from the keyboard queue into DS:DX (DOS 0Ah). */
static void read_buffered(uint16_t buf_off)
{
    uint32_t p = seg_off_to_phys(cpu.sr[DS], buf_off);
    int maxlen = peek8(p);
    int i = 0;
    int echo = dos_stdin_console();        /* a redirected line is not echoed */
    (void)maxlen;

    for (;;) {
        int c = kb_pop();
        if (c < 0) break;
        if (c == '\r' || c == '\n') {
            if (echo) dos_putc('\n');
            break;
        }
        if (c == 8) {
            if (i > 0) { i--; if (echo) dos_puts("\b \b"); }
            continue;
        }
        poke8(p + 2 + (uint32_t)i, (uint8_t)c);
        i++;
        if (echo) dos_putc(c);
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
/* Host path of each open handle.  57h (get/set file date) needs a name to
 * stat, and DOS hands it only a handle, so it has to be remembered at open. */
static char  dos_fname[DOS_FILES][260];

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

/* ---- the PSP, and the job file table inside it ----------------------- */
/* DOS keeps every process's handle table - the "job file table" - at PSP:0x18:
 * 20 one-byte entries, 0xFF for a free one, 0x80+n for a character device, and
 * 0..0x7F for a slot in the system file table.  The clone's handles are 5+i for
 * dos_files[i] (sec. 5.1), so entry 5+i holds i, entries 0-4 hold 0x80+h the way
 * DOS numbers CON/AUX/PRN, and PSP:0x32/0x34 hold the table's size and offset.
 *
 * dos_psp is whose PSP is current - the program main.c loaded, or the window of
 * the child 4Bh is running - so 3C/3D/3E keep the right table honest and a
 * child finds the handles it inherited already marked open. */
#define DOS_BOOT_PSP 0x0100
#define DOS_JFT_OFF  0x18
#define DOS_JFT_SIZE 20

static uint16_t dos_psp = DOS_BOOT_PSP;

static void dos_jft_set(uint16_t psp, int h, uint8_t v)
{
    if (h < 0 || h >= DOS_JFT_SIZE) return;
    poke8(((uint32_t)psp << 4) + DOS_JFT_OFF + (uint32_t)h, v);
}

/* The entry a handle should have here: 0xFF when it is not open. */
static uint8_t dos_jft_byte(int h)
{
    int i = h - DOS_FIRST_HANDLE;
    if (h < DOS_FIRST_HANDLE) return (uint8_t)(0x80 + h);   /* CON, AUX, PRN */
    if (i < DOS_FILES && dos_files[i]) return (uint8_t)i;
    return 0xFF;
}

/* Write the whole table, plus the DOS 3.x size and far-pointer fields. */
static void dos_jft_fill(uint16_t psp)
{
    uint32_t base = (uint32_t)psp << 4;
    int h;
    for (h = 0; h < DOS_JFT_SIZE; h++) dos_jft_set(psp, h, dos_jft_byte(h));
    poke16(base + 0x32, DOS_JFT_SIZE);          /* table size, in bytes */
    poke16(base + 0x34, DOS_JFT_OFF);           /* and where it starts */
    poke16(base + 0x36, psp);
}

/* The program `mue` runs has a PSP too.  There is no COMMAND.COM above it, so
 * its parent segment points at itself.  main.c calls this once per load. */
/* CONFIRMED: emu8086 ships a real interrupt vector table in its INT_VECT
 * file and a stub block in BIOS_ROM at F400:0100.  Every vector except 19h
 * points at a three-instruction thunk inside it:
 *
 *     FFFF        pushf
 *     CD NN       int NNh
 *     CF          iret        (retf, so it suits a far call too)
 *
 * with the INT 10h vector at F400:0190, INT 21h at F400:0200 and so on.
 * That is why a guest may `call far es:[40h]` instead of using `int` -
 * examples/far_call.asm does exactly that.  The clone installs the same
 * shape so such a jump lands on a thunk that reaches dos_interrupt(). */
#define IVT_STUB_SEG 0xF400
static void dos_boot_ivt(void)
{
    int n;
    for (n = 0; n < 256; n++) {
        uint32_t v = (uint32_t)n * 4;
        uint32_t thunk = ((uint32_t)IVT_STUB_SEG << 4) + 0x100 + (uint32_t)n * 16;
        if (n == 0x19) {                        /* a reboot is not callable */
            poke16(v, 0x0000);
            poke16(v + 2, 0xFFFF);
            continue;
        }
        poke8 (thunk + 0, 0x9C);                /* pushf  */
        poke8 (thunk + 1, 0xCD);                /* int    */
        poke8 (thunk + 2, (uint8_t)n);
        poke8 (thunk + 3, 0xCF);                /* iret   */
        poke16(v, (uint16_t)(0x100 + n * 16));
        poke16(v + 2, IVT_STUB_SEG);
    }
}

void dos_boot_psp(void)
{
    uint32_t base = (uint32_t)DOS_BOOT_PSP << 4;

    dos_boot_ivt();
    dos_psp = DOS_BOOT_PSP;
    poke16(base + 0x16, DOS_BOOT_PSP);
    /* PSP:0x02 is the first paragraph past the program's block, and a guest
     * sizes its heap with (top - PSP).  The boot program's block runs to the
     * end of the 48h arena - 0xA000 paragraphs, the classic top of
     * conventional memory - and child windows start above that (sec. 40). */
    poke16(base + 2, DOS_HEAP_END);
    dos_jft_fill(DOS_BOOT_PSP);
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

/* Little-endian word out of a file image, for the MZ header. */
static unsigned dos_img16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* ---- INT 21h --------------------------------------------------------- */
static void int21(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;
    static int printer_reported;

    switch (ah) {
    case 0x00:                                  /* terminate */
        dos_exit_requested = 1;
        dos_exit_code = 0;
        cpu.halted = 1;
        break;

    case 0x01: {                                /* read char with echo */
        /* A console function: it echoes even when stdin is redirected, which
         * is what the original does here (check int21-01-reads-in-order). */
        int c = kb_pop();
        if (c < 0) c = 0x1A;
        dos_putc(c);
        setAThi(c);
        break; }

    case 0x02:                                  /* display output */
        if ((cpu.r[2] & 0xFF) == 9) stdout_byte('\t');
        else stdout_byte(cpu.r[2] & 0xFF);
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

    case 0x05:                                  /* printer output, DL */
        /* Original: 05 -> 0x5E1E70, which raises "virtual printer does not
         * respond." (0x452204 at 0x5E1FEF) and returns AL=0x90 so a guest
         * that polls keeps going.  There is no printer here, and a spool
         * file nobody reads would be a fiction, so the clone prints the same
         * refusal and answers AL=0x90 (FINDINGS 5.1). */
        if (printer_reported == 0) {
            printer_reported = 1;
            fprintf(stderr, "virtual printer does not respond.\n");
            fflush(stderr);
        }
        setAThi(0x90);
        break;

    case 0x09:                                  /* print $-string */
        print_dollar_string();
        break;

    case 0x0A:                                  /* buffered input */
        read_buffered(cpu.r[2]);
        break;

    case 0x0B:                                  /* check input status */
        /* A redirected stream is always "ready" until it ends: the queue is
         * refilled lazily, so an empty queue is not an empty input here. */
        if (!dos_stdin_console()) kb_refill();
        setAThi(kb_head != kb_tail ? 0xFF : 0x00);
        break;

    case 0x0E:                                  /* select disk, DL */
        /* Original: 0x5DC310 stores DL in ds:0x621C5C, then the arm sets
         * AL=0x19 (0x5277AD) - 25 logical drives.  Every drive keeps its own
         * current directory, so the host cwd follows the switch. */
        dos_seed();
        dos_drive = cpu.r[DX] & 0xFF;
        if (dos_drive < DOS_DRIVES) dos_chdir(dos_dir[dos_drive]);
        setAThi(0x19);
        break;

    case 0x19:                                  /* current drive */
        /* Original: 0x5DC390 returns ds:0x621C5C, arm puts it in AL. */
        setAThi(dos_drive);
        break;

    case 0x36: {                                /* free disk space */
        /* Original (0x527F35-0x528060): DL above 26 sets both halves of AX
         * (AL and AH) to 0xFF, i.e. AX=0xFFFF.  Every other drive gets the
         * same fixed picture: AL=0x12, AH=0, BX=0x0200, DL=0, DH=0xA0, with CX
         * and the other registers untouched.  There is no filesystem behind
         * it, so the values never change. */
        if ((cpu.r[DX] & 0xFF) > 26) {
            dos_file_error(0xFFFF);
            break;
        }
        setAX(0x0012);                          /* AL=18, AH=0 */
        cpu.r[3] = 0x0200;                      /* BX */
        cpu.r[2] = 0xA000;                      /* DL=0, DH=0xA0 */
        cpu.flags &= ~F_CF;
        break; }

    case 0x48: {                                /* allocate block, BX paras */
        uint32_t seg;

        if (dos_mem_noop()) break;              /* original: no answer at all */
        seg = dos_arena_alloc(cpu.r[3]);
        if (!seg) {                             /* 0 paragraphs fails too */
            cpu.r[3] = (uint16_t)dos_largest_free();
            dos_file_error(8);                  /* insufficient memory */
            break;
        }
        setAX((uint16_t)seg);
        cpu.flags &= ~F_CF;
        break; }

    case 0x49: {                                /* free block, ES = segment */
        int i;

        if (dos_mem_noop()) break;
        i = dos_block_find(cpu.sr[ES]);

        if (i < 0) {
            dos_file_error(9);                  /* invalid block */
            break;
        }
        dos_block[i].used = 0;
        setAX(0);
        cpu.flags &= ~F_CF;
        break; }

    case 0x4B: {                                /* exec a program */
        char name[160], want[160], host[512], path_rec[200];
        FILE *f;
        long n;
        uint8_t *img;
        cpu_t saved;
        int saved_req = dos_exit_requested, saved_code = dos_exit_code;
        uint32_t saved_dta = dta;
        int saved_dta_valid = dta_valid, saved_depth = dos_exec_depth;
        int child_exit, cmd_len, i, fi, is_mz = 0;
        int overlay = ((cpu.r[0] & 0xFF) == 1);
        uint16_t seg, load_seg, env_seg = 0;
        uint16_t e_ip = 0x100, e_cs = 0, e_ss = 0, e_sp = 0xFFFE;
        uint16_t saved_psp = dos_psp;
        FILE *saved_files[DOS_FILES];
        uint32_t base, img_base, pb, cmd_phys, env_phys, env_len, j;
        uint32_t top_paras = 0;                 /* PSP:0x02, once seg is known */

        memcpy(saved_files, dos_files, sizeof saved_files);

        /* Original: no 4Bh arm either (see 30), so it reports "not supported"
         * and returns.  A child process is a divergence, implemented the DOS
         * way for the one form that can work here: AL=0, load and execute a
         * .COM or MZ image with a parameter block (ES:BX); MZ support is in
         * 34.  AL=1 loads an overlay into the caller's own memory without a
         * process (39).  AL=3 is the same load without an environment or
         * command line, and is still refused with error 1. */
        if ((cpu.r[0] & 0xFF) != 0 && !overlay) { dos_file_error(1); break; }
        if (!overlay && dos_exec_depth >= DOS_CHILD_LEVELS) { dos_file_error(8); break; }
        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        dos_seed();
        if (dos_apply_path(dos_drive, name, want, sizeof want) != 0) {
            dos_file_error(3);
            break;
        }
        dos_host_dir(dos_drive, want, host, sizeof host);
        f = fopen(host, "rb");
        if (!f) { dos_file_error(2); break; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        img = (uint8_t *)malloc((size_t)n + 1);
        if (!img || n <= 0) { fclose(f); free(img); dos_file_error(11); break; }
        if (fread(img, 1, (size_t)n, f) != (size_t)n) {
            fclose(f); free(img); dos_file_error(11); break;
        }
        fclose(f);
        is_mz = (n >= 2 && img[0] == 'M' && img[1] == 'Z');
        pb = seg_off_to_phys(cpu.sr[ES], cpu.r[3]);

        if (overlay) {
            /* An overlay has no process and no PSP: the caller names where the
             * image goes in the parameter block, as a far pointer at +0x0E
             * whose segment is the load address (DOS wants the offset part
             * zero).  It also enters the overlay itself, so nothing here
             * touches the registers beyond CF. */
            seg = peek16(pb + 0x10);
            if (seg < 0x40) {                   /* 0 = no address given */
                free(img);
                dos_file_error(9);              /* invalid block address */
                break;
            }
        } else {
            if (!is_mz && n > 0xFF00) n = 0xFF00;
            seg = dos_child_seg();
        }
        base = (uint32_t)seg << 4;
        load_seg = overlay ? seg : (uint16_t)(seg + 0x10);
        img_base = overlay ? base : base + 0x100;   /* MZ module or .COM text */
        top_paras = (uint32_t)seg + 0x1000;         /* the whole window */
        if (!overlay) memset(MEM + base, 0, 0x10000);

        if (!is_mz) {
            if (img_base + (uint32_t)n > 0x100000) {
                free(img);
                dos_file_error(8);
                break;
            }
            memcpy(MEM + img_base, img, (size_t)n);
        } else {
            /* MZ (EXE): the header says how long the module is, where the
             * relocation table is, and what CS:IP / SS:SP start as.  Every
             * relocation is a segment word that has to have the load segment
             * added, which is the whole reason an EXE cannot simply be
             * copied (FINDINGS 34).  The header layout is the one exe.h
             * documents from the writing side. */
            unsigned cblp = dos_img16(img + 0x02), cp = dos_img16(img + 0x04);
            unsigned nrel = dos_img16(img + 0x06), hpar = dos_img16(img + 0x08);
            unsigned mina = dos_img16(img + 0x0A);
            unsigned maxa = dos_img16(img + 0x0C);
            unsigned rel  = dos_img16(img + 0x18);
            long hdr = (long)hpar * 16;
            long total, isize;

            total = (cp ? (long)(cp - 1) * 512 : 0) +
                    (cblp ? (long)cblp : 512);
            isize = total - hdr;
            if (hdr < 0x1C || hdr > n || isize <= 0 || hdr + isize > n ||
                (long)rel + 4L * (long)nrel > n) {
                free(img);
                dos_file_error(11);             /* invalid EXE format */
                break;
            }
            /* A child gets a 64K window and has to fit its minalloc beside the
             * image; an overlay goes wherever the caller asked and allocates
             * nothing, so only the top of memory matters. */
            if (overlay ? (img_base + (uint32_t)isize > 0x100000)
                        : (isize + (long)mina * 16 > 0x10000 - 0x100)) {
                free(img);
                dos_file_error(8);              /* insufficient memory */
                break;
            }
            /* DOS gives the image plus minalloc, then grows the block toward
             * maxalloc as far as memory allows.  That block end is what
             * PSP:0x02 reports, and it is how a program learns its heap size,
             * so a header that caps maxalloc must shrink it.  The window is
             * the ceiling, and 0xFFFF means "as much as there is". */
            if (!overlay && maxa != 0xFFFF) {
                uint32_t need = (uint32_t)isize + (uint32_t)mina * 16;
                uint32_t want = (uint32_t)maxa * 16;

                if (want < need) want = need;
                if (want > 0x10000 - 0x100) want = 0x10000 - 0x100;
                top_paras = (uint32_t)seg + 0x10 + (want + 15) / 16;
            }
            memcpy(MEM + img_base, img + hdr, (size_t)isize);
            for (i = 0; i < (int)nrel; i++) {
                unsigned off = dos_img16(img + rel + 4 * i);
                unsigned sgm = dos_img16(img + rel + 4 * i + 2);
                uint32_t at;

                if ((long)off + 2 > isize || ((long)sgm << 4) + off + 2 > isize) {
                    free(img);
                    dos_file_error(11);
                    break;
                }
                at = img_base + ((uint32_t)sgm << 4) + off;
                poke16(at, (uint16_t)(peek16(at) + load_seg));
            }
            if (i < (int)nrel) break;           /* bad relocation, no cleanup */
            e_cs = dos_img16(img + 0x16);
            e_ip = dos_img16(img + 0x14);
            e_ss = dos_img16(img + 0x0E);
            e_sp = dos_img16(img + 0x10);
        }
        free(img);

        if (overlay) {
            /* Loaded, not run: DOS hands no entry point back, because the
             * caller chose the address and knows the image's header. */
            cpu.flags &= ~F_CF;
            break;
        }

        /* ---- minimal PSP, from the caller's parameter block ------------
         * ES:BX points at the DOS 3.x block: +0 environment segment, then a
         * far pointer to the counted command line, then two FCB pointers we
         * leave as the zeroed block already has them. */
        cmd_phys = seg_off_to_phys(peek16(pb + 4), peek16(pb + 2));
        env_phys = seg_off_to_phys(peek16(pb), 0);

        cmd_len = peek8(cmd_phys);
        if (cmd_len > 127) cmd_len = 127;
        MEM[base + 0x80] = (uint8_t)cmd_len;
        for (i = 0; i < cmd_len; i++)
            MEM[base + 0x81 + i] = peek8(cmd_phys + 1 + (uint32_t)i);
        MEM[base + 0x81 + cmd_len] = 0x0D;      /* DOS's terminating CR */

        MEM[base]      = 0xCD;                  /* int 20h */
        MEM[base + 1]  = 0x20;
        MEM[base + 0x50] = 0xCD;                /* int 21h; retf */
        MEM[base + 0x51] = 0x21;
        MEM[base + 0x52] = 0xCB;
        {
            /* PSP:0x02 is the first paragraph past the program's block.  The
             * topmost window ends at the megabyte, where that value does not
             * fit in a word: report the last paragraph instead of wrapping to
             * 0x0000, so a child computing its free memory (top - PSP) still
             * gets a positive, under-estimated answer (FINDINGS 35). */
            uint32_t top = top_paras;

            poke16(base + 2, (uint16_t)(top > 0xFFFF ? 0xFFFF : top));
        }
        poke16(base + 0x16, dos_psp);           /* parent PSP */

        /* The job file table: every handle open here is open in the child too,
         * because 4Bh inherits them, so the child can use PSP:0x18+5 to find
         * the handle it was given. */
        dos_jft_fill(seg);

        /* ---- environment: the caller's strings plus DOS's two trailing
         * records (a word 1, the full path, a word 0), copied into a block of
         * its own so the child owns it and the parent may free or reuse
         * its copy.  PSP:0x2C points at the copy. ---------------------- */
        env_len = 0;
        if (peek16(pb)) {
            while (env_len < 2048 &&
                   !(peek8(env_phys + env_len) == 0 &&
                     peek8(env_phys + env_len + 1) == 0))
                env_len++;
            env_len += 2;                       /* keep the empty string */
        }
        if (env_len) {
            uint32_t total;

            snprintf(path_rec, sizeof path_rec, "%c:\\%s", 'A' + dos_drive, want);
            total = env_len + 2 + (uint32_t)strlen(path_rec) + 1 + 2;
            env_seg = (uint16_t)dos_arena_alloc((total + 15) / 16);
            if (!env_seg) { dos_file_error(8); break; }
            {
                uint32_t eb = (uint32_t)env_seg << 4;

                memset(MEM + eb, 0, ((total + 15) / 16) * 16);
                for (j = 0; j < env_len; j++)
                    MEM[eb + j] = peek8(env_phys + j);
                MEM[eb + env_len] = 1;
                MEM[eb + env_len + 1] = 0;
                for (j = 0; j <= strlen(path_rec); j++)
                    MEM[eb + env_len + 2 + j] = (uint8_t)path_rec[j];
                MEM[eb + env_len + 3 + strlen(path_rec)] = 0;
            }
            poke16(base + 0x2C, env_seg);
        }

        saved = cpu;
        if (is_mz) {
            /* DOS gives an EXE its PSP in DS and ES, and CS:IP / SS:SP exactly
             * as the header describes them, relative to the load segment. */
            cpu.sr[CS] = (uint16_t)(load_seg + e_cs);
            cpu.sr[SS] = (uint16_t)(load_seg + e_ss);
            cpu.sr[DS] = cpu.sr[ES] = seg;
            cpu.ip = e_ip;
            cpu.r[SP] = e_sp;
        } else {
            cpu.sr[ES] = cpu.sr[CS] = cpu.sr[SS] = cpu.sr[DS] = seg;
            cpu.ip = 0x100;
            cpu.r[SP] = 0xFFFE;
        }
        cpu.phys = seg_off_to_phys(cpu.sr[CS], cpu.ip);
        cpu.flags = F_IF;
        cpu.halted = 0;
        cpu.exit_code = 0;
        cpu.seg_override = -1;
        cpu.lock = 0;
        cpu.rep_kind = 0;
        dta_valid = 0;                          /* the child's own PSP */
        dos_exit_requested = 0;
        dos_exit_code = 0;
        dos_exec_depth = saved_depth + 1;
        dos_psp = seg;                          /* its PSP is current now */

        cpu_run(2000000);

        child_exit = dos_exit_requested ? dos_exit_code : 0;
        cpu = saved;
        dta = saved_dta;
        dta_valid = saved_dta_valid;
        dos_exit_requested = saved_req;
        dos_exit_code = saved_code;
        dos_exec_depth = saved_depth;
        dos_psp = saved_psp;

        /* Handles the child opened and never closed are its own; a DOS process
         * cannot leak them into the parent, and the clone's table is shared by
         * every process (sec. 32), so drop them here. */
        for (fi = 0; fi < DOS_FILES; fi++) {
            if (!saved_files[fi] && dos_files[fi]) {
                fclose(dos_files[fi]);
                dos_files[fi] = NULL;
            }
        }
        if (env_seg) dos_arena_free(env_seg);
        setAX((uint16_t)(child_exit & 0xFF));
        cpu.flags &= ~F_CF;
        break; }

    case 0x4A: {                                /* resize block, ES/BX */
        int i;
        uint32_t seg, want;

        if (dos_mem_noop()) break;
        i = dos_block_find(cpu.sr[ES]);
        want = cpu.r[3];

        if (i < 0) {
            dos_file_error(9);                  /* invalid block */
            break;
        }
        seg = dos_block[i].seg;
        if (want > dos_block[i].paras) {
            /* Blocks never move: ask for the space right after this one. */
            uint32_t grow = want - dos_block[i].paras, room = 0;

            while (seg + dos_block[i].paras + room < DOS_HEAP_END &&
                   dos_gap_free(seg + dos_block[i].paras + room, 1) &&
                   room < grow)
                room++;
            if (room < grow) {
                cpu.r[3] = (uint16_t)(dos_block[i].paras + room);
                dos_file_error(8);              /* insufficient memory */
                break;
            }
        }
        dos_block[i].paras = (uint16_t)want;
        setAX(0);
        cpu.flags &= ~F_CF;
        break; }

    case 0x3B: {                                /* change directory */
        char path[160], want[160];

        /* Original (0x52829A-0x52837C): DS:DX goes through the EA prologue and
         * CHANGE_VIRTUAL_SUBDIRECTORY (0x5DD420); non-zero sets +0x3A and
         * AX=0, zero sets +0x38 and AX=3 (path not found). */
        if (dos_guest_name(path, sizeof path) != 0) {
            dos_file_error(3);
            break;
        }
        dos_seed();
        if (dos_apply_path(dos_drive, path, want, sizeof want) != 0 ||
            dos_chdir(want) != 0) {
            dos_file_error(3);
            break;
        }
        strcpy(dos_dir[dos_drive], want);
        cpu.r[0] = 0;
        cpu.flags &= ~F_CF;
        break; }

    case 0x47: {                                /* get current directory */
        char path[64];
        int drive = cpu.r[DX] & 0xFF;
        uint32_t p;
        size_t i;

        /* Original (0x5283A5-0x528407): DL above 26 is "invalid drive" -
         * status word +0x38 = FFFF, AH=0, AL=0x0F.  Success (0x5284D5) writes
         * the ASCIIZ at DS:SI through 0x5DCA30, marks +0x3A, AH=0, AL=1. */
        if (drive > 26) { dos_file_error(0x0F); break; }
        dos_seed();
        if (drive == 0) drive = dos_drive;      /* 0 = default drive */
        strncpy(path, dos_dir[drive], sizeof path - 1);
        path[sizeof path - 1] = 0;              /* other drives: root or 3Bh */
        p = seg_off_to_phys(cpu.sr[DS], cpu.r[SI]);
        for (i = 0; path[i] && i < 63; i++) poke8(p + (uint32_t)i, path[i]);
        poke8(p + (uint32_t)i, 0);
        cpu.r[0] = 0x0001;
        cpu.flags &= ~F_CF;
        break; }

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

    case 0x2A:                              /* get system date */
        /* CONFIRMED (0x52793A): the arm calls 0x40140C (VB Now), 0x401098
         * (DateValue) and 0x4012DC (year), then 0x401050 for month/day, and
         * stores five fields: CH year-hi (+0xBE), CL year-lo (+0xBF),
         * DH month (+0xC1), DL day (+0xC0), AL day-of-week (+0xBA) - exactly
         * the documented "CX = year, DH = month, DL = day, AL = day of week".
         * The clone used to return a frozen 2024-01-01. */
        {
            time_t now = time(NULL);
            const struct tm *lt = localtime(&now);
            int year = lt ? lt->tm_year + 1900 : 1980;
            int mon  = lt ? lt->tm_mon + 1 : 1;
            int day  = lt ? lt->tm_mday : 1;
            int dow  = lt ? lt->tm_wday : 0;      /* 0 = Sunday */
            cpu.r[CX] = (uint16_t)year;
            cpu.r[DX] = (uint16_t)((mon << 8) | day);
            setAL((uint8_t)dow);
        }
        break;

    case 0x2C:                              /* get system time */
        /* CONFIRMED (0x527C69): same host clock, storing CH hour (+0xBE),
         * CL minute (+0xBF), DH second (+0xC1), DL hundredths (+0xC0), and
         * AL = 0 (+0xBA) - the documented "CH hour, CL minute, DH second,
         * DL 1/100 seconds". */
        {
            time_t now = time(NULL);
            const struct tm *lt = localtime(&now);
            int hour = lt ? lt->tm_hour : 0;
            int min  = lt ? lt->tm_min : 0;
            int sec  = lt ? lt->tm_sec : 0;
            int hundredths = 0;
#ifdef _WIN32
            {   SYSTEMTIME st;
                GetLocalTime(&st);
                hour = st.wHour; min = st.wMinute; sec = st.wSecond;
                hundredths = st.wMilliseconds / 10; }
#endif
            cpu.r[CX] = (uint16_t)((hour << 8) | min);
            cpu.r[DX] = (uint16_t)((sec << 8) | hundredths);
            setAL(0);
        }
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

    case 0x31:                                  /* terminate and stay resident */
        /* Original: there is no 31h arm in the dispatcher (see 30), so it
         * prints "not supported yet" and carries on.  Residency has no
         * meaning here - one program, one prompt-less run - so the clone ends
         * the program the way 4Ch does and ignores DX.  Divergence. */
        dos_exit_requested = 1;
        dos_exit_code = cpu.r[0] & 0xFF;
        cpu.halted = 1;
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
        snprintf(dos_fname[i], sizeof dos_fname[i], "%s", name);
        dos_jft_set(dos_psp, DOS_FIRST_HANDLE + i, (uint8_t)i);
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
        snprintf(dos_fname[i], sizeof dos_fname[i], "%s", name);
        dos_jft_set(dos_psp, DOS_FIRST_HANDLE + i, (uint8_t)i);
        cpu.flags &= ~F_CF;
        setAX(DOS_FIRST_HANDLE + i);
        break; }

    case 0x3E: {                                /* close handle BX */
        int i;
        FILE *f = dos_slot_file(cpu.r[BX], &i);

        if (!f) { dos_file_error(6); break; }
        fclose(f);
        dos_files[i] = NULL;
        dos_fname[i][0] = 0;
        dos_jft_set(dos_psp, cpu.r[BX], 0xFF);  /* free again in this PSP */
        cpu.flags &= ~F_CF;
        break; }

    case 0x3F: {                                /* read BX -> DS:DX, CX bytes */
        unsigned char buf[512];
        uint32_t p, n, done = 0;
        int i, h = cpu.r[BX];
        FILE *f;

        /* Handle 0 is the console: DOS stops at CR and keeps it in the
         * buffer, and echoes what it read - but only to a console, so a
         * redirected stream is read silently (FINDINGS 33). */
        if (h == 0) {
            int echo = dos_stdin_console();
            p = seg_off_to_phys(cpu.sr[DS], cpu.r[DX]);
            n = cpu.r[CX];
            if (p + n > (uint32_t)0x100000) { dos_file_error(5); break; }
            while (done < n) {
                int c = kb_pop();
                if (c < 0) break;
                poke8(p + done, (uint8_t)c);
                done++;
                if (c == 0x0D) {
                    if (echo) dos_putc('\n');
                    break;
                }
                if (echo) dos_putc(c);
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
        if (h == 1) {                           /* stdout: maybe redirected */
            for (k = 0; k < n; k++) stdout_byte(peek8(p + k));
            if (dos_out_file) fflush(dos_out_file);
            else dos_flush(stdout);
            cpu.flags &= ~F_CF;
            setAX((int)n);
            break;
        }
        if (h == 2) {                           /* stderr */
            for (k = 0; k < n; k++) dos_putc(peek8(p + k));
            dos_flush(stderr);
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

    case 0x39:                                  /* create directory */
    case 0x3A: {                                /* remove directory */
        char name[128];
        int r;

        if (dos_guest_name(name, sizeof name) != 0) { dos_file_error(3); break; }
        r = (ah == 0x39) ? mkdir(name) : rmdir(name);
        if (r != 0) {
            /* DOS reports 3 when the path does not exist and 5 for every
             * other refusal (already there, not empty, is a file).  The
             * CF + code convention is the clone's, see 21.3. */
            dos_file_error(errno == ENOENT ? 3 : 5);
            break;
        }
        cpu.flags &= ~F_CF;
        break; }

    case 0x44: {                                /* device I/O control (AL) */
        /* Only the subfunctions that are meaningful for a console or an
         * ordinary file: 00/01 get/set device information, 06/07 get/set
         * input status, 08/09 test-ability, 0A test-ability.  Anything else
         * is reported through 01 (invalid function). */
        int sub = cpu.r[AX] & 0xFF;
        int h   = cpu.r[BX];
        int dev = (h == 0 || h == 1 || h == 2);

        switch (sub) {
        case 0x00:                              /* get device information */
            /* bit7 = character device, bit0 = stdin, bit1 = stdout, bit2 =
             * CON, bit5 = not binary, so a console handle answers 0x80D3 and
             * a disk file answers 0x0000. */
            cpu.r[DX] = dev ? (uint16_t)(0x80D3) : 0;
            setAX(0);
            cpu.flags &= ~F_CF;
            break;
        case 0x01:                              /* set device information */
            setAX(0);
            cpu.flags &= ~F_CF;
            break;
        case 0x06: case 0x07:                   /* input status */
            if (!dev) { dos_file_error(6); break; }
            if (!dos_stdin_console()) kb_refill();
            setAThi(kb_head != kb_tail ? 0xFF : 0x00);
            cpu.r[DX] = 0;
            cpu.flags &= ~F_CF;
            break;
        case 0x08: case 0x0A:                   /* is the handle removable */
            if (!dev) { dos_file_error(6); break; }
            setAX(0);
            cpu.flags &= ~F_CF;
            break;
        case 0x09:                              /* is it a remote device */
            if (!dev) { dos_file_error(6); break; }
            cpu.r[DX] = 0;                     /* local */
            setAX(0);
            cpu.flags &= ~F_CF;
            break;
        default:
            dos_file_error(1);                  /* invalid function */
            break;
        }
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

    case 0x2E:                                  /* set verify flag (AL) */
        /* CONFIRMED: arm at 0x527DB9 -> FUN_005e0860, whose whole body is
         * `mov cl,[eax]; mov ds:0x621CD0,cl`.  One byte, nothing else. */
        dos_verify_flag = (unsigned char)(cpu.r[AX] & 0xFF);
        cpu.flags &= ~F_CF;
        break;

    case 0x54:                                  /* get verify flag */
        /* CONFIRMED: arm at 0x529E15 -> FUN_005e0870, `mov al,ds:0x621CD0`,
         * stored back through +0xBA. */
        cpu.flags &= ~F_CF;
        setAL(dos_verify_flag);
        break;

    case 0x17: {                                /* rename DS:DX -> ES:DI */
        /* CONFIRMED arm at 0x5277C0 (calls FUN_005e33f0/FUN_005dc390, then
         * AL -> +0xBA).  The clone had no rename at all. */
        char old[128], newn[128];
        uint16_t saved_ds = cpu.sr[DS];
        uint16_t saved_dx = cpu.r[DX];

        if (dos_guest_name(old, sizeof old) != 0) { dos_file_error(3); break; }
        /* ES:DI names the new path, but dos_guest_name reads DS:DX, so point
         * DS at ES *and* DX at DI before calling it a second time. */
        cpu.sr[DS] = cpu.sr[ES];
        cpu.r[DX] = cpu.r[DI];
        if (dos_guest_name(newn, sizeof newn) != 0) {
            cpu.sr[DS] = saved_ds;
            cpu.r[DX] = saved_dx;
            dos_file_error(3);
            break;
        }
        cpu.sr[DS] = saved_ds;
        cpu.r[DX] = saved_dx;
        if (rename(old, newn) != 0) { dos_file_error(3); break; }
        cpu.flags &= ~F_CF;
        setAX(0);
        break; }

    case 0x57: {                                /* get/set file date+time */
        /* CONFIRMED arm at 0x52A182 -> FUN_005e2e50 (821 bytes), which walks
         * the handle table at ds:0x6214D0 and touches a host timestamp.
         * AL=00 get, AL=01 set.  CX = time, DX = date.
         *   time: bits 15-11 hour, 10-5 min, 4-0 sec/2
         *   date: bits 15-9 year-1980, 8-5 month, 4-0 day */
        FILE *f = dos_slot_file(cpu.r[BX], NULL);
        int sub = cpu.r[AX] & 0xFF;

        if (!f) { dos_file_error(6); break; }
        if (sub == 0x00) {                      /* get */
            struct _stat st;
            int i = cpu.r[BX] - DOS_FIRST_HANDLE;

            if (i < 0 || i >= DOS_FILES || !dos_fname[i][0] ||
                _stat(dos_fname[i], &st) != 0) { dos_file_error(6); break; }
            {
                const struct tm *tm = localtime(&st.st_mtime);
                int tt = 0, dd = 0;
                if (tm) {
                    tt = ((tm->tm_hour & 0x1F) << 11) |
                         ((tm->tm_min & 0x3F) << 5) |
                         ((tm->tm_sec / 2) & 0x1F);
                    dd = (((tm->tm_year + 1900 - 1980) & 0x7F) << 9) |
                         (((tm->tm_mon + 1) & 0x0F) << 5) |
                         (tm->tm_mday & 0x1F);
                }
                cpu.r[CX] = (uint16_t)tt;
                cpu.r[DX] = (uint16_t)dd;
            }
            cpu.flags &= ~F_CF;
            setAX(0);
            break;
        }
        if (sub == 0x01) {                      /* set */
            /* The clone does not apply a settable stamp to the host file,
             * so it must not pretend: SET succeeds silently (DOS programs
             * expect CF=0) but a following GET returns the real mtime. */
            cpu.flags &= ~F_CF;
            setAX(0);
            break;
        }
        dos_file_error(1);                      /* bad subfunction */
        break; }

    case 0x0C:                                  /* flush then invoke AH=AL */
        /* CONFIRMED from the arm at 0x5275BC.  It is NOT a file flush:
         *   0x5275DA  mov WORD PTR ds:0x621270,0   -> clear the key buffer
         *   0x527636  call [edx+0x7b0]             -> host console flush
         *   0x5276A7..0x527706                     -> test AL against 6,7,8,0A
         *   0x527715  mov [ecx+0xbb],al            -> AH = AL
         *   0x527749  call [eax+0x9d4]             -> re-dispatch INT 21h
         * The AL values 06/07/08/0A are the direct-console I/O services, and
         * those are skipped (they read keys, which a flushed buffer must not
         * consume), so only the other AH values are re-entered. */
        kb_head = kb_tail = 0;                  /* ds:0x621270 = 0 */
        {
            int al = cpu.r[AX] & 0xFF;
            if (al != 0x06 && al != 0x07 && al != 0x08 && al != 0x0A) {
                cpu.r[AX] = (uint16_t)((al << 8) | (cpu.r[AX] & 0x00FF));
                int21();                        /* re-dispatch on AH=AL */
            }
        }
        break;

    case 0x0D:                                  /* disk reset: pure no-op */
        /* CONFIRMED: the arm at 0x52775B is `jmp 0x52a52a` and nothing else -
         * it goes straight to the epilogue.  An earlier version of the clone
         * flushed every open write handle here, which the original does not
         * do.  Disk reset means "commit buffered writes", and emu8086 writes
         * straight through, so there is nothing to commit. */
        break;

    case 0x23:                                  /* get break flag */
        /* CONFIRMED arm at 0x52A1AD, a single word store into [eax+0x7C0].
         * DOS reports 0 = break checking off. */
        setAL(0);
        break;

    case 0x33:                                  /* get/set Ctrl-Break */
        /* CONFIRMED arm at 0x52A21E (word at [edx+0xB4C], calls 0x60A910).
         * AL=00 read -> DL, AL=01 write from DL.  Distinct from INT 33h,
         * which is the mouse. */
        {
            static unsigned char ctrl_break = 0;
            if ((cpu.r[AX] & 0xFF) == 0x00) {
                cpu.r[DX] = (uint16_t)((cpu.r[DX] & 0xFF00) | ctrl_break);
            } else if ((cpu.r[AX] & 0xFF) == 0x01) {
                ctrl_break = (unsigned char)(cpu.r[DX] & 0xFF);
            } else {
                setAL(0xFF);
                break;
            }
            setAL(0);
        }
        break;

    default:
        /* Original: an AH with no arm falls through the whole compare chain
         * into the same "not supported yet" report the other services use
         * (FINDINGS 30), leaving the registers alone and returning. */
        unsupported_ah("INT 21h", ah);
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
#define VIDEO_PAGES 8                  /* mode 3 reserves 8 text pages */
#define VIDEO_PAGE_BYTES 4096          /* B800:0000 + page*4096 */
/* CONFIRMED from the original: INT 10h AH=02 (0x520A4D) hands BH, DL and DH
 * to [obj+0x760] and AH=03 (0x520B27) asks [obj+0x768]/0x764/0x784/0x788 for
 * page BH's row, column and cursor shape.  BH is an argument, so every page
 * keeps its own cursor - one shared pair loses page 0's position as soon as
 * page 1 is touched. */
static int cur_row[VIDEO_PAGES], cur_col[VIDEO_PAGES], cur_page;
/* AH=01 stores the cursor shape and AH=03 must hand the same value back in
 * CX: [obj+0x788] keeps CH (0x520DBC) and [obj+0x784] keeps CL (0x520D0C).
 * The real BIOS default is 0607, and mode 3 sets that, so the clone starts
 * there instead of reporting a fixed 0607 forever. */
static uint16_t cursor_shape = 0x0607;
static int gfx_mode;                   /* 0 = text, 1 = CGA 320x200x4 */

/* CGA order: even byte = character, odd byte = attribute.  One array per
 * page, so AH=05 can select a page and the guest can fill 0xB800 windows
 * itself. */
static unsigned char vram[VIDEO_PAGES][VIDEO_PAGE_BYTES];
static int vram_ready;

static void vram_init(void)
{
    int p, i;
    if (vram_ready) return;
    vram_ready = 1;
    for (p = 0; p < VIDEO_PAGES; p++)
        for (i = 0; i < VIDEO_COLS * VIDEO_ROWS; i++) {
            vram[p][i * 2]     = ' ';       /* mode 3 clears to blank/0x07 */
            vram[p][i * 2 + 1] = 0x07;
        }
}

/* Text pages start at page*4096 in a 32K window: page 0 at B800:0000,
 * page 1 at +4096, and so on.  The guest writes those windows directly, so
 * the model reads and writes the same offsets the guest does. */
static unsigned char *vram_page_base(int page)
{
    if (page < 0) page = 0; else if (page >= VIDEO_PAGES) page = VIDEO_PAGES - 1;
    return vram[page];
}

/* Cell at row/col on the given page (the clone's current page).  A guest can
 * set the cursor out of range through AH=02, so clamp rather than wander off
 * the array. */
static unsigned char *vram_cell_page(int page, int row, int col)
{
    vram_init();
    if (row < 0) row = 0; else if (row >= VIDEO_ROWS) row = VIDEO_ROWS - 1;
    if (col < 0) col = 0; else if (col >= VIDEO_COLS) col = VIDEO_COLS - 1;
    return &vram_page_base(page)[(row * VIDEO_COLS + col) * 2];
}

static unsigned char *vram_cell(int row, int col)
{
    return vram_cell_page(cur_page, row, col);
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
    int p = cur_page;
    cur_col[p] += n;
    while (cur_col[p] >= VIDEO_COLS) {
        cur_col[p] -= VIDEO_COLS;
        cur_row[p]  = (cur_row[p] + 1) % VIDEO_ROWS;
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

/* Original fail tail for an AH that has no arm: it logs
 * "INT 10h, AH=0" + Hex(AH) + "h   " + "- not supported yet."
 * (0x52244F, 0x523E92, 0x524DE2 ...), then the separate hint
 * "refer to the list of supported interrupts" (0x44782C), sets the 0x62141C
 * error word and returns.  Only the first line is reproduced: the hint is
 * noise on a terminal, and the error word has no consumer outside the
 * dispatcher (FINDINGS 24.2).  Notably these paths do NOT stop the machine. */
static void unsupported_ah(const char *service, int ah)
{
    /* On stderr so a guest's own output stays byte-exact on stdout, but
     * flushed on its own so a caller reading them combined (and a person
     * watching) sees the report in the order it happened. */
    fprintf(stderr, "%s, AH=%02Xh - not supported yet.\n", service, ah);
    fflush(stderr);
}

/* Mode 13h (VGA 320x200x256) linear: one byte per pixel, 320 bytes a row,
 * 64000 bytes, at A000:0000.  Stored in its own buffer because it does not
 * fit a text page and does not overlap B800. */
static unsigned char vga13[64000];

/* AH=0Ch/0Dh: CGA 320x200 pixel store/fetch.  In that mode each byte covers
 * four pixels, 80 bytes per row, 8000 bytes per page, and pages are 8K apart
 * inside the B800 window.  Returns -1 when the mode is not 320x200, which is
 * what the original reports as unsupported.  colour < 0 reads instead. */
static int cga_pixel(int page, int x, int y, int colour)
{
    unsigned char *base;
    unsigned char *p;
    int ofs, shift, old;

    if (x < 0 || x >= 320 || y < 0 || y >= 200) return -1;

    if (gfx_mode == 2) {                        /* mode 13h, one byte/pixel */
        if (colour < 0) return vga13[y * 320 + x];
        vga13[y * 320 + x] = (unsigned char)colour;
        return 0;
    }
    if (gfx_mode != 1) return -1;
    if (page < 0 || page >= 2) page = 0;
    base = vram_page_base(page);
    ofs   = y * 80 + (x >> 2);
    shift = (3 - (x & 3)) * 2;
    p     = &base[ofs];
    old   = (*p >> shift) & 3;
    if (colour < 0) return old;
    *p = (unsigned char)((*p & ~(3 << shift)) | ((colour & 3) << shift));
    return old;
}

static void int10(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;

    switch (ah) {
    case 0x00:                                  /* set video mode */
        /* Only mode is tracked, not the full mode table: mode 4/5 put the
         * clone in the CGA 320x200 four-colour model that AH=0C/0D address,
         * anything else is text. */
        /* Mode 13h is VGA, not CGA: 320x200x256 means one byte a pixel. */
        switch (cpu.r[AX] & 0xFF) {
        case 0x04: case 0x05: gfx_mode = 1; break;   /* CGA 4-colour */
        case 0x06: case 0x13: gfx_mode = 2; break;   /* one byte a pixel */
        default:              gfx_mode = 0; break;   /* text */
        }
        /* A mode set clears every page's cursor, as the BIOS does. */
        {
            int i;
            for (i = 0; i < VIDEO_PAGES; i++) cur_row[i] = cur_col[i] = 0;
        }
        cursor_shape = 0x0607;
        cur_page = 0;
        break;
    case 0x01:                                  /* set cursor shape */
        /* CONFIRMED: the shape reaches [obj+0x788] (CH) and [obj+0x784]
         * (CL) and AH=03 hands it back in CX (0x520D0C/0x520DBC).  The clone
         * has no blinking cursor to draw, but it must not invent 0607. */
        cursor_shape = cpu.r[CX];
        break;
    case 0x02:                                  /* set cursor position */
        {
            int p = (cpu.r[BX] >> 8) & 0xFF;    /* BH = page */
            if (p >= VIDEO_PAGES) p = 0;
            cur_row[p] = (cpu.r[DX] >> 8) & 0xFF;
            cur_col[p] = cpu.r[DX] & 0xFF;
        }
        break;
    case 0x03:                                  /* read cursor position */
        {
            int p = (cpu.r[BX] >> 8) & 0xFF;    /* BH = page */
            if (p >= VIDEO_PAGES) p = 0;
            cpu.r[CX] = cursor_shape;
            cpu.r[DX] = (uint16_t)((cur_row[p] << 8) | cur_col[p]);
        }
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
            unsigned char *cell = vram_cell(cur_row[cur_page], cur_col[cur_page]);
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
            for (i = 0; i < n && cur_col[cur_page] + i < VIDEO_COLS; i++) {
                unsigned char *cell = vram_cell(cur_row[cur_page], cur_col[cur_page] + i);
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
                unsigned char *cell = vram_cell(cur_row[cur_page], cur_col[cur_page]);
                cell[0] = (unsigned char)c;
                cell[1] = 0x07;
            }
            video_putc(c);
            /* Teletype moves the cursor: CR to column 0, LF down a row, BS
             * back one column, anything else one column forward. */
            if (c == 0x0D)                        cur_col[cur_page] = 0;
            else if (c == 0x0A)                   cur_row[cur_page] = (cur_row[cur_page] + 1) % VIDEO_ROWS;
            else if (c == 0x08) { if (cur_col[cur_page] > 0) cur_col[cur_page]--; }
            else                                  cursor_advance_cols(1);
        }
        break;
    case 0x0C: case 0x0D:                       /* write / read pixel */
        {
            int color = cpu.r[AX] & 0xFF;
            int page  = (cpu.r[BX] >> 8) & 0xFF;   /* BH, named so it cannot
                                                    * alias another register */
            int y     = cpu.r[CX];                /* row */
            int x     = cpu.r[DX];                /* column */
            int r = cga_pixel(page, x, y, ah == 0x0C ? color : -1);
            if (r < 0) {
                /* cga_pixel only models modes 4/5/13.  In text mode the
                 * original's arm still runs and reports nothing, so a guest
                 * that pokes a pixel must not see a diagnostic here. */
                if (gfx_mode != 0) unsupported_ah("INT 10h", ah);
            } else if (ah == 0x0D) {
                /* AH=0Dh returns the pixel in AL (BIOS: "AL = color").  The
                 * clone used to throw the result away, so a read-modify-write
                 * guest saw the old AL. */
                cpu.r[AX] = (uint16_t)((cpu.r[AX] & 0xFF00) | (r & 0xFF));
            }
        }
        break;
    case 0x05:                                  /* select active page */
        cur_page = cpu.r[AX] & 0xFF;
        if (cur_page >= VIDEO_PAGES) cur_page = VIDEO_PAGES - 1;
        break;
    case 0x10:                                  /* palette (mode 4 only) */
        /* AL=0 set border, 1 set palette (BL), 2 set all, 3 toggle blink.
         * The clone models mode 3 only, so these change nothing visible; the
         * point is that a guest which calls them keeps running. */
        break;
    case 0x0F:                                  /* get video mode */
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0x00FF) | (0x03 << 8));
        cpu.r[3] = 0;
        break;
    case 0x11:                                  /* character generator */
        /* CONFIRMED: the original's whole arm at 0x522CD8 is
         * `cmp [ebp-0x270],al / jne next / jmp 0x523B50` - it jumps to the
         * epilogue and does NOTHING, silently and with no report.  The clone
         * used to print "not supported yet.", which put a line on stderr the
         * original never writes. */
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
            cur_row[cur_page] = (cpu.r[DX] >> 8) & 0xFF;
            cur_col[cur_page] = cpu.r[DX] & 0xFF;
            for (i = 0; i < cells; i++) {
                int attr = 0x07;
                unsigned char *cell;
                ch = peek8(p++);
                /* With bit1 set every second byte is an attribute: consume it
                 * and give it to the cell instead of printing it. */
                if (pairs) attr = peek8(p++);
                cell = vram_cell(cur_row[cur_page], cur_col[cur_page] + written);
                cell[0] = (unsigned char)ch;
                cell[1] = (unsigned char)attr;
                video_putc(ch);
                written++;
            }
            if (advance) cursor_advance_cols(written);
        }
        break;
    default:
        unsupported_ah("INT 10h", ah);
        break;
    }
}

/* ---- INT 16h (keyboard) --------------------------------------------- */
static void int16(void)
{
    int ah = (cpu.r[0] >> 8) & 0xFF;

    /* INT 16h returns the scan code in AH and the character in AL, which the
     * original documents as "AH = BIOS scan code. AL = ASCII character." and
     * implements through the ds:0x621264 pairs (FINDINGS 40). */
    switch (ah) {
    case 0x00: case 0x10: {
        int c = kb_pop();
        if (c < 0) { c = 0; kb_last_scan = 0; }
        cpu.r[0] = (uint16_t)((kb_last_scan << 8) | (c & 0xFF));
        cpu.flags &= ~F_ZF;
        break; }
    case 0x01: case 0x11: {                 /* peek: wait, but do not consume */
        int c = kb_peek();                  /* blocks on a terminal */
        if (c >= 0) { cpu.flags &= ~F_ZF;
            cpu.r[0] = (uint16_t)((kb_last_scan << 8) | (c & 0xFF)); }
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
        unsupported_ah("INT 13h", ah);         /* cf. fail path 5.3 */
        cpu.flags |= F_CF;
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
/* INT 15h AH=86h - wait a BCD-free interval in microseconds.
 *
 * CONFIRMED from the original (0x524112): ticks = DX + CX*0x10000
 * (imul eax,eax,0x10000 at 0x52421D), then divided by 1000.0 and handed to
 * Sleep(), and the arm clears CF/AH on a successful wait.  The clone does
 * actually sleep, because a guest that asks for one second expects one, and
 * examples/timer.asm loops on `jc stop` after every call - answering with
 * CF set would end it after the first character. */
static void int15(void)
{
    int ah = (cpu.r[AX] >> 8) & 0xFF;

    cpu.flags &= ~F_CF;
    if (ah == 0x86) {
        uint32_t ticks = cpu.r[DX] + ((uint32_t)cpu.r[CX] << 16);
        uint32_t ms = ticks / 1000;
        /* The clone really waits, because a guest asking for one second
         * expects one (examples/timer counts its characters).  Two things
         * keep that from being a trap: the wait is capped, and MUE_NOSLEEP
         * turns it off entirely, which the example sweep needs because
         * traffic_lights.asm loops on five-second delays. */
        if (getenv("MUE_NOSLEEP")) ms = 0;
        if (ms > 1000) ms = 1000;
#ifdef _WIN32
        if (ms) Sleep(ms);
#else
        if (ms) usleep((useconds_t)ms * 1000);
#endif
        setAThi(0);
        return;
    }
    if (ah == 0x00 || ah == 0x01 || ah == 0x91 || ah == 0x90) {
        setAThi(0);
        return;
    }
    cpu.flags |= F_CF;
    setAThi(0x86);                          /* function not supported */
}

/* Returns 1 if the interrupt was serviced here, 0 to forward to the IVT. */
int dos_interrupt(int n)
{
    switch (n) {
    case 0x00: return 0;                /* divide error: let the guest see it */
    case 0x10: int10(); return 1;
    case 0x11: cpu.r[0] = 0x0001; return 1;   /* equipment list */
    case 0x12: cpu.r[0] = 0x0280; return 1;   /* conventional memory size */
    case 0x15: int15(); return 1;    case 0x16: int16(); return 1;
    case 0x17: setAThi(0x90); return 1;        /* printer not ready */
    case 0x1A: int1a(); return 1;
    case 0x19:                          /* reboot */
        /* CONFIRMED (0x525549-0x525581): the arm writes 0xFFFF to the
         * register block's +0x4C word and to ds:0x621218, which is the halt
         * marker the run loop tests, then logs "restart interrupted...".
         * So a boot program that has finished (examples/timer ends with
         * int 19h) stops instead of running into whatever follows. */
        cpu.halted = 1;
        cpu.exit_code = 0;
        return 1;
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
