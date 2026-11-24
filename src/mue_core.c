/* mue_core.c - in-process bridge to the verified emulator core, plus the
 * disassembler the disassembly panel needs.
 *
 * The core (cpu.h / exec.c / mem.c / dos.c / asm_*.c) is the same code the
 * mue.exe CLI links, so anything the test suite verifies about the CLI holds
 * for the UI as well. What is added here is only the glue: reset-and-load, a
 * single step, and a read-only view of memory for the panels.
 *
 * The disassembler is separate from the executor on purpose. The executor
 * decodes and acts; a disassembler must decode WITHOUT acting, and must
 * calculate an instruction's length so the next line starts in the right
 * place. Sharing the decode path would mean either side-effecting the CPU or
 * threading a "don't actually run this" flag through every opcode arm.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#  include <direct.h>
#  define mue_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define mue_mkdir(p) mkdir(p, 0777)
#endif

#include "cpu.h"
#include "dos.h"
#include "asm.h"
#include "mue_core.h"
#include "mue_paths.h"

/* ---- register naming ------------------------------------------------- */

static const char *RN16[8] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
static const char *RN8[8]  = { "AL","CL","DL","BL","AH","CH","DH","BH" };
static const char *RSEG[4] = { "ES","CS","SS","DS" };
static const char *CC[16]  = {
    "O","NO","B","NB","Z","NZ","BE","A","S","NS","P","NP","L","GE","LE","G"
};

/* Whether a guest has been assembled and loaded in this session. */
static int loaded = 0;
int core_loaded(void) { return loaded; }

static uint32_t P(uint16_t seg, uint16_t off)
{
    return ((uint32_t)seg << 4) + off;
}

/* ---- load ------------------------------------------------------------- */

/* Step-back journal accessors, defined in mem.c. */
void     sb_begin(void);
void     sb_end(void);
int      sb_count(void);
uint32_t sb_phys_at(int i);
uint8_t  sb_old_at(int i);
void     core_hist_clear(void);

/* ---- build artefacts ---------------------------------------------------
 *
 * emu8086 does not merely run a program: every build leaves five files in
 * MyBuild\, and its listing and symbol-table windows read them back
 * (re/FILESYSTEM.md section 2).  The binary carries a trailing underscore --
 * `hello.com_` -- and the other four are named after it:
 *
 *   hello.com_          the image the emulator loads
 *   hello.com_.debug    per-line address record, for the listing/step-back
 *   hello.com_.list     machine code beside the source
 *   hello.com_.symbol   the symbol table
 *   hello.com_.~asm     the source as it was at build time
 *
 * The same five names without the underscore are what an explicit export
 * writes, which is why the dialogs try the underscored name first.
 */
static void split_stem(const char *path, char *stem, int stemsz)
{
    const char *b = path ? path : "";
    const char *s1 = strrchr(b, '\\');
    const char *s2 = strrchr(b, '/');
    const char *dot;
    int n;
    if (s1 && (!s2 || s1 > s2)) b = s1 + 1;
    else if (s2) b = s2 + 1;
    dot = strrchr(b, '.');
    n = dot ? (int)(dot - b) : (int)strlen(b);
    if (n <= 0) { snprintf(stem, (size_t)stemsz, "untitled"); return; }
    if (n >= stemsz) n = stemsz - 1;
    memcpy(stem, b, (size_t)n);
    stem[n] = 0;
}

static int sym_cmp(const void *a, const void *b)
{
    const label_t *x = (const label_t *)a, *y = (const label_t *)b;
    return strcmp(x->name, y->name);
}

static void write_list(FILE *f, const char *name, const uint8_t *bin, int n)
{
    int count = 0, i, k;
    const asm_lineinfo *li = asm_last_lines(&count);
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);

    fprintf(f, "MUE GENERATED LISTING. MACHINE CODE <- SOURCE.\n\n");
    fprintf(f, "%s -- mue assembler\n\n", name);
    if (tmv)
        fprintf(f, "[ %02d/%02d/%04d  --  %02d:%02d:%02d ]\n\n",
                tmv->tm_mon + 1, tmv->tm_mday, tmv->tm_year + 1900,
                tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    fprintf(f, "===================================================================================================\n");
    fprintf(f, "[LINE]     LOC: MACHINE CODE                          SOURCE\n");
    fprintf(f, "===================================================================================================\n\n");

    for (i = 0; i < count; i++) {
        const char *src = asm_last_source(li[i].line);
        char bytes[64];
        int used = 0;
        if (!src) src = "";
        bytes[0] = 0;
        for (k = 0; k < li[i].len && k < 12; k++) {
            int off = li[i].off + k;
            if (off >= 0 && off < n)
                used += snprintf(bytes + used, sizeof bytes - (size_t)used,
                                 "%02X ", bin[off]);
        }
        if (li[i].len > 0)
            fprintf(f, "[%4d]    %04X: %-38s%s\n", li[i].line, li[i].addr,
                    bytes, src);
        else
            fprintf(f, "[%4d]        : %-38s%s\n", li[i].line, "", src);
        for (k = 12; k < li[i].len; k += 12) {
            int j;
            used = 0;
            bytes[0] = 0;
            for (j = k; j < li[i].len && j < k + 12; j++) {
                int off = li[i].off + j;
                if (off >= 0 && off < n)
                    used += snprintf(bytes + used, sizeof bytes - (size_t)used,
                                     "%02X ", bin[off]);
            }
            fprintf(f, "                %s\n", bytes);
        }
    }
}

static void write_symbols(FILE *f, const char *name)
{
    int count = 0, i;
    const label_t *lab = asm_last_labels(&count);
    label_t *copy = count ? (label_t *)malloc(sizeof(label_t) * (size_t)count) : NULL;

    fprintf(f, "< THE SYMBOL TABLE >  %s  --  mue assembler\n", name);
    fprintf(f, "===================================================================================================\n");
    fprintf(f, "Name                     \tOffset    \tSize      \tType      \tSegment\n");
    fprintf(f, "===================================================================================================\n");
    if (!copy) return;
    memcpy(copy, lab, sizeof(label_t) * (size_t)count);
    qsort(copy, (size_t)count, sizeof(label_t), sym_cmp);
    for (i = 0; i < count; i++) {
        char up[64], offs[16], szs[16];
        const char *type = copy[i].is_proc ? "PROC"
                         : (copy[i].size > 0 ? "VAR" : "LABEL");
        int size = copy[i].is_proc ? -1 : (copy[i].size > 0 ? copy[i].size : -1);
        int k;
        for (k = 0; copy[i].name[k] && k < (int)sizeof up - 1; k++)
            up[k] = (char)toupper((unsigned char)copy[i].name[k]);
        up[k] = 0;
        snprintf(offs, sizeof offs, "%05X", copy[i].value);
        snprintf(szs, sizeof szs, "%d", size);
        /* emu8086 pads every column to ten characters and upper-cases the
         * name, because its symbol table is case-insensitive. */
        fprintf(f, "%-25s\t%-10s\t%-10s\t%-10s\t(NOSEG)\n",
                up, offs, szs, type);
    }
    free(copy);
}

/* Our own record for the step-back/listing panel: one line per emitted
 * statement plus the image facts.  emu8086's .debug is its own binary format;
 * mue writes text so the file is inspectable and versioned. */
static void write_debug(FILE *f, const char *name, int binlen, int org)
{
    int count = 0, i;
    const asm_lineinfo *li = asm_last_lines(&count);
    fprintf(f, "mue debug info 1\n");
    fprintf(f, "image %s\n", name);
    fprintf(f, "org %04X\n", org);
    fprintf(f, "bytes %d\n", binlen);
    for (i = 0; i < count; i++)
        if (li[i].len > 0)
            fprintf(f, "line %d %04X %d\n", li[i].line, li[i].addr, li[i].len);
}

static char last_image_name[260];
/* The image of the last successful assembly.  Compile shows the assembler
 * status window *before* asking where the .bin goes, so the bytes have to
 * survive until the Save-As box returns. */
static uint8_t last_image[0x10000];
static int last_image_len;
static char last_src[65536];

const char *core_last_image_name(void)
{
    return last_image_name;
}

/* Write the image and its four sidecars.  `to` is the file chosen in
 * Compile's Save-As box; NULL asks for the MyBuild\<stem>.<ext>_ convention
 * (what Emulate writes). */
static void write_build(const char *src_path, const char *src,
                        const uint8_t *bin, int n, const char *to)
{
    char stem[128] = "untitled", name[260];
    char path[MUE_MAXP], img[MUE_MAXP];
    const char *dir = mue_dir_build();
    const char *ext = asm_last_was_exe() ? "exe"
                    : (asm_last_is_boot() ? "bin" : "com");
    FILE *f;

    split_stem(src_path, stem, sizeof stem);
    if (to) {
        /* emu8086 writes the image where Save-As pointed, with the same four
         * sidecars beside it and those names without the underscore. */
        const char *b = to;
        const char *s;
        char dirbuf[MUE_MAXP];
        for (s = to; *s; s++) if (*s == '\\' || *s == '/') b = s + 1;
        snprintf(name, sizeof name, "%s", b);
        snprintf(img, sizeof img, "%s", to);
        snprintf(dirbuf, sizeof dirbuf, "%s", to);
        s = strrchr(dirbuf, '\\');
        if (!s) s = strrchr(dirbuf, '/');
        if (s) { *((char *)s) = 0; mue_mkdirs(dirbuf); }
    } else {
        snprintf(name, sizeof name, "%s.%s_", stem, ext);
        if (!mue_mkdirs(dir)) return;
        snprintf(img, sizeof img, "%s\\%s", dir, name);
    }
    snprintf(last_image_name, sizeof last_image_name, "%s", name);
    mue_write_file(img, bin, n);

    /* The source as it stood at build time: that is what `.~asm` is, and it is
     * what the listing's SOURCE column is produced from. */
    snprintf(path, sizeof path, "%s.~asm", img);
    mue_write_file(path, src, (int)strlen(src));

    snprintf(path, sizeof path, "%s.list", img);
    f = fopen(path, "wb");
    if (f) { write_list(f, name, bin, n); fclose(f); }

    snprintf(path, sizeof path, "%s.symbol", img);
    f = fopen(path, "wb");
    if (f) { write_symbols(f, name); fclose(f); }

    snprintf(path, sizeof path, "%s.debug", img);
    f = fopen(path, "wb");
    if (f) { write_debug(f, name, n, asm_last_org()); fclose(f); }
}

const char *core_image_ext(void)
{
    return asm_last_was_exe() ? "exe" : (asm_last_is_boot() ? "bin" : "com");
}

static int last_image_size;

int core_last_size(void)
{
    return last_image_size;
}

int core_assemble(const char *src, const char *src_path, char *err, int errsz)
{
    char dir[512], path[600];
    FILE *f;
    uint8_t *p = NULL;
    int n = 0;
    const char *tmp = getenv("TEMP");
    int trace = getenv("MUE_TRACE") != 0;
    clock_t t0 = clock(), t1, t2, t3;
    (void)src_path;
    if (err && errsz) err[0] = 0;
    if (!tmp || !tmp[0]) tmp = ".";

    snprintf(dir, sizeof dir, "%s\\mue_ui_live", tmp);
    /* Creating the work directory used to cost ~600ms here because it went
     * through `system("cmd /c mkdir")`: every Emulate/Compile click spawned a
     * shell. A direct mkdir is microseconds, and a failure is harmless (the
     * directory already exists, or the fopen below reports the real error). */
    mue_mkdir(dir);
    snprintf(path, sizeof path, "%s\\guest.asm", dir);
    t1 = clock();

    f = fopen(path, "wb");
    if (!f) { snprintf(err, errsz, "cannot write %s", path); return 0; }
    fputs(src, f);
    fclose(f);

    int asm_ok = asm_assemble_file(path, 1, &p, &n, err, errsz ? errsz : 0);
    t2 = clock();
    if (!asm_ok) return 0;

    /* Keep the image and its source: Compile asks where it goes only after
     * this returns (emu8086 shows the status window first), and Emulate
     * writes nothing at all. */
    last_image_len = n;
    if (n > 0) memcpy(last_image, p, (size_t)n);
    snprintf(last_src, sizeof last_src, "%s", src);
    last_image_size = n;

    memset(MEM, 0, sizeof(MEM));
    if (n > 0xFF00) n = 0xFF00;
    memcpy(MEM + 0x01100, p, (size_t)n);
    free(p);

    cpu_reset();
    cpu.sr[CS] = cpu.sr[DS] = cpu.sr[ES] = cpu.sr[SS] = 0x0100;
    cpu.r[SP]  = 0xFFFE;
    poke16(P(cpu.sr[SS], 0xFFFE), 0x0000);
    cpu.ip     = 0x0100;
    cpu.phys   = P(cpu.sr[CS], cpu.ip);
    dos_boot_psp();
    loaded = 1;
    cpu.halted = 0;
    core_hist_clear();
    t3 = clock();
    if (trace)
        fprintf(stderr, "core_assemble: mkdir %ld write+asm %ld rest %ld total %ld ms (n=%d)\n",
                (long)((t1 - t0) * 1000 / CLOCKS_PER_SEC),
                (long)((t2 - t1) * 1000 / CLOCKS_PER_SEC),
                (long)((t3 - t2) * 1000 / CLOCKS_PER_SEC),
                (long)((t3 - t0) * 1000 / CLOCKS_PER_SEC), n);
    return 1;
}

int core_load(const char *src, const char *src_path, char *err, int errsz)
{
    if (!core_assemble(src, src_path, err, errsz)) return 0;
    write_build(src_path, last_src, last_image, last_image_len, NULL);
    return 1;
}

int core_load_binary(const char *path, char *err, int errsz)
{
    FILE *f;
    uint8_t buf[0xFF00];
    size_t n;
    const char *name;
    if (err && errsz) err[0] = 0;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) {
        if (err && errsz) snprintf(err, errsz, "cannot open %s", path);
        return 0;
    }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    memset(MEM, 0, sizeof(MEM));
    if (n > 0) memcpy(MEM + 0x01100, buf, n);
    cpu_reset();
    cpu.sr[CS] = cpu.sr[DS] = cpu.sr[ES] = cpu.sr[SS] = 0x0100;
    cpu.r[SP]  = 0xFFFE;
    poke16(P(cpu.sr[SS], 0xFFFE), 0x0000);
    cpu.ip     = 0x0100;
    cpu.phys   = P(cpu.sr[CS], cpu.ip);
    dos_boot_psp();

    name = strrchr(path, '\\');
    if (!name) name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(last_image_name, sizeof last_image_name, "%s", name);
    last_src[0] = 0;
    last_image_len = (int)n;
    last_image_size = (int)n;
    if (n > 0) memcpy(last_image, buf, n);
    loaded = 1;
    cpu.halted = 0;
    core_hist_clear();
    return 1;
}

int core_save_image(const char *path)
{
    if (!path || !path[0] || last_image_len <= 0) return 0;
    write_build(NULL, last_src, last_image, last_image_len, path);
    return 1;
}

int core_code_after_end(const char *src)
{
    const char *p = src;
    int seen_end = 0;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        const char *s = p;
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t i;
        while (*s == ' ' || *s == '\t') { s++; len--; }
        if (!seen_end) {
            if (len >= 3 &&
                (s[0] == 'e' || s[0] == 'E') &&
                (s[1] == 'n' || s[1] == 'N') &&
                (s[2] == 'd' || s[2] == 'D') &&
                (len == 3 || s[3] == ' ' || s[3] == '\t' ||
                 s[3] == '\r' || s[3] == ';'))
                seen_end = 1;
        } else {
            for (i = 0; i < len; i++)
                if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
                    s[i] != ';') return 1;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return 0;
}

/* ---- stepping --------------------------------------------------------- */

/* One history entry: the CPU exactly as it was before the step, plus the
 * bytes that step overwrote. Registers live in cpu_t, so copying the struct
 * restores IP, flags, the fetch cursor and the halt latch in one go. */
typedef struct {
    cpu_t     c;
    int       n;
    uint32_t *a;
    uint8_t  *v;
} hist_t;

#define HIST_MAX 400
static hist_t hist[HIST_MAX];
static int    hn;

void core_hist_clear(void)
{
    int i;
    for (i = 0; i < hn; i++) {
        free(hist[i].a);
        free(hist[i].v);
    }
    hn = 0;
}

int core_back_depth(void) { return hn; }

static void hist_push(const cpu_t *before)
{
    int n = sb_count(), i;
    hist_t *h;

    if (hn == HIST_MAX) {
        free(hist[0].a);
        free(hist[0].v);
        memmove(&hist[0], &hist[1], sizeof(hist_t) * (HIST_MAX - 1));
        hn--;
    }
    h = &hist[hn++];
    h->c = *before;
    h->n = n;
    h->a = n ? (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n) : NULL;
    h->v = n ? (uint8_t *)malloc((size_t)n) : NULL;
    for (i = 0; i < n; i++) {
        h->a[i] = sb_phys_at(i);
        h->v[i] = sb_old_at(i);
    }
}

int core_step(void)
{
    cpu_t before;
    int ok;

    if (cpu.halted) return 0;
    before = cpu;
    sb_begin();
    ok = cpu_step();
    sb_end();
    if (!ok) return 0;
    hist_push(&before);
    return 1;
}

int core_step_back(void)
{
    hist_t *h;
    int i;

    if (hn <= 0) return 0;
    h = &hist[--hn];
    /* Undo memory in reverse, then put the CPU back exactly as it was. */
    for (i = h->n - 1; i >= 0; i--) MEM[h->a[i] & 0xFFFFF] = h->v[i];
    cpu = h->c;
    free(h->a);
    free(h->v);
    h->a = NULL;
    h->v = NULL;
    h->n = 0;
    return 1;
}

int core_run(int max)
{
    int n = 0;
    /* A run is not journalled, and keeping stale entries would let `step
     * back` rewind into a state the run already left. */
    core_hist_clear();
    while (!cpu.halted && n < max) {
        if (!cpu_step()) break;
        n++;
    }
    return n;
}


int core_halted(void) { return cpu.halted; }



void core_regs(unsigned short *r8, unsigned short *sr4,
               unsigned short *ip, unsigned short *flags, long *icount)
{
    int i;
    for (i = 0; i < 8; i++) r8[i] = cpu.r[i];
    for (i = 0; i < 4; i++) sr4[i] = cpu.sr[i];
    *ip = cpu.ip;
    *flags = cpu.flags;
    *icount = cpu.icount;
}

void core_cs_ip(unsigned short *cs, unsigned short *ip)
{
    *cs = (unsigned short)cpu.cur_seg;
    *ip = cpu.ip;
}

uint8_t core_peek8(uint32_t phys) { return peek8(phys); }
uint16_t core_peek16(uint32_t phys) { return peek16(phys); }

void core_screen_text(char *out, int outsz)
{
    int row, col, o = 0;
    int page = dos_screen_page();
    if (outsz <= 0) return;
    out[0] = 0;
    if (!dos_screen_touched() || dos_screen_mode() != 0) return;

    for (row = 0; row < 25; row++) {
        for (col = 0; col < 80; col++) {
            unsigned char ch = dos_screen_cell(page, row, col, NULL);
            if (ch < 32 || ch > 126) ch = ' ';
            if (o < outsz - 2) out[o++] = (char)ch;
        }
        /* trim the right margin so the panel does not look ragged */
        while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n') &&
               (o % 80) == 0) o--;
        if (o < outsz - 1) out[o++] = '\n';
    }
    out[o] = 0;
}

/* ====================================================================== */
/* disassembler                                                            */
/*                                                                         */
/* Length decoding is the hard part: the mnemonic is easy, but the next    */
/* line only lines up if the instruction's byte count is right. Every      */
/* operand form below accounts for its own bytes, and prefixes are summed  */
/* first. NOTHING here writes to the CPU or memory.                        */
/* ====================================================================== */

typedef struct {
    uint32_t p;       /* physical cursor          */
    uint16_t seg;     /* segment for the fetch    */
    uint16_t off;     /* starting offset          */
    int      n;       /* bytes consumed           */
    int      segov;   /* -1 or ES..DS             */
} dis_t;

static uint8_t d8(dis_t *d) { uint8_t v = peek8(d->p); d->p++; d->n++; return v; }
static uint16_t d16(dis_t *d)
{
    uint8_t lo = d8(d), hi = d8(d);
    return (uint16_t)(lo | (hi << 8));
}

/* Format a modrm operand. Returns the number of extra bytes consumed and
 * writes the operand text into out. `width` is 1 or 2 bytes. */
static void rm_str(dis_t *d, int mod, int rm, int width, char *out, int outsz)
{
    const char **rn = width == 1 ? RN8 : RN16;
    static const char *EA[8] = {
        "BX+SI", "BX+DI", "BP+SI", "BP+DI", "SI", "DI", "BP", "BX"
    };
    if (mod == 3) {
        snprintf(out, outsz, "%s", rn[rm]);
        return;
    }
    if (mod == 0 && rm == 6) {
        uint16_t a = d16(d);
        snprintf(out, outsz, "[%04X]", a);
        return;
    }
    if (mod == 0) {
        snprintf(out, outsz, "[%s]", EA[rm]);
        return;
    }
    {
        int16_t disp = (mod == 1) ? (int8_t)d8(d) : (int16_t)d16(d);
        if (disp < 0)
            snprintf(out, outsz, "[%s-%X]", EA[rm], -disp);
        else
            snprintf(out, outsz, "[%s+%X]", EA[rm], disp);
    }
}

/* Segment prefix text, e.g. "es:" when the instruction carried one. */
static const char *seg_prefix(const dis_t *d)
{
    static const char *t[4] = { "es:", "cs:", "ss:", "ds:" };
    return d->segov >= 0 ? t[d->segov] : "";
}

int core_disasm_here(char *out, int *len)
{
    return core_disasm_at(out, len);
}

/* Disassemble one instruction at an arbitrary segment:offset without running
 * it.  The disassembly pane needs this because its anchor is independent of
 * CS:IP (the user can type any address into the address field), and step-back
 * needs to re-highlight a line that is not the current one. */
int core_disasm_seg(unsigned short seg, unsigned short off, char *out, int *len)
{
    dis_t d;
    uint8_t op;
    char a[64], b[64];

    d.seg = seg;
    d.off = off;
    d.p = P(d.seg, d.off);

    d.n = 0;
    d.segov = -1;

    if (len) *len = 0;
    if (!out) return 0;
    out[0] = 0;

    /* prefixes */
    for (;;) {
        op = peek8(d.p);
        if (op == 0x26) { d.segov = ES; d8(&d); continue; }
        if (op == 0x2E) { d.segov = CS; d8(&d); continue; }
        if (op == 0x36) { d.segov = SS; d8(&d); continue; }
        if (op == 0x3E) { d.segov = DS; d8(&d); continue; }
        if (op == 0xF0 || op == 0xF2 || op == 0xF3) { d8(&d); continue; }
        break;
    }

    op = d8(&d);
    out[0] = 0;

    /* --- one-byte forms with a modrm --- */
    static const char *ALU[8] = { "add","or","adc","sbb","and","sub","xor","cmp" };
    if (op >= 0x00 && op <= 0x3D && (op & 7) < 6) {
        int alu = op >> 3, form = op & 7, width = (op & 1) ? 2 : 1;
        const char **rn = width == 1 ? RN8 : RN16;
        if (form < 4) {
            int mod = 0, rm = 0, reg = 0;
            uint8_t m = d8(&d);
            mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
            rm_str(&d, mod, rm, width, a, sizeof a);
            if (form == 0 || form == 1)
                snprintf(out, 200, "%s %s,%s%s", ALU[alu],
                         seg_prefix(&d), a, rn[reg]);
            else
                snprintf(out, 200, "%s %s%s,%s", ALU[alu], rn[reg],
                         seg_prefix(&d), a);
        } else {
            /* accumulator, immediate */
            unsigned v = width == 1 ? d8(&d) : d16(&d);
            snprintf(out, 200, "%s %s,%X", ALU[alu], rn[0], v);
        }
        goto done;
    }

    /* segment pushes / pops */
    if (op >= 0x06 && op <= 0x07) { snprintf(out,200,"push es"); goto done; }
    if (op == 0x0E) { snprintf(out,200,"push cs"); goto done; }
    if (op == 0x16) { snprintf(out,200,"push ss"); goto done; }
    if (op == 0x1E) { snprintf(out,200,"push ds"); goto done; }
    if (op == 0x17) { snprintf(out,200,"pop es"); goto done; }
    if (op == 0x1F) { snprintf(out,200,"pop ds"); goto done; }

    switch (op) {
    case 0x27: snprintf(out,200,"daa"); goto done;
    case 0x2F: snprintf(out,200,"das"); goto done;
    case 0x37: snprintf(out,200,"aaa"); goto done;
    case 0x3F: snprintf(out,200,"aas"); goto done;
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B:
        snprintf(out,200,"db %02X", op); goto done;
    case 0x6C: snprintf(out,200,"insb"); goto done;
    case 0x6D: snprintf(out,200,"insw"); goto done;
    case 0x6E: snprintf(out,200,"outsb"); goto done;
    case 0x6F: snprintf(out,200,"outsw"); goto done;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)d8(&d);
        unsigned tgt = (unsigned)(d.off + d.n + rel) & 0xFFFF;
        snprintf(out, 200, "j%s %04X", CC[op & 0xF], tgt);
        goto done;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int width = (op == 0x80 || op == 0x82) ? 1 : 2;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        unsigned v;
        if (op == 0x83) v = (unsigned)(int8_t)d8(&d) & 0xFFFF;
        else v = (width == 1) ? d8(&d) : d16(&d);
        snprintf(out, 200, "%s %s,%X", ALU[reg], a, v);
        goto done;
    }
    case 0x84: case 0x85: {
        int width = (op & 1) ? 2 : 1;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        snprintf(out, 200, "test %s,%s", a,
                 (width == 1 ? RN8 : RN16)[reg]);
        goto done;
    }
    case 0x86: case 0x87: {
        int width = (op & 1) ? 2 : 1;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        snprintf(out, 200, "xchg %s,%s", a, (width == 1 ? RN8 : RN16)[reg]);
        goto done;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        int width = (op & 1) ? 2 : 1;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        if (op == 0x88 || op == 0x89)
            snprintf(out, 200, "mov %s%s,%s", seg_prefix(&d), a,
                     (width == 1 ? RN8 : RN16)[reg]);
        else
            snprintf(out, 200, "mov %s,%s%s", (width == 1 ? RN8 : RN16)[reg],
                     seg_prefix(&d), a);
        goto done;
    }
    case 0x8C: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "mov %s,%s", a, RSEG[(m >> 3) & 3]);
        goto done;
    }
    case 0x8D: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "lea %s,%s", RN16[(m >> 3) & 7], a);
        goto done;
    }
    case 0x8E: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "mov %s,%s", RSEG[(m >> 3) & 3], a);
        goto done;
    }
    case 0x8F: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "pop %s", a);
        goto done;
    }
    case 0x90: snprintf(out,200,"nop"); goto done;
    case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
        snprintf(out, 200, "xchg ax,%s", RN16[op & 7]);
        goto done;
    case 0x98: snprintf(out,200,"cbw"); goto done;
    case 0x99: snprintf(out,200,"cwd"); goto done;
    case 0x9A: { uint16_t o = d16(&d), s = d16(&d);
        snprintf(out,200,"call %04X:%04X", s, o); goto done; }
    case 0x9B: snprintf(out,200,"wait"); goto done;
    case 0x9C: snprintf(out,200,"pushf"); goto done;
    case 0x9D: snprintf(out,200,"popf"); goto done;
    case 0x9E: snprintf(out,200,"sahf"); goto done;
    case 0x9F: snprintf(out,200,"lahf"); goto done;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int width = (op & 1) ? 2 : 1;
        uint16_t a16 = d16(&d);
        const char *r = (width == 1 ? RN8 : RN16)[0];
        if (op == 0xA0) snprintf(out,200,"mov al,[%s%04X]",seg_prefix(&d),a16);
        else if (op == 0xA1) snprintf(out,200,"mov ax,[%s%04X]",seg_prefix(&d),a16);
        else if (op == 0xA2) snprintf(out,200,"mov [%s%04X],al",seg_prefix(&d),a16);
        else snprintf(out,200,"mov [%s%04X],ax",seg_prefix(&d),a16);
        (void)r;
        goto done;
    }
    case 0xA4: snprintf(out,200,"movsb"); goto done;
    case 0xA5: snprintf(out,200,"movsw"); goto done;
    case 0xA6: snprintf(out,200,"cmpsb"); goto done;
    case 0xA7: snprintf(out,200,"cmpsw"); goto done;
    case 0xA8: { unsigned v=d8(&d); snprintf(out,200,"test al,%X",v); goto done; }
    case 0xA9: { unsigned v=d16(&d); snprintf(out,200,"test ax,%X",v); goto done; }
    case 0xAA: snprintf(out,200,"stosb"); goto done;
    case 0xAB: snprintf(out,200,"stosw"); goto done;
    case 0xAC: snprintf(out,200,"lodsb"); goto done;
    case 0xAD: snprintf(out,200,"lodsw"); goto done;
    case 0xAE: snprintf(out,200,"scasb"); goto done;
    case 0xAF: snprintf(out,200,"scasw"); goto done;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        unsigned v = d8(&d);
        snprintf(out, 200, "mov %s,%X", RN8[op & 7], v);
        goto done;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        unsigned v = d16(&d);
        snprintf(out, 200, "mov %s,%X", RN16[op & 7], v);
        goto done;
    }
    case 0xC2: { unsigned v=d16(&d); snprintf(out,200,"ret %X",v); goto done; }
    case 0xC3: snprintf(out,200,"ret"); goto done;
    case 0xC4: case 0xC5: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "%s %s,%s", op == 0xC4 ? "les" : "lds",
                 RN16[(m >> 3) & 7], a);
        goto done;
    }
    case 0xC6: case 0xC7: {
        int width = (op == 0xC6) ? 1 : 2;
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        unsigned v = width == 1 ? d8(&d) : d16(&d);
        snprintf(out, 200, "mov %s,%X", a, v);
        goto done;
    }
    case 0xCA: { unsigned v=d16(&d); snprintf(out,200,"retf %X",v); goto done; }
    case 0xCB: snprintf(out,200,"retf"); goto done;
    case 0xCC: snprintf(out,200,"int 3"); goto done;
    case 0xCD: { unsigned v=d8(&d); snprintf(out,200,"int %X",v); goto done; }
    case 0xCE: snprintf(out,200,"into"); goto done;
    case 0xCF: snprintf(out,200,"iret"); goto done;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        static const char *SH[8] = {
            "rol","ror","rcl","rcr","shl","shr","sal","sar"
        };
        int width = (op == 0xD0 || op == 0xD2) ? 1 : 2;
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        if (op == 0xD0 || op == 0xD1)
            snprintf(out, 200, "%s %s,1", SH[(m >> 3) & 7], a);
        else
            snprintf(out, 200, "%s %s,cl", SH[(m >> 3) & 7], a);
        goto done;
    }
    case 0xD4: { unsigned v=d8(&d); snprintf(out,200,"aam %X",v); goto done; }
    case 0xD5: { unsigned v=d8(&d); snprintf(out,200,"aad %X",v); goto done; }
    case 0xD7: snprintf(out,200,"xlat"); goto done;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        /* ESC / coprocessor: the following modrm is part of the encoding */
        uint8_t m = d8(&d);
        rm_str(&d, m >> 6, m & 7, 2, a, sizeof a);
        snprintf(out, 200, "esc %s", a);
        goto done;
    }
    case 0xE0: { int8_t r=(int8_t)d8(&d);
        snprintf(out,200,"loopnz %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xE1: { int8_t r=(int8_t)d8(&d);
        snprintf(out,200,"loopz %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xE2: { int8_t r=(int8_t)d8(&d);
        snprintf(out,200,"loop %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xE3: { int8_t r=(int8_t)d8(&d);
        snprintf(out,200,"jcxz %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xE4: { unsigned v=d8(&d); snprintf(out,200,"in al,%X",v); goto done; }
    case 0xE5: { unsigned v=d8(&d); snprintf(out,200,"in ax,%X",v); goto done; }
    case 0xE6: { unsigned v=d8(&d); snprintf(out,200,"out %X,al",v); goto done; }
    case 0xE7: { unsigned v=d8(&d); snprintf(out,200,"out %X,ax",v); goto done; }
    case 0xE8: { uint16_t r=d16(&d);
        snprintf(out,200,"call %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xE9: { uint16_t r=d16(&d);
        snprintf(out,200,"jmp %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xEA: { uint16_t o=d16(&d), s=d16(&d);
        snprintf(out,200,"jmp %04X:%04X", s, o); goto done; }
    case 0xEB: { int8_t r=(int8_t)d8(&d);
        snprintf(out,200,"jmp %04X",(unsigned)(d.off+d.n+r)&0xFFFF); goto done; }
    case 0xEC: snprintf(out,200,"in al,dx"); goto done;
    case 0xED: snprintf(out,200,"in ax,dx"); goto done;
    case 0xEE: snprintf(out,200,"out dx,al"); goto done;
    case 0xEF: snprintf(out,200,"out dx,ax"); goto done;
    case 0xF4: snprintf(out,200,"hlt"); goto done;
    case 0xF5: snprintf(out,200,"cmc"); goto done;
    case 0xF6: case 0xF7: {
        static const char *G3[8] = { "test","test","not","neg","mul","imul","div","idiv" };
        int width = (op == 0xF6) ? 1 : 2;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        if (reg < 2) {
            unsigned v = width == 1 ? d8(&d) : d16(&d);
            snprintf(out, 200, "%s %s,%X", G3[reg], a, v);
        } else {
            snprintf(out, 200, "%s %s", G3[reg], a);
        }
        goto done;
    }
    case 0xF8: snprintf(out,200,"clc"); goto done;
    case 0xF9: snprintf(out,200,"stc"); goto done;
    case 0xFA: snprintf(out,200,"cli"); goto done;
    case 0xFB: snprintf(out,200,"sti"); goto done;
    case 0xFC: snprintf(out,200,"cld"); goto done;
    case 0xFD: snprintf(out,200,"std"); goto done;
    case 0xFE: {
        int mod, rm;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7;
        rm_str(&d, mod, rm, 1, a, sizeof a);
        snprintf(out, 200, "%s %s", ((m >> 3) & 7) == 1 ? "dec" : "inc", a);
        goto done;
    }
    case 0xFF: {
        static const char *G5[8] = {
            "inc","dec","call","callf","jmp","jmpf","push","db"
        };
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, 2, a, sizeof a);
        snprintf(out, 200, "%s %s", G5[reg], a);
        goto done;
    }
    default:
        break;
    }

    /* register-immediate ALU shortcuts and inc/dec r16 */
    if (op >= 0x40 && op <= 0x47) {
        snprintf(out, 200, "inc %s", RN16[op & 7]);
        goto done;
    }
    if (op >= 0x48 && op <= 0x4F) {
        snprintf(out, 200, "dec %s", RN16[op & 7]);
        goto done;
    }
    if (op >= 0x50 && op <= 0x57) {
        snprintf(out, 200, "push %s", RN16[op & 7]);
        goto done;
    }
    if (op >= 0x58 && op <= 0x5F) {
        snprintf(out, 200, "pop %s", RN16[op & 7]);
        goto done;
    }

    /* Anything not decoded above is data as far as this view is concerned. */
    snprintf(out, 200, "db %02X", op);

done:
    if (len) *len = d.n;
    (void)b;
    return 1;
}

/* Disassemble one instruction at CS:IP. */
int core_disasm_at(char *out, int *len)
{
    return core_disasm_seg(cpu.sr[CS], cpu.ip, out, len);
}

void core_disasm_list(char *out, int outsz, int rows)
{
    int o = 0, i;
    unsigned short cs;
    int ip, len;

    if (outsz <= 0) return;
    out[0] = 0;
    cs = cpu.sr[CS];
    ip = cpu.ip;

    for (i = 0; i < rows; i++) {
        char txt[200];
        len = 0;
        core_disasm_seg(cs, (unsigned short)ip, txt, &len);
        if (len <= 0) break;
        o += snprintf(out + o, (size_t)(outsz - o), "%04X:%04X  %-16s\n",
                      cs, (unsigned)(ip & 0xFFFF), txt);
        if (o >= outsz - 100) break;
        ip = (ip + len) & 0xFFFF;
    }
}

/* Length of the instruction at seg:off, or 0 when nothing decodes. */
int core_insn_len_at(unsigned short seg, unsigned short off)
{
    char txt[200];
    int len = 0;
    core_disasm_seg(seg, off, txt, &len);
    return len;
}

/* Disassemble `rows` instructions from an arbitrary anchor, reporting each
 * line's address and length so the caller can highlight the right row and
 * select exactly the bytes of the current instruction.
 *
 * `addrs`, `lens` and `text` are all caller arrays of at least `rows`
 * entries; `text` is one flat buffer of `rows * tstride` bytes. */
void core_disasm_rows(unsigned short seg, unsigned short first_off, int rows,
                      unsigned short *addrs, int *lens, char *text,
                      int tstride)
{
    int i;
    unsigned off = first_off;

    for (i = 0; i < rows; i++) {
        int len = 0;
        char *slot = text + (size_t)i * tstride;
        slot[0] = 0;
        core_disasm_seg(seg, (unsigned short)off, slot, &len);
        if (len <= 0) { addrs[i] = (unsigned short)off; lens[i] = 0; continue; }
        addrs[i] = (unsigned short)off;
        lens[i] = len;
        off = (off + len) & 0xFFFF;
    }
}

/* ====================================================================== */
/* writing state                                                           */
/*                                                                         */
/* The register pane and the memory pane are editable, as in the original: */
/* typing a value and pressing Enter changes the machine. These go through  */
/* the same poke8/poke16 the CPU uses, so the journal sees them too.       */
/* ====================================================================== */

void core_write_reg(int idx, unsigned short v)
{
    if (idx >= 0 && idx < 8)        cpu.r[idx] = v;
    else if (idx >= 8 && idx < 12)  cpu.sr[idx - 8] = v;
    else if (idx == 12)             cpu.ip = v;
    else if (idx == 13)             cpu.flags = v;
    if (idx == 12) cpu.phys = P(cpu.sr[CS], cpu.ip);
}

void core_write_flags(unsigned short f) { cpu.flags = f; }

/* Absolute write: `phys` is a physical address, not a segment:offset. */
void core_write8(uint32_t phys, unsigned char v) { poke8(phys, v); }
void core_write16(uint32_t phys, unsigned short v) { poke16(phys, v); }

/* ====================================================================== */
/* screen                                                                  */
/* ====================================================================== */

void core_screen_cells(unsigned char *chars, unsigned char *attrs)
{
    int row, col, o = 0;
    int page = dos_screen_page();

    for (row = 0; row < 25; row++) {
        for (col = 0; col < 80; col++) {
            unsigned char a = 0x07;
            unsigned char c = dos_screen_cell(page, row, col, &a);
            if (chars) chars[o] = c;
            if (attrs) attrs[o] = a;
            o++;
        }
    }
}

void core_screen_cursor(int *row, int *col)
{
    int r = 0, c = 0;
    dos_screen_cursor(dos_screen_page(), &r, &c);
    if (row) *row = r;
    if (col) *col = c;
}