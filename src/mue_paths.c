/* mue_paths.c - see mue_paths.h.
 *
 * The tree is described in re/FILESYSTEM.md.  On a machine where the reference
 * emu8086 install is present (the usual case for someone who wants a clone),
 * mue copies its inc\, examples\, Options\ and documentation\ folders verbatim
 * the first time it runs, so the clone reads byte-identical templates and
 * examples.  Where the reference is absent, the same files are seeded from the
 * literals at the bottom of this file.
 */
#include "mue_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  define mue_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  ifndef MAX_PATH
#    define MAX_PATH 260
#  endif
#  define mue_mkdir(p) mkdir(p, 0777)
#  define GetModuleFileNameA(h, b, n) (snprintf(b, n, "/proc/self/exe"), (int)strlen(b))
#endif


static char home[MUE_MAXP];
static int  home_ready;

/* ---- small filesystem helpers ----------------------------------------- */

static int is_dir(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
#endif
}

static int is_file(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFREG) != 0;
#endif
}

int mue_mkdirs(const char *path)
{
    char tmp[MUE_MAXP];
    size_t i, n;
    if (!path || !*path) return 0;
    snprintf(tmp, sizeof tmp, "%s", path);
    n = strlen(tmp);
    while (n > 1 && (tmp[n - 1] == '\\' || tmp[n - 1] == '/')) tmp[--n] = 0;
    for (i = 1; i < n; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char c = tmp[i];
            tmp[i] = 0;
            if (tmp[i - 1] != ':') mue_mkdir(tmp);
            tmp[i] = c;
        }
    }
    mue_mkdir(tmp);
    return is_dir(tmp);
}

int mue_write_file(const char *path, const void *data, int len)
{
    char dir[MUE_MAXP];
    const char *s;
    FILE *f;
    if (!path || !*path) return 0;
    snprintf(dir, sizeof dir, "%s", path);
    s = strrchr(dir, '\\');
    if (!s) s = strrchr(dir, '/');
    if (s) { dir[s - dir] = 0; if (dir[0]) mue_mkdirs(dir); }
    f = fopen(path, "wb");
    if (!f) return 0;
    if (len > 0 && data) fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return 1;
}

char *mue_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char chunk[8192];
    size_t n;
    if (!in) return 0;
    out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) fwrite(chunk, 1, n, out);
    fclose(out);
    fclose(in);
    return 1;
}

/* Recursively copy `from` into `to`, skipping files that already exist so a
 * user's edited template or example is never clobbered. */
static void copy_tree(const char *from, const char *to, int depth)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pat[MUE_MAXP], src[MUE_MAXP], dst[MUE_MAXP];
    HANDLE h;
    if (depth > 6 || !is_dir(from)) return;
    mue_mkdirs(to);
    snprintf(pat, sizeof pat, "%s\\*", from);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(src, sizeof src, "%s\\%s", from, fd.cFileName);
        snprintf(dst, sizeof dst, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            copy_tree(src, dst, depth + 1);
        } else if (!is_file(dst)) {
            copy_file(src, dst);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)from; (void)to; (void)depth;
#endif
}

/* ---- the install root -------------------------------------------------- */

static void exe_dir(char *buf, int n)
{
    char *s;
    buf[0] = 0;
#ifdef _WIN32
    GetModuleFileNameA(NULL, buf, (DWORD)n);
#else
    GetModuleFileNameA(0, buf, n);
#endif
    s = strrchr(buf, '\\');
    if (!s) s = strrchr(buf, '/');
    if (s) *s = 0;
}

static void normalize(char *p, int n)
{
#ifdef _WIN32
    char tmp[MUE_MAXP];
    DWORD k = GetFullPathNameA(p, (DWORD)sizeof tmp, tmp, NULL);
    if (k && k < sizeof tmp) snprintf(p, (size_t)n, "%s", tmp);
#else
    (void)p; (void)n;
#endif
}

static void resolve_home(void)
{
    const char *env;
    char cand[MUE_MAXP], probe[MUE_MAXP];

    if (home_ready) return;
    home_ready = 1;

    env = getenv("MUE_HOME");
    if (env && env[0]) {
        snprintf(home, sizeof home, "%s", env);
        normalize(home, sizeof home);
        if (!is_dir(home)) mue_mkdirs(home);
        if (is_dir(home)) return;
    }

    /* A self-contained package: muei.exe and the whole emu8086-shaped tree
     * side by side in one build folder.  Checked first so a built copy never
     * falls through to the developer's C:\mue install. */
    exe_dir(cand, sizeof cand);
    snprintf(probe, sizeof probe, "%s\\inc", cand);
    if (is_dir(probe)) {
        snprintf(home, sizeof home, "%s", cand);
        normalize(home, sizeof home);
        return;
    }

    /* The development layout: the UI binary in src\ and the tree one level
     * up, mirroring emu8086's own <root>\ + <root>\bin\ arrangement. */
    snprintf(probe, sizeof probe, "%s\\..\\inc", cand);
    if (is_dir(probe)) {
        snprintf(home, sizeof home, "%s\\..", cand);
        normalize(home, sizeof home);
        return;
    }

    snprintf(home, sizeof home, "C:\\mue");
    if (!is_dir(home)) mue_mkdirs(home);
    if (is_dir(home)) return;

    snprintf(home, sizeof home, "C:\\emu8086");
}

const char *mue_home(void)
{
    resolve_home();
    return home;
}

const char *mue_home_path(char *buf, int bufsz, const char *rel)
{
    if (!rel || !*rel) snprintf(buf, (size_t)bufsz, "%s", mue_home());
    else if (rel[0] == '\\' || rel[0] == '/')
        snprintf(buf, (size_t)bufsz, "%s%s", mue_home(), rel);
    else
        snprintf(buf, (size_t)bufsz, "%s\\%s", mue_home(), rel);
    return buf;
}

static char d_examples[MUE_MAXP], d_inc[MUE_MAXP], d_build[MUE_MAXP];
static char d_source[MUE_MAXP], d_options[MUE_MAXP], d_vdrive[MUE_MAXP];
static char d_docs[MUE_MAXP];

const char *mue_dir_examples(void) { return mue_home_path(d_examples, MUE_MAXP, "examples"); }
const char *mue_dir_inc(void)      { return mue_home_path(d_inc, MUE_MAXP, "inc"); }
const char *mue_dir_build(void)    { return mue_home_path(d_build, MUE_MAXP, "MyBuild"); }
const char *mue_dir_source(void)   { return mue_home_path(d_source, MUE_MAXP, "MySource"); }
const char *mue_dir_options(void)  { return mue_home_path(d_options, MUE_MAXP, "Options"); }
const char *mue_dir_vdrive(void)   { return mue_home_path(d_vdrive, MUE_MAXP, "vdrive"); }
const char *mue_dir_docs(void)     { return mue_home_path(d_docs, MUE_MAXP, "documentation"); }

/* ---- seeding ----------------------------------------------------------- */

/* Where to import the stock tree from.  MUE_REF wins, then the reference
 * install the clone was reverse-engineered from. */
static const char *ref_root(void)
{
    static const char *cached;
    static int tried;
    const char *e;
    if (tried) return cached;
    tried = 1;
    e = getenv("MUE_REF");
    if (e && e[0] && is_dir(e)) { cached = e; return cached; }
    if (is_dir("C:\\emu8086") && strcmp(mue_home(), "C:\\emu8086"))
        cached = "C:\\emu8086";
    return cached;
}

static void seed_text(const char *rel, const char *text)
{
    char path[MUE_MAXP];
    if (is_file(mue_home_path(path, sizeof path, rel))) return;
    mue_write_file(path, text, (int)strlen(text));
}

static void seed_ini(void)
{
    char path[MUE_MAXP], text[1024];
    if (is_file(mue_home_path(path, sizeof path, "mue.ini"))) return;
    snprintf(text, sizeof text,
        "; mue configuration.  The keys mirror emu8086.ini (re/FILESYSTEM.md\n"
        "; section 1); the parser is the clone's own, but the names are kept so\n"
        "; a migrated install behaves the same.\n"
        "\n"
        "STARTUPWIN=false\n"
        "LISTING=true\n"
        "WARN_RUN=true\n"
        "ASK_OPEN_URL=true\n"
        "VDRIVE_PATH=%s\\vdrive\n"
        "HOME=%s\n"
        "\n"
        "[fonts]\n"
        "editor=Terminal\n"
        "editor_size=13\n",
        mue_home(), mue_home());
    mue_write_file(path, text, (int)strlen(text));
}

/* Options\Keywords.dat: eleven 12-byte records of (foreground, background, 0)
 * in COLORREF byte order.  The values are the ones decoded in FILESYSTEM.md. */
static void seed_keywords(void)
{
    static const unsigned char rec[11][3] = {
        { 0x00, 0x80, 0x00 },   /* 0  green       #008000 */
        { 0x00, 0x00, 0xFF },   /* 1  red         #FF0000 */
        { 0xA0, 0xA0, 0xA0 },   /* 2  grey        #A0A0A0 */
        { 0x00, 0x00, 0x00 },   /* 3  black       #000000 */
        { 0x00, 0x64, 0xC8 },   /* 4  orange      #C86400 */
        { 0x00, 0x00, 0x00 },   /* 5  unused */
        { 0x80, 0x00, 0x80 },   /* 6  purple      #800080 */
        { 0xC8, 0x00, 0x00 },   /* 7  blue        #0000C8 */
        { 0x80, 0x00, 0x00 },   /* 8  navy        #000080 */
        { 0x00, 0x00, 0x64 },   /* 9  dark red    #640000 */
        { 0x00, 0x00, 0x00 }    /* 10 unused */
    };
    unsigned char buf[11 * 12];
    char path[MUE_MAXP];
    int i;
    if (is_file(mue_home_path(path, sizeof path, "Options\\Keywords.dat"))) return;
    memset(buf, 0xFF, sizeof buf);
    for (i = 0; i < 11; i++) {
        buf[i * 12 + 0] = rec[i][0];
        buf[i * 12 + 1] = rec[i][1];
        buf[i * 12 + 2] = rec[i][2];
    }
    mue_write_file(path, buf, (int)sizeof buf);
}

static void seed_other(void)
{
    char path[MUE_MAXP];
    unsigned char buf[45];
    if (is_file(mue_home_path(path, sizeof path, "Options\\Other.dat"))) return;
    memset(buf, 0, sizeof buf);
    buf[0] = 1;   /* auto-indent on            */
    buf[1] = 0;   /* spaces, not tabs          */
    buf[2] = 4;   /* tab size                  */
    buf[3] = 1;   /* line numbers on           */
    buf[4] = 1;   /* numbers start at 1        */
    mue_write_file(path, buf, (int)sizeof buf);
}

static void seed_templates(void)
{
    char path[MUE_MAXP], head[512];

    snprintf(head, sizeof head,
             "; You may customize this and other start-up templates;\n"
             "; The location of this template is %s\\inc\\0_com_template.txt\n"
             "\n"
             "org 100h\n"
             "\n"
             "; add your code here\n"
             "\n"
             "ret\n", mue_home());
    seed_text("inc\\0_com_template.txt", head);

    snprintf(head, sizeof head,
             "; You may customize this and other start-up templates;\n"
             "; The location of this template is %s\\inc\\1_exe_template.txt\n"
             "\n"
             "data segment\n"
             "    pkey db \"press any key...$\"\n"
             "ends\n"
             "\n"
             "stack segment\n"
             "    dw   128  dup(0)\n"
             "ends\n"
             "\n"
             "code segment\n"
             "start:\n"
             "; set segment registers:\n"
             "    mov ax, data\n"
             "    mov ds, ax\n"
             "    mov es, ax\n"
             "\n"
             "; add your code here\n"
             "\n"
             "    lea dx, pkey\n"
             "    mov ah, 9\n"
             "    int 21h        ; output string at ds:dx\n"
             "\n"
             "; wait for any key...\n"
             "    mov ah, 1\n"
             "    int 21h\n"
             "\n"
             "    mov ax, 4c00h ; exit to operating system.\n"
             "    int 21h\n"
             "ends\n"
             "\n"
             "end start\n", mue_home());
    seed_text("inc\\1_exe_template.txt", head);

    snprintf(head, sizeof head,
             "; You may customize this and other start-up templates;\n"
             "; The location of this template is %s\\inc\\2_bin_template.txt\n"
             "\n"
             "#make_bin#\n"
             "\n"
             "; add your code here\n"
             "\n"
             "hlt\n", mue_home());
    seed_text("inc\\2_bin_template.txt", head);

    snprintf(head, sizeof head,
             "; You may customize this and other start-up templates;\n"
             "; The location of this template is %s\\inc\\3_boot_template.txt\n"
             "\n"
             "#make_boot#\n"
             "\n"
             "org 7c00h\n"
             "\n"
             "; add your code here\n"
             "\n"
             "jmp $\n"
             "\n"
             "org 7dfeh\n"
             "db 55h, 0aah\n", mue_home());
    seed_text("inc\\3_boot_template.txt", head);

    /* The #fasm# variants are the same skeletons behind a directive. */
    {
        static const char *pair[4][2] = {
            { "inc\\0_com_template.txt",  "inc\\fasm_0_com_template.txt"  },
            { "inc\\1_exe_template.txt",  "inc\\fasm_1_exe_template.txt"  },
            { "inc\\2_bin_template.txt",  "inc\\fasm_2_bin_template.txt"  },
            { "inc\\3_boot_template.txt", "inc\\fasm_3_boot_template.txt" }
        };
        int i;
        for (i = 0; i < 4; i++) {
            char src[MUE_MAXP], dst[MUE_MAXP];
            char *body;
            mue_home_path(src, sizeof src, pair[i][0]);
            mue_home_path(dst, sizeof dst, pair[i][1]);
            if (is_file(dst)) continue;
            body = mue_read_file(src);
            if (!body) continue;
            {
                size_t n = strlen(body);
                char *out = (char *)malloc(n + 16);
                if (out) {
                    snprintf(out, n + 16, "#fasm#\n%s", body);
                    mue_write_file(dst, out, (int)strlen(out));
                    free(out);
                }
            }
            free(body);
        }
    }
    (void)path;
}

static void seed_docs(void)
{
    static const char page[] =
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
        "<title>mue - help</title>\n"
        "<style>body{font:13px Tahoma,sans-serif;margin:24px;max-width:46em}"
        "h1{font-size:18px}h2{font-size:14px;margin-top:1.4em}"
        "code{background:#f0f0f0;padding:1px 3px}</style></head><body>\n"
        "<h1>mue</h1>\n"
        "<p>An 8086 assembler and emulator.  This folder is the install tree;\n"
        "the same shape emu8086 uses, recorded in <code>re/FILESYSTEM.md</code>.</p>\n"
        "<h2>folders</h2>\n"
        "<ul>\n"
        "<li><code>MyBuild\\</code> - every build: <code>&lt;name&gt;.com_</code>\n"
        "plus <code>.debug</code>, <code>.list</code>, <code>.symbol</code>\n"
        "and <code>.~asm</code>.</li>\n"
        "<li><code>MySource\\</code> - where Save As points by default.</li>\n"
        "<li><code>examples\\</code> - the examples menu and the start-up file.</li>\n"
        "<li><code>inc\\</code> - the four start-up templates, also used by\n"
        "<code>file &gt; new</code>.</li>\n"
        "<li><code>Options\\</code> - editor colours, fonts, hotkeys, flags.</li>\n"
        "<li><code>vdrive\\A .. F</code> - the virtual drive letters.</li>\n"
        "<li><code>DEVICES\\</code> - external virtual devices.</li>\n"
        "</ul>\n"
        "<h2>keys</h2>\n"
        "<ul><li><code>F5</code> emulate, <code>F7</code> single step,\n"
        "<code>F8</code> step back, <code>Ctrl+F9</code> compile.</li></ul>\n"
        "</body></html>\n";
    seed_text("documentation\\index.html", page);
    seed_text("documentation\\img\\.keep", "");
}

static void seed_readme(void)
{
    seed_text("ReadMe.txt",
        "mue - 8086 assembler and emulator\r\n"
        "\r\n"
        "The install tree mirrors emu8086 4.08 so that the editor, the examples\r\n"
        "menu and the build outputs behave the way the original does:\r\n"
        "  inc\\        start-up templates (file > new)\r\n"
        "  examples\\   the examples menu and the file opened at start-up\r\n"
        "  MyBuild\\    <name>.com_ and its four sidecars\r\n"
        "  MySource\\   default Save As folder\r\n"
        "  Options\\    editor colours, fonts, hotkeys\r\n"
        "  vdrive\\     virtual drive letters A..F and tmp\r\n"
        "  DEVICES\\    external virtual devices\r\n"
        "\r\n"
        "mue is MIT licensed; see LICENSE.txt.\r\n");

    seed_text("LICENSE.txt",
        "MIT License\r\n"
        "\r\n"
        "Copyright (c) 2026 AZDev\r\n"
        "\r\n"
        "Permission is hereby granted, free of charge, to any person obtaining a copy\r\n"
        "of this software and associated documentation files (the \"Software\"), to deal\r\n"
        "in the Software without restriction, including without limitation the rights\r\n"
        "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n"
        "copies of the Software, and to permit persons to whom the Software is\r\n"
        "furnished to do so, subject to the following conditions:\r\n"
        "\r\n"
        "The above copyright notice and this permission notice shall be included in all\r\n"
        "copies or substantial portions of the Software.\r\n"
        "\r\n"
        "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n"
        "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\n"
        "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\n"
        "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\n"
        "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\n"
        "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\n"
        "SOFTWARE.\r\n");
}

void mue_tree_ensure(void)
{
    static int done;
    const char *ref;
    char p[MUE_MAXP];
    int i;

    if (done) return;
    done = 1;

    mue_mkdirs(mue_home());
    mue_mkdirs(mue_dir_examples());
    mue_mkdirs(mue_dir_inc());
    mue_mkdirs(mue_dir_build());
    mue_mkdirs(mue_dir_source());
    mue_mkdirs(mue_dir_options());
    mue_mkdirs(mue_dir_docs());
    mue_mkdirs(mue_home_path(p, sizeof p, "documentation\\img"));
    mue_mkdirs(mue_home_path(p, sizeof p, "DEVICES"));
    mue_mkdirs(mue_home_path(p, sizeof p, "fasm\\INCLUDE"));
    mue_mkdirs(mue_dir_vdrive());
    for (i = 0; i < 6; i++) {
        snprintf(p, sizeof p, "%s\\%c", mue_dir_vdrive(), 'A' + i);
        mue_mkdirs(p);
    }
    snprintf(p, sizeof p, "%s\\tmp", mue_dir_vdrive());
    mue_mkdirs(p);

    /* DEVICES\ is a shipped folder, not a generated one: copy the reference
     * device set (and its docs) the first time, even on an install whose
     * mue.ini already exists.  The menu enumerates this folder, so the clone
     * lists exactly the files the original lists. */
    {
        char devdir[MUE_MAXP], probe[MUE_MAXP];
        mue_home_path(devdir, sizeof devdir, "DEVICES");
        mue_home_path(probe, sizeof probe, "DEVICES\\_READ_ME.txt");
        if (!is_file(probe)) {
            const char *rr = ref_root();
            if (rr) {
                char from[MUE_MAXP];
                snprintf(from, sizeof from, "%s\\DEVICES", rr);
                copy_tree(from, devdir, 0);
            }
        }
    }

    /* Import the stock tree once.  mue.ini is the marker: it is written by the
     * seeding below, so a second run skips straight past the copies. */
    if (is_file(mue_home_path(p, sizeof p, "mue.ini"))) return;

    ref = ref_root();
    if (ref) {
        char from[MUE_MAXP], to[MUE_MAXP];
        static const char *folders[] = { "inc", "examples", "Options",
                                         "documentation" };
        for (i = 0; i < 4; i++) {
            snprintf(from, sizeof from, "%s\\%s", ref, folders[i]);
            mue_home_path(to, sizeof to, folders[i]);
            copy_tree(from, to, 0);
        }
    }

    seed_templates();
    seed_keywords();
    seed_other();
    seed_docs();
    seed_readme();
    seed_ini();
}
