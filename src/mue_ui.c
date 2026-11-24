/* mue_ui.c - the emulator's windows, shaped like emu8086 4.08.
 *
 * re/UI_SPEC.md is the authority: every size, colour and band boundary below
 * was measured from a live capture of the program being cloned. The editor
 * and the emulator are different layouts and are built separately.
 *
 * The window client sizes follow the original's *client* rectangles, because
 * emu8086's menu bar is non-client. FLTK draws its menu bar inside the client,
 * so 24px of menu is added above the measured control geometry.
 */
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <commdlg.h>
#endif

#include "mue_widgets.h"
#include "mue_icons.h"
#include "mue_source.h"
#include "mue_core.h"
#include "mue_paths.h"
#include "mue_tools.h"
#include "mue_dialogs.h"
#include "mue_devices.h"
#include "cpu.h"

/* ====================================================================== */
/* machine state                                                           */

typedef struct {
    unsigned short r[8], sr[4], ip, flags;
    long icount;
} RegState;

static RegState cur, prev;
static char guest_src[65536];
static char source_path[512] = "";
static int  loaded;
static int  follow = 1;          /* panes track CS:IP until the user anchors */
static int  running;
static int  step_delay;

static Fl_Double_Window *editor_window, *emu_window;
static SourceView *editor_text, *source_view;
static MueStatus *ed_status;
static Fl_Scrollbar *ed_vscroll, *ed_hscroll;
static Fl_Scrollbar *mem_scroll, *dis_scroll;
static MemList *mem_list;
static DisasmList *dis_list;
static MueField *mem_addr, *dis_addr;

/* registers: four byte pairs (AX BX CX DX), nine word fields */
static MueField *byte_f[4][2];
static MueField *word_f[9];
static Fl_Box *reg_label[13];
static Fl_Button *step_back_btn;
static MueSlider *delay_slider;
static Fl_Box *mem_hdr_keepalive;

/* core register index for the displayed order */
static const int BYTE_REG[4] = { AX, BX, CX, DX };
static const int WORD_REG[9] = { 9, 12, 10, 4, 5, 6, 7, 11, 8 };
/* 9=CS 12=IP 10=SS 4=SP 5=BP 6=SI 7=DI 11=DS 8=ES */

/* ====================================================================== */
/* helpers                                                                 */

static void state_from_core(void)
{
    prev = cur;
    core_regs(cur.r, cur.sr, &cur.ip, &cur.flags, &cur.icount);
}

static unsigned phys_of(unsigned short seg, unsigned short off)
{
    return (((unsigned)seg << 4) + off) & 0xFFFFF;
}

static unsigned short parse_hex(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return (unsigned short)strtoul(s, 0, 16);
}

/* Read the editor into guest_src, without the trailing NUL surprises. */
static void snapshot_source(void)
{
    snprintf(guest_src, sizeof guest_src, "%s", editor_text->text().c_str());
}

/* The name of the image the last build wrote.  Compile lets the user choose it
 * in a Save-As box (emu8086's binary executable files (*.com) dialog), so the
 * answer comes from the core; fall back to the MyBuild convention when
 * nothing has been built yet. */
static const char *build_image_name(void)
{
    static char name[256];
    char stem[128];
    const char *b;
    const char *dot;
    if (core_last_image_name()[0])
        return core_last_image_name();
    b = strrchr(source_path, '\\');
    b = b ? b + 1 : source_path;
    snprintf(stem, sizeof stem, "%s", b);
    dot = strrchr(stem, '.');
    if (dot) stem[dot - stem] = 0;
    if (!stem[0]) snprintf(stem, sizeof stem, "untitled");
    snprintf(name, sizeof name, "%s.%s_", stem, core_image_ext());
    return name;
}

/* ---- source <-> instruction mapping (for the yellow band) -------------
 * The clone's assembler does not retain line numbers, so the band follows
 * the ordinal of the instruction being executed. Labels, comments, blank
 * lines and data/directive lines are skipped; that is exact for straight-line
 * code and a close approximation across branches. */
static int src_exec_lines[4096];
static int src_exec_count;

static int directive_word(const char *w)
{
    static const char *d[] = {
        "org","end","ends","endp","segment","assume","include","name","equ",
        "db","dw","dd","dq","dt","proc","endm","macro","local","align","even",
        "public","extrn","extern","includelib","option",".model",".code",
        ".data",".stack",".386",".8086"
    };
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
        if (strcasecmp(w, d[i]) == 0) return 1;
    return 0;
}

static void build_src_map(void)
{
    const char *p = editor_text->text().c_str();
    int line = 0;
    src_exec_count = 0;
    while (*p && src_exec_count < 4096) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char buf[512];
        if (n > 511) n = 511;
        memcpy(buf, p, (size_t)n);
        buf[n] = 0;
        char *s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (*s && *s != ';' && *s != '#') {
            char *c = strchr(s, ';');
            if (c) *c = 0;
            /* strip a leading label */
            char *colon = strchr(s, ':');
            if (colon && colon != s) {
                int ok = 1;
                for (char *q = s; q < colon; q++)
                    if (!isalnum((unsigned char)*q) && *q != '_' && *q != '.')
                        ok = 0;
                if (ok) s = colon + 1;
            }
            while (*s == ' ' || *s == '\t') s++;
            if (*s && !directive_word(s)) src_exec_lines[src_exec_count++] = line;
        }
        line++;
        if (!e) break;
        p = e + 1;
    }
}

static int src_line_for(long icount)
{
    if (src_exec_count == 0) return -1;
    long i = icount;
    if (i < 0) i = 0;
    if (i >= src_exec_count) i = src_exec_count - 1;
    return src_exec_lines[i];
}

/* ====================================================================== */
/* refreshing the machine views                                            */

static void fmt_hex(char *b, int sz, unsigned v, int digits)
{
    snprintf(b, (size_t)sz, "%0*X", digits, v & (digits == 2 ? 0xFF : 0xFFFF));
}

static void refresh_regs(void)
{
    char b[16];
    for (int r = 0; r < 4; r++) {
        int idx = BYTE_REG[r];
        unsigned v = cur.r[idx];
        for (int h = 0; h < 2; h++) {
            unsigned oldv = prev.r[idx];
            int changed = (v & 0xFF) != (oldv & 0xFF);
            if (h == 0) changed = ((v >> 8) & 0xFF) != ((oldv >> 8) & 0xFF);
            fmt_hex(b, sizeof b, h ? (v & 0xFF) : (v >> 8), 2);
            if (byte_f[r][h] && Fl::focus() != byte_f[r][h]) {
                byte_f[r][h]->value(b);
                byte_f[r][h]->textcolor(changed ? mue_blue() : FL_BLACK);
                byte_f[r][h]->redraw();
            }
        }
    }
    for (int i = 0; i < 9; i++) {
        int idx = WORD_REG[i];
        unsigned v, oldv;
        if (idx == 12) { v = cur.ip; oldv = prev.ip; }
        else if (idx < 8) { v = cur.r[idx]; oldv = prev.r[idx]; }
        else { v = cur.sr[idx - 8]; oldv = prev.sr[idx - 8]; }
        fmt_hex(b, sizeof b, v, 4);
        if (word_f[i] && Fl::focus() != word_f[i]) {
            word_f[i]->value(b);
            word_f[i]->textcolor(v != oldv ? mue_blue() : FL_BLACK);
            word_f[i]->redraw();
        }
    }
}

static void set_addr_field(MueField *f, unsigned short seg, unsigned short off)
{
    char b[16];
    snprintf(b, sizeof b, "%04X:%04X", seg, off);
    if (Fl::focus() != f) f->value(b);
}

static void refresh_panes(void)
{
    unsigned short cs = cur.sr[CS];
    int len = core_insn_len_at(cs, cur.ip);
    if (len <= 0) len = 1;

    if (follow) {
        mem_list->base(phys_of(cs, cur.ip));
        dis_list->anchor(cs, cur.ip);
        set_addr_field(mem_addr, cs, cur.ip);
        set_addr_field(dis_addr, cs, cur.ip);
    }
    mem_list->selection_phys(phys_of(cs, cur.ip), len);
    dis_list->current(cs, cur.ip);
    mem_list->redraw();
    dis_list->redraw();

    if (source_view && src_exec_count) {
        source_view->highlight_line(src_line_for(cur.icount));
    }
    if (step_back_btn) {
        if (core_back_depth() > 0) step_back_btn->activate();
        else                       step_back_btn->deactivate();
        step_back_btn->redraw();
    }
}

static void refresh_all(void)
{
    state_from_core();
    refresh_regs();
    if (loaded) refresh_panes();
    if (ed_status && editor_text) {
        ed_status->caret(editor_text->caret_line() + 1,
                         editor_text->caret_col() + 1);
        ed_status->redraw();
    }
    dlg_refresh();
}

/* ====================================================================== */
/* actions                                                                 */

static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    editor_text->set_text(buf);
    snprintf(source_path, sizeof source_path, "%s", path);
    snprintf(guest_src, sizeof guest_src, "%s", buf);
    build_src_map();
    char title[600];
    snprintf(title, sizeof title, "edit: %s", path);
    editor_window->copy_label(title);
    if (ed_status) ed_status->hint("drag a file here to open");
}

/* ---- start-up templates ------------------------------------------------
 *
 * The four radio buttons of the original's `choose code template` window are
 * exactly the four files in inc\, which is also what file > new reads
 * (re/FILESYSTEM.md section 4).  The literal below is only the fallback for a
 * tree that could not be seeded.
 */
static char *template_read(int which)
{
    static const char *rel[4] = {
        "inc\\0_com_template.txt", "inc\\1_exe_template.txt",
        "inc\\2_bin_template.txt", "inc\\3_boot_template.txt"
    };
    char path[MUE_MAXP];
    return mue_read_file(mue_home_path(path, sizeof path, rel[which & 3]));
}

static const char *template_fallback(void)
{
    return "; You may customize this and other start-up templates;\n"
           "; The location of this template is c:\\emu8086\\inc\\0_com_template.txt\n"
           "\n"
           "org 100h\n"
           "\n"
           "; add your code here\n"
           "\n"
           "ret\n";
}

static void editor_new_from(int which)
{
    char *body = template_read(which);
    editor_text->set_text(body ? body : template_fallback());
    free(body);
    source_path[0] = 0;
    guest_src[0] = 0;
    editor_window->copy_label("edit: untitled.asm");
    build_src_map();
    refresh_all();
}

static void cb_new(Fl_Widget *, void *)
{
    editor_new_from(0);
}

static void cb_open(Fl_Widget *, void *)
{
    static char start[MUE_MAXP];
    const char *p = fl_file_chooser("open source", "*.asm\tassembly sources",
                                    mue_home_path(start, sizeof start, "examples\\"));
    if (p) load_file(p);
    refresh_all();
}

static const char *untitled_path(void)
{
    static char buf[MUE_MAXP];
    return mue_home_path(buf, sizeof buf, "MySource\\untitled.asm");
}

static void cb_save(Fl_Widget *, void *)
{
    const char *p = source_path[0] ? source_path
        : fl_file_chooser("save source", "*.asm\tassembly sources",
                          untitled_path());
    if (!p) return;
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fputs(editor_text->text().c_str(), f);
    fclose(f);
    snprintf(source_path, sizeof source_path, "%s", p);
    char title[600];
    snprintf(title, sizeof title, "edit: %s", p);
    editor_window->copy_label(title);
}

static void cb_save_as(Fl_Widget *, void *)
{
    const char *p = fl_file_chooser("save source as", "*.asm\tassembly sources",
                                    untitled_path());
    if (!p) return;
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fputs(editor_text->text().c_str(), f);
    fclose(f);
    snprintf(source_path, sizeof source_path, "%s", p);
    char title[600];
    snprintf(title, sizeof title, "edit: %s", p);
    editor_window->copy_label(title);
}

static void cb_example(Fl_Widget *w, void *data)
{
    const char *p = (const char *)data;
    if (w && w->user_data()) p = (const char *)w->user_data();
    if (p) { load_file(p); refresh_all(); }
}

static void cb_quit(Fl_Widget *, void *) { exit(0); }

static void cb_step(Fl_Widget *, void *);
static void cb_step_back(Fl_Widget *, void *);

static void cb_emulate(Fl_Widget *, void *)
{
    char err[1024];
    int trace = getenv("MUE_TRACE") != 0;
    clock_t t0 = clock(), t1, t2, t3, t4, t5;
    snapshot_source();
    t1 = clock();
    int ok = core_load(guest_src, source_path, err, sizeof err);
    t2 = clock();
    if (!ok) {
        dlg_assembler_status(err);
        return;
    }
    /* Each load is a fresh machine: reset every virtual device, then start
     * the ones the source names in a comment or string (#start=...). */
    devices_reset();
    devices_scan_source(guest_src);
    loaded = 1;
    follow = 1;
    core_regs(cur.r, cur.sr, &cur.ip, &cur.flags, &cur.icount);
    /* The load just reset the CPU, so the fresh values are the baseline:
     * nothing is "changed" until the first instruction runs. */
    prev = cur;
    build_src_map();
    t3 = clock();
    /* emu8086 names the emulator after the image it just built, e.g.
     * "emulator: hello.com_" (re/FILESYSTEM.md section 2). */
    {
        char title[300];
        snprintf(title, sizeof title, "emulator: %s", build_image_name());
        emu_window->copy_label(title);
    }
    emu_window->show();
    emu_window->take_focus();
    t4 = clock();
    refresh_regs();
    refresh_panes();
    dlg_refresh();
    t5 = clock();
    if (trace)
        fprintf(stderr, "emulate: snapshot %ld load %ld map %ld show %ld panes %ld total %ld ms\n",
                (long)((t1 - t0) * 1000 / CLOCKS_PER_SEC),
                (long)((t2 - t1) * 1000 / CLOCKS_PER_SEC),
                (long)((t3 - t2) * 1000 / CLOCKS_PER_SEC),
                (long)((t4 - t3) * 1000 / CLOCKS_PER_SEC),
                (long)((t5 - t4) * 1000 / CLOCKS_PER_SEC),
                (long)((t5 - t0) * 1000 / CLOCKS_PER_SEC));
}

/* Compile's Save-As box, as emu8086 shows it: the install's MyBuild folder,
 * the source's stem plus the image extension, and the "binary executable
 * files (*.com)" filter.  Returns 0 when the user cancels. */
static int ask_binary_path(char *out, int outsz)
{
#ifdef _WIN32
    OPENFILENAMEA ofn;
    char file[512];
    char stem[128];
    const char *b = strrchr(source_path, '\\');
    const char *dot;
    static char filter[] =
        "binary executable files (*.com)\0*.com\0all files (*.*)\0*.*\0";

    b = b ? b + 1 : source_path;
    snprintf(stem, sizeof stem, "%s", b);
    dot = strrchr(stem, '.');
    if (dot) stem[dot - stem] = 0;
    if (!stem[0]) snprintf(stem, sizeof stem, "untitled");
    snprintf(file, sizeof file, "%s.%s", stem, core_image_ext());

    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = editor_window ? (HWND)fl_xid(editor_window) : NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrInitialDir = mue_dir_build();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameA(&ofn)) return 0;
    snprintf(out, outsz, "%s", ofn.lpstrFile);
    return 1;
#else
    (void)out; (void)outsz;
    return 1;
#endif
}

static void cb_compile(Fl_Widget *, void *)
{
    char err[1024];
    char out[MUE_MAXP];
    char base[600];
    clock_t t0 = clock();
    double secs;

    /* The order is emu8086's, seen live: clicking compile assembles at once
     * and raises the assembler status window WITH its messages, then the
     * Save-As box asks where the .bin goes.  Cancel leaves the messages and
     * appends "executable file is not created!". */
    snapshot_source();
    if (!core_assemble(guest_src, source_path, err, sizeof err)) {
        dlg_assembler_status(err);
        return;
    }
    secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    snprintf(base, sizeof base,
             "%s"
             "Assembled in 3 passes. Time spent: %.3f seconds.\n"
             "%s",
             core_code_after_end(guest_src)
                 ? "any code after 'END' directive is ignored.\n" : "",
             secs,
             core_image_ext()[0] == 'c'
                 ? "entry point is ignored for .com files.\n" : "");
    dlg_assembler_status(base);
    dlg_status_run_enabled(0);
    Fl::flush();

    if (ask_binary_path(out, sizeof out)) {
        char msg[1600];
        core_save_image(out);
        snprintf(msg, sizeof msg,
                 "%s"
                 "\"%s\" is assembled successfully into %d bytes.\n"
                 "\n"
                 "Listing is saved: \"%s.list\"\n"
                 "Symbol table is saved: \"%s.symbol\"",
                 base, build_image_name(), core_last_size(),
                 build_image_name(), build_image_name());
        dlg_assembler_status(msg);
        dlg_status_run_enabled(1);
    } else {
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "%s"
                 "executable file is not created!\n", base);
        dlg_assembler_status(msg);
        dlg_status_run_enabled(0);
    }
}

static void cb_reset(Fl_Widget *, void *)
{
    if (!loaded) return;
    char err[1024];
    core_load(guest_src, source_path, err, sizeof err);
    follow = 1;
    running = 0;
    state_from_core();
    prev = cur;
    refresh_regs();
    refresh_panes();
}

static void stop_run(void)
{
    running = 0;
}

static void cb_stop(Fl_Widget *, void *) { stop_run(); }

static void cb_step(Fl_Widget *, void *)
{
    if (!loaded) { cb_emulate(0, 0); return; }
    int trace = getenv("MUE_TRACE") != 0;
    clock_t t0 = clock();
    if (!core_step()) { refresh_all(); return; }
    clock_t t1 = clock();
    state_from_core();
    refresh_regs();
    refresh_panes();
    if (trace)
        fprintf(stderr, "step: exec %ld refresh %ld total %ld ms\n",
                (long)((t1 - t0) * 1000 / CLOCKS_PER_SEC),
                (long)((clock() - t1) * 1000 / CLOCKS_PER_SEC),
                (long)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
}

static void cb_step_back(Fl_Widget *, void *)
{
    if (!core_step_back()) return;
    state_from_core();
    refresh_regs();
    refresh_panes();
}

static void run_tick(void *)
{
    if (!running) return;
    int n = 0;
    while (n < 400 && !core_halted() && n < 1 + step_delay * 30) {
        if (!core_step()) break;
        n++;
    }
    state_from_core();
    refresh_regs();
    refresh_panes();
    if (running && !core_halted())
        Fl::repeat_timeout(0.02, run_tick);
    else
        running = 0;
}

static void cb_run(Fl_Widget *, void *)
{
    if (!loaded) { cb_emulate(0, 0); return; }
    if (running) { stop_run(); return; }
    running = 1;
    Fl::add_timeout(0.01, run_tick);
}

static void cb_delay(Fl_Widget *w, void *)
{
    /* MueSlider draws its own caption, so the only job here is to record the
     * new delay; redraw() comes from the widget itself. */
    step_delay = ((MueSlider *)w)->value();
}

/* ---- register editing -------------------------------------------------- */

static void cb_reg(Fl_Widget *w, void *)
{
    MueField *f = (MueField *)w;
    int id = (int)(intptr_t)w->user_data();
    unsigned v = parse_hex(f->value());

    if (id < 100) {                     /* byte field: id = row*2 + half */
        int row = id / 2, half = id % 2;
        int idx = BYTE_REG[row];
        unsigned word = cur.r[idx];
        if (half == 0) word = (word & 0x00FF) | ((v & 0xFF) << 8);
        else           word = (word & 0xFF00) | (v & 0xFF);
        core_write_reg(idx, (unsigned short)word);
    } else {
        int i = id - 100;
        core_write_reg(WORD_REG[i], (unsigned short)v);
    }
    state_from_core();
    refresh_regs();
    refresh_panes();
}

/* ---- address fields ---------------------------------------------------- */

static void parse_addr(const char *s, unsigned short *seg, unsigned short *off,
                       int *ok)
{
    const char *c = strchr(s, ':');
    *ok = 0;
    if (!c) return;
    *seg = (unsigned short)strtoul(s, 0, 16);
    *off = (unsigned short)strtoul(c + 1, 0, 16);
    *ok = 1;
}

static void cb_mem_addr(Fl_Widget *w, void *)
{
    unsigned short seg, off;
    int ok;
    parse_addr(((MueField *)w)->value(), &seg, &off, &ok);
    if (!ok) return;
    follow = 0;
    mem_list->base(phys_of(seg, off));
    mem_list->selection_phys(phys_of(cur.sr[CS], cur.ip),
                             core_insn_len_at(cur.sr[CS], cur.ip));
}

static void cb_dis_addr(Fl_Widget *w, void *)
{
    unsigned short seg, off;
    int ok;
    parse_addr(((MueField *)w)->value(), &seg, &off, &ok);
    if (!ok) return;
    follow = 0;
    dis_list->anchor(seg, off);
}

static void mem_picked(unsigned phys, void *)
{
    char b[16];
    snprintf(b, sizeof b, "%05X:", phys);
    (void)b;
    unsigned short seg = (unsigned short)(phys >> 4);
    unsigned short off = (unsigned short)(phys & 0xF);
    set_addr_field(mem_addr, seg, off);
    follow = 0;
}

static void dis_picked(unsigned short seg, unsigned short off, void *)
{
    set_addr_field(dis_addr, seg, off);
    dis_list->selection(seg, off);
}

/* ====================================================================== */
/* editor window                                                           */

static Fl_Menu_Bar *make_editor_menu(int W);

static void cb_gutter_toggle(Fl_Widget *w, void *)
{
    if (!editor_text) return;
    editor_text->gutter(editor_text->gutter() ? 0 : 1);
    editor_text->redraw();
    if (w) w->redraw();
}

static void cb_help_doc(Fl_Widget *, void *)
{
#ifdef _WIN32
    /* ShellExecute hands the file to its registered handler directly. Going
     * through `system("start ...")` spawned a shell, which took a visible
     * fraction of a second on every click. */
    char page[MUE_MAXP];
    ShellExecuteA(NULL, "open",
                  mue_home_path(page, sizeof page, "documentation\\index.html"),
                  NULL, NULL, SW_SHOWNORMAL);
#endif
}

/* ---- folder actions ---------------------------------------------------
 * emu8086's "more examples..." is an Explorer window, not a file dialog, and
 * the virtual-drive commands operate on vdrive\<letter>\ rather than a single
 * flat folder.  Keeping those actions here means the menu and the dialogs
 * agree with the tree in re/FILESYSTEM.md. */
static void open_folder(const char *dir)
{
#ifdef _WIN32
    ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
#else
    (void)dir;
#endif
}

static void cb_more_examples(Fl_Widget *, void *)
{
    open_folder(mue_dir_examples());
}

/* The emulator's own Open is for built images, not for assembly sources:
 * emu8086 starts in MyBuild\ and filters for executable files. */
static void cb_load_exe(Fl_Widget *, void *)
{
    static char start[MUE_MAXP];
    snprintf(start, sizeof start, "%s", mue_dir_build());
    const char *p = fl_file_chooser("load executable",
                                    "*.com\t*.bin\tbinary executable files",
                                    start);
    if (!p) return;
    char err[1024];
    if (!core_load_binary(p, err, sizeof err)) {
        fl_alert("%s", err);
        return;
    }
    devices_reset();
    loaded = 1;
    follow = 1;
    core_regs(cur.r, cur.sr, &cur.ip, &cur.flags, &cur.icount);
    prev = cur;
    {
        char title[300];
        snprintf(title, sizeof title, "emulator: %s", build_image_name());
        emu_window->copy_label(title);
    }
    emu_window->show();
    refresh_regs();
    refresh_panes();
    dlg_refresh();
}

static int dir_exists(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    (void)p;
    return 0;
#endif
}

static void cb_vd_new(Fl_Widget *, void *)
{
    char p[MUE_MAXP];
    int c;
    for (c = 'A'; c <= 'Z'; c++) {
        snprintf(p, sizeof p, "%s\\%c", mue_dir_vdrive(), c);
        if (!dir_exists(p)) {
            mue_mkdirs(p);
            open_folder(p);
            return;
        }
    }
}

static void cb_vd_write_bin(Fl_Widget *, void *)
{
    static char start[MUE_MAXP];
    const char *p = fl_file_chooser("write '.bin' file to floppy",
                                    "*.bin\tbinary files",
                                    mue_home_path(start, sizeof start, "MyBuild\\"));
    char dst[MUE_MAXP], dir[MUE_MAXP];
    const char *name;
    if (!p) return;
    name = strrchr(p, '\\');
    name = name ? name + 1 : p;
    snprintf(dir, sizeof dir, "%s\\A", mue_dir_vdrive());
    mue_mkdirs(dir);
    snprintf(dst, sizeof dst, "%s\\%s", dir, name);
#ifdef _WIN32
    CopyFileA(p, dst, FALSE);
#else
    (void)dst;
#endif
    open_folder(dir);
}

/* The device menu is generated from DEVICES\ the way emu8086 generates it:
 * every .exe in the folder, in directory order. */
static void cb_device(Fl_Widget *, void *data)
{
    if (data) devices_open_name((const char *)data);
}

static Fl_Double_Window *build_editor(void)
{
    const int W = 722, H = 747;
    Fl_Double_Window *win = new Fl_Double_Window(W, H, "edit: untitled.asm");
    win->color(mue_face());
    win->size_range(480, 360);

    Fl_Menu_Bar *mb = make_editor_menu(W);
    (void)mb;

    /* Toolbar. Measured in cap4/win2.png: the strip is 35px tall, the icons
     * are 16px, the labels are Helvetica 9, and the group rules sit at
     * x = 269, 333, 391 and 521. Button edges are derived from the measured
     * icon and label centres. */
    Fl_Group *tb = new Fl_Group(0, 24, W, 35);
    tb->box(FL_FLAT_BOX);
    tb->color(mue_face());
    struct { const char *n; int x, w; Fl_Image *ic; Fl_Callback *cb; int drop; }
    btns[] = {
        { "new",         0, 58, icon_new(),      cb_new,       0 },
        { "open",       58, 58, icon_open(),     cb_open,      0 },
        { "examples",  116, 72, icon_star(),     cb_open,      1 },
        { "save",      188, 70, icon_save(),     cb_save,      1 },
        { "compile",   269, 64, icon_compile(),  cb_compile,   0 },
        { "emulate",   333, 58, icon_run(),      cb_emulate,   0 },
        { "calculator",391, 67, icon_calc(),     cb_open_calc, 0 },
        { "convertor", 458, 63, icon_convert(),  cb_open_conv, 0 },
        { "options",   521, 64, icon_options(),  cb_open_opts, 0 },
        { "help",      585, 55, icon_help(),     cb_help_doc,  0 },
        { "about",     640, 60, icon_about(),    cb_about,     0 },
    };
    for (unsigned i = 0; i < sizeof btns / sizeof btns[0]; i++) {
        MueTool *b = new MueTool(btns[i].x, 24, btns[i].w, 35,
                                 btns[i].n, btns[i].ic, btns[i].drop, 0, 9);
        b->callback(btns[i].cb);
    }
    /* group separators, full height of the strip */
    int seps[] = { 269, 333, 391, 521 };
    for (int i = 0; i < 4; i++) {
        Fl_Box *sp = new Fl_Box(seps[i], 26, 1, 32);
        sp->box(FL_FLAT_BOX);
        sp->color(mue_gutter_line());
    }
    tb->end();

    /* Editor band, measured in the original's client pixels (the clone's
     * 24px menu adds 24 to every y):
     * toolbar 0..35, text 36..681, hscroll 682..696, status 697..718. */
    Fl_Box *rail = new Fl_Box(2, 60, 24, 646);
    rail->box(FL_FLAT_BOX);
    rail->color(mue_rail());

    editor_text = new SourceView(26, 60, 677, 646);
    editor_text->box(FL_DOWN_FRAME);
    editor_text->color(FL_WHITE);
    editor_text->font_size(13);
    editor_text->gutter(1);
    {
        char *body = template_read(0);
        editor_text->set_text(body ? body : template_fallback());
        free(body);
    }

    ed_vscroll = new Fl_Scrollbar(703, 60, 17, 646);
    ed_vscroll->type(FL_VERTICAL);
    ed_hscroll = new Fl_Scrollbar(2, 706, 701, 15);
    ed_hscroll->type(FL_HORIZONTAL);

    ed_status = new MueStatus(0, 721, 722, 22);

    win->end();
    win->callback(cb_quit);
    win->resizable(editor_text);
    return win;
}

/* ====================================================================== */
/* emulator window                                                         */

static Fl_Menu_Bar *make_emu_menu(int W);

static void reg_label_at(int x, int y, const char *t)
{
    Fl_Box *b = new Fl_Box(x, y, 26, 20, t);
    b->box(FL_NO_BOX);
    b->labelfont(FL_HELVETICA);
    b->labelsize(11);
    b->labelcolor(FL_BLACK);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

static void cb_screen_win(Fl_Widget *, void *);
static void cb_source_win(Fl_Widget *, void *);
static void cb_vars_win(Fl_Widget *, void *);
static void cb_debug_win(Fl_Widget *, void *);
static void cb_stack_win(Fl_Widget *, void *);
static void cb_flags_win(Fl_Widget *, void *);

static Fl_Menu_Button *aux_menu;

static void cb_close_emu(Fl_Widget *w, void *);
static void cb_show_emulator(Fl_Widget *, void *);

static void cb_aux(Fl_Widget *w, void *)
{
    /* Fl_Menu_Button::popup() with box()==FL_NO_BOX pops at the current
     * pointer position, which is exactly what the "aux" button wants. */
    aux_menu->popup();
}

static Fl_Double_Window *build_emulator(void)
{
    const int W = 565, H = 408;
    Fl_Double_Window *win = new Fl_Double_Window(W, H, "emulator");
    win->color(mue_face());
    win->size_range(520, 360);

    make_emu_menu(W);

    /* ---- toolbar (measured: 45px strip; five 78px bordered buttons with
     * 5px gaps, at client x 3/86/170/253/336, y 4, h 38; delay control at the
     * right) ---- */
    Fl_Group *tb = new Fl_Group(0, 24, W, 45);
    tb->box(FL_FLAT_BOX);
    tb->color(mue_face());

    MueTool *load = new MueTool(3, 28, 78, 38, "Load", icon_open(), 0, 1);
    load->callback(cb_emulate);
    MueTool *reload = new MueTool(86, 28, 78, 38, "reload", icon_reload(), 0, 1);
    reload->callback(cb_reset);
    step_back_btn = new MueTool(170, 28, 78, 38, "step back",
                                icon_step_back(), 0, 1);
    step_back_btn->callback(cb_step_back);
    MueTool *single = new MueTool(253, 28, 78, 38, "single step",
                                  icon_step(), 0, 1);
    single->callback(cb_step);
    MueTool *run = new MueTool(336, 28, 78, 38, "run", icon_ffwd(), 0, 1);
    run->callback(cb_run);

    delay_slider = new MueSlider(424, 26, 130, 39);
    delay_slider->range(10);
    delay_slider->value(0);
    delay_slider->callback(cb_delay);
    delay_slider->when(FL_WHEN_CHANGED);
    tb->end();

    /* ---- registers group (content 3,45..116,376) ---- */
    MueGroup *rg = new MueGroup(3, 69, 114, 331, "registers");

    Fl_Box *hh = new Fl_Box(50, 84, 24, 12, "H");
    Fl_Box *ll = new Fl_Box(79, 84, 24, 12, "L");
    for (Fl_Box *b : { hh, ll }) {
        b->box(FL_NO_BOX);
        b->labelfont(FL_HELVETICA);
        b->labelsize(11);
        b->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    static const char *byte_names[4] = { "AX", "BX", "CX", "DX" };
    static const int byte_y[4] = { 104, 128, 151, 174 };
    for (int r = 0; r < 4; r++) {
        int y = byte_y[r];
        reg_label_at(23, y, byte_names[r]);
        for (int h = 0; h < 2; h++) {
            byte_f[r][h] = new MueField(50 + h * 29, y, 24, 17, 1);
            byte_f[r][h]->maximum_size(2);
            byte_f[r][h]->value("00");
            byte_f[r][h]->callback(cb_reg);
            byte_f[r][h]->user_data((void *)(intptr_t)(r * 2 + h));
            byte_f[r][h]->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        }
    }

    static const char *word_names[9] = {
        "CS", "IP", "SS", "SP", "BP", "SI", "DI", "DS", "ES"
    };
    for (int i = 0; i < 9; i++) {
        int y = 200 + i * 22;
        reg_label_at(23, y, word_names[i]);
        word_f[i] = new MueField(57, y, 41, 17, 1);
        word_f[i]->maximum_size(4);
        word_f[i]->value("0000");
        word_f[i]->callback(cb_reg);
        word_f[i]->user_data((void *)(intptr_t)(100 + i));
        word_f[i]->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
    }
    rg->end();

    /* ---- memory pane (address field measured 107x22 at content y 54) ---- */
    mem_addr = new MueField(156, 78, 107, 22, 1);
    mem_addr->value("0100:0100");
    mem_addr->callback(cb_mem_addr);
    mem_addr->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);

    mem_list = new MemList(121, 104, 198, 269);
    mem_list->base(0x1100);
    mem_list->pick_fn(mem_picked);

    mem_scroll = new Fl_Scrollbar(319, 104, 17, 269);
    mem_scroll->type(FL_VERTICAL);

    /* ---- disassembly pane ---- */
    dis_addr = new MueField(376, 78, 107, 22, 1);
    dis_addr->value("0100:0100");
    dis_addr->callback(cb_dis_addr);
    dis_addr->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);

    dis_list = new DisasmList(341, 104, 203, 269);
    dis_list->anchor(0x0100, 0x0100);
    dis_list->pick_fn(dis_picked);

    dis_scroll = new Fl_Scrollbar(547, 104, 17, 269);
    dis_scroll->type(FL_VERTICAL);

    /* ---- bottom row (measured: content y 347..382, buttons 121..545) ----
     * Unlike the editor toolbar these are ordinary raised buttons: 35px
     * tall with a 4-5px gap between neighbours.  Drawing them 26px tall and
     * flush made them read as one flat strip. */
    struct { const char *t; int x, w; Fl_Callback *cb; } bb[] = {
        { "screen", 121, 55, cb_screen_win },
        { "source", 180, 47, cb_source_win },
        { "reset",  232, 47, cb_reset },
        { "aux",    283, 47, cb_aux },
        { "vars",   334, 48, cb_vars_win },
        { "debug",  386, 48, cb_debug_win },
        { "stack",  439, 51, cb_stack_win },
        { "flags",  494, 51, cb_flags_win },
    };
    for (int i = 0; i < 8; i++) {
        MueButton *b = new MueButton(bb[i].x, 377, bb[i].w, 30, bb[i].t);
        b->callback(bb[i].cb);
    }

    aux_menu = new Fl_Menu_Button(-100, -100, 10, 10);
    aux_menu->box(FL_NO_BOX);
    aux_menu->add("memory", 0, cb_ram, 0, 0);
    aux_menu->add("ALU - arithmetic & logic unit", 0, cb_alu, 0, 0);
    aux_menu->add("FPU", 0, cb_fpu, 0, 0);
    aux_menu->add("stop on condition", 0, cb_stop_condition, 0, 0);
    aux_menu->add("symbol table", 0, cb_symbol_table, 0, 0);
    aux_menu->add("listing", 0, cb_listing, 0, 0);

    (void)mem_hdr_keepalive;
    win->end();
    win->callback(cb_close_emu);
    win->resizable(mem_list);
    return win;
}

static void cb_close_emu(Fl_Widget *w, void *)
{
    ((Fl_Window *)w)->hide();
}

/* ====================================================================== */
/* menus                                                                   */

static void add_mru(Fl_Menu_Bar *mb, const char *base) { (void)mb; (void)base; }

/* The examples menu is curated: the original names ten programs and maps each
 * onto a file in examples\ (re/FILESYSTEM.md section 5).  Paths are resolved
 * against the install root once, when the menus are built. */
static char ex_buf[16][MUE_MAXP];

static void *ex(const char *name)
{
    static int slot;
    char *p = ex_buf[slot++ % 16];
    snprintf(p, MUE_MAXP, "%s\\examples\\%s", mue_home(), name);
    return p;
}

static Fl_Menu_Bar *make_editor_menu(int W)
{
    Fl_Menu_Bar *mb = new Fl_Menu_Bar(0, 0, W, 24);
    mb->box(FL_FLAT_BOX);
    mb->color(mue_face());
    mb->textfont(FL_HELVETICA);
    mb->textsize(11);
    mb->textcolor(FL_BLACK);
    mb->down_box(FL_FLAT_BOX);

    mb->add("file/new",           0, cb_new, 0, 0);
    mb->add("file/examples/Hello, world", 0, cb_example, ex("hello.asm"), 0);
    mb->add("file/examples/add / subtract", 0, cb_example, ex("1_sample.asm"), 0);
    mb->add("file/examples/calculate sum", 0, cb_example,
            ex("add_two_arrays.asm"), 0);
    mb->add("file/examples/compare numbers", 0, cb_example,
            ex("2_sample.asm"), 0);
    mb->add("file/examples/binary, hex and octal values", 0, cb_example,
            ex("3_sample.asm"), 0);
    mb->add("file/examples/traffic lights", 0, cb_example,
            ex("traffic_lights.asm"), 0);
    mb->add("file/examples/palindrome", 0, cb_example, ex("palindrome.asm"), 0);
    mb->add("file/examples/LED display test", 0, cb_example,
            ex("LED_display_test.asm"), 0);
    mb->add("file/examples/stepper motor", 0, cb_example,
            ex("stepper_motor.asm"), 0);
    mb->add("file/examples/simple i/o", 0, cb_example, ex("simple_io.asm"), 0);
    mb->add("file/examples/more examples...", 0, cb_more_examples, 0,
            FL_MENU_DIVIDER);
    mb->add("file/open...",       0, cb_open, 0, 0);
    mb->add("file/save",          0, cb_save, 0, 0);
    mb->add("file/save as...",    0, cb_save_as, 0, FL_MENU_DIVIDER);
    mb->add("file/print...",      0, 0, 0, 0);
    mb->add("file/export to HTML...", 0, 0, 0, FL_MENU_DIVIDER);
    add_mru(mb, "file");
    mb->add("file/exit",          0, cb_quit, 0, 0);

    mb->add("edit/undo",          0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/redo",          0, 0, 0, FL_MENU_INACTIVE | FL_MENU_DIVIDER);
    mb->add("edit/cut",           0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/copy",          0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/paste",         0, 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/select all",    0, 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/find...",       FL_CTRL + 'f', 0, 0, 0);
    mb->add("edit/find next",     FL_F + 3, 0, 0, 0);
    mb->add("edit/replace...",    FL_CTRL + 'h', 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/go to line...", FL_CTRL + 'g', 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/indent",        FL_Tab, 0, 0, 0);
    mb->add("edit/outdent",       FL_SHIFT + FL_Tab, 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/comment block", FL_CTRL + 'q', 0, 0, 0);
    mb->add("edit/uncomment block", FL_CTRL + 'w', 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/advanced editor macros/Play Macro 1", 0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/advanced editor macros/Play Macro 2", 0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/advanced editor macros/Play Macro 3", 0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/advanced editor macros/Play Macro 4", 0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/advanced editor macros/Play Macro 5", 0, 0, 0, FL_MENU_INACTIVE);
    mb->add("edit/advanced editor macros/Repeat Next Command...",
            FL_CTRL + 'r', 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/advanced editor macros/Record new Keystroke Macro", 0, 0, 0, 0);
    mb->add("edit/advanced/show line numbers", 0, cb_gutter_toggle, 0,
            FL_MENU_TOGGLE | FL_MENU_VALUE | FL_MENU_DIVIDER);
    mb->add("edit/advanced/tabify selection", 0, 0, 0, 0);
    mb->add("edit/advanced/untabify selection", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("edit/advanced/lowercase selection", FL_CTRL + 'l', 0, 0, 0);
    mb->add("edit/advanced/uppercase selection", FL_CTRL + 'u', 0, 0,
            FL_MENU_DIVIDER);
    mb->add("edit/advanced/display whitespace", 0, 0, 0, 0);

    mb->add("bookmarks/toggle bookmark", FL_CTRL + FL_F + 2, 0, 0, 0);
    mb->add("bookmarks/previous bookmark", FL_SHIFT + FL_F + 2, 0, 0,
            FL_MENU_DIVIDER);
    mb->add("bookmarks/next bookmark", FL_F + 2, 0, 0, FL_MENU_DIVIDER);
    mb->add("bookmarks/jump to first", 0, 0, 0, 0);
    mb->add("bookmarks/jump to last", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("bookmarks/clear all bookmarks", 0, 0, 0, 0);

    mb->add("assembler/compile", 0, cb_compile, 0, 0);
    mb->add("assembler/compile and load in the emulator", FL_F + 5,
            cb_emulate, 0, 0);
    mb->add("assembler/fasm", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("assembler/set output directory...", 0, 0, 0, 0);

    mb->add("emulator/show emulator", 0, cb_show_emulator, 0, 0);
    mb->add("emulator/assemble and load in the emulator", 0, cb_emulate, 0, 0);

    mb->add("math/multi base calculator", 0, cb_open_calc, 0, 0);
    mb->add("math/base converter", 0, cb_open_conv, 0, 0);

    mb->add("ascii codes", 0, cb_ascii_codes, 0, 0);

    mb->add("help/documentation and tutorials", FL_F + 1, cb_help_doc, 0, 0);
    mb->add("help/check for an update...", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("help/about...", 0, cb_about, 0, 0);
    return mb;
}

static Fl_Menu_Bar *make_emu_menu(int W)
{
    Fl_Menu_Bar *mb = new Fl_Menu_Bar(0, 0, W, 24);
    mb->box(FL_FLAT_BOX);
    mb->color(mue_face());
    mb->textfont(FL_HELVETICA);
    mb->textsize(11);
    mb->textcolor(FL_BLACK);
    mb->down_box(FL_FLAT_BOX);

    mb->add("file/load executable...", 0, cb_load_exe, 0, 0);
    mb->add("file/reload", FL_F + 4, cb_reset, 0, FL_MENU_DIVIDER);
    mb->add("file/examples/Hello, world", 0, cb_example, ex("hello.asm"), 0);
    mb->add("file/examples/add / subtract", 0, cb_example, ex("1_sample.asm"), 0);
    mb->add("file/examples/more examples...", 0, cb_more_examples, 0,
            FL_MENU_DIVIDER);
    mb->add("file/reset emulator and ram", 0, cb_reset, 0, FL_MENU_DIVIDER);
    mb->add("file/set command line parameters", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("file/save the emulator's state", 0, 0, 0, 0);
    mb->add("file/load from previous state", 0, 0, 0,
            FL_MENU_INACTIVE | FL_MENU_DIVIDER);
    add_mru(mb, "file");
    mb->add("file/close the emulator", 0, cb_close_emu, 0, 0);

    mb->add("math/multi base calculator", 0, cb_open_calc, 0, 0);
    mb->add("math/base converter", 0, cb_open_conv, 0, 0);

    mb->add("debug/single step", FL_F + 8, cb_step, 0, 0);
    mb->add("debug/step over", FL_SHIFT + FL_F + 8, cb_step, 0,
            FL_MENU_DIVIDER);
    mb->add("debug/step back", FL_F + 6, cb_step_back, 0, 0);

    mb->add("view/log and debug.exe emulation", FL_F + 11, cb_debug_win,
            0, FL_MENU_DIVIDER);
    mb->add("view/extended value viewer", 0, cb_extended_value, 0,
            FL_MENU_DIVIDER);
    mb->add("view/stack", 0, cb_stack_win, 0, FL_MENU_DIVIDER);
    mb->add("view/variables", 0, cb_vars_win, 0, 0);
    mb->add("view/memory", 0, cb_ram, 0, FL_MENU_DIVIDER);
    mb->add("view/symbol table", 0, cb_symbol_table, 0, 0);
    mb->add("view/listing", 0, cb_listing, 0, FL_MENU_DIVIDER);
    mb->add("view/original source code", 0, cb_source_win, 0, FL_MENU_DIVIDER);
    mb->add("view/options", 0, cb_open_opts, 0, FL_MENU_DIVIDER);
    mb->add("view/arithmetic & logical unit/memory", 0, cb_ram, 0, 0);
    mb->add("view/arithmetic & logical unit/ALU", 0, cb_alu, 0, 0);
    mb->add("view/arithmetic & logical unit/FPU", 0, cb_fpu, 0, 0);
    mb->add("view/arithmetic & logical unit/stop on condition",
            0, cb_stop_condition, 0, 0);
    mb->add("view/arithmetic & logical unit/symbol table",
            0, cb_symbol_table, 0, 0);
    mb->add("view/arithmetic & logical unit/listing", 0, cb_listing, 0,
            FL_MENU_DIVIDER);
    mb->add("view/flags", 0, cb_flags_win, 0, FL_MENU_DIVIDER);
    mb->add("view/lexical flag analyzer", 0, cb_lexical_flags, 0,
            FL_MENU_DIVIDER);
    mb->add("view/ascii codes", 0, cb_ascii_codes, 0, FL_MENU_DIVIDER);
    mb->add("view/emulator screen", 0, cb_screen_win, 0, 0);

    mb->add("external/start debug.exe (external)", FL_F + 12, 0, 0,
            FL_MENU_DIVIDER);
    mb->add("external/command prompt (external)", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("external/run (external)", 0, 0, 0, 0);

    {
        int i, n = devices_menu_count();
        for (i = 0; i < n; i++) {
            const char *name = devices_menu_name(i);
            char item[300];
            snprintf(item, sizeof item, "virtual devices/%s", name);
            mb->add(item, 0, cb_device, (void *)name, 0);
        }
    }

    mb->add("virtual drive/boot from virtual floppy/boot from floppy image",
            0, 0, 0, 0);
    mb->add("virtual drive/write 512 bytes at 0000:7c00 to boot sector",
            0, 0, 0, FL_MENU_DIVIDER);
    mb->add("virtual drive/write '.bin' file to floppy...", 0, cb_vd_write_bin,
            0, FL_MENU_DIVIDER);
    mb->add("virtual drive/create new floppy drive", 0, cb_vd_new, 0, 0);

    mb->add("help/documentation and tutorials", FL_F + 1, cb_help_doc, 0, 0);
    mb->add("help/check for an update...", 0, 0, 0, FL_MENU_DIVIDER);
    mb->add("help/about...", 0, cb_about, 0, 0);
    return mb;
}

/* ====================================================================== */
/* the emulator's side windows (shapes measured in re/UI_SPEC.md)           */

static Fl_Double_Window *source_win;
static Fl_Double_Window *stack_win, *vars_win, *debug_win, *flags_win;

static void cb_source_win(Fl_Widget *, void *)
{
    if (!source_win) {
        source_win = new Fl_Double_Window(281, 259, "original source code");
        source_win->color(mue_face());
        source_view = new SourceView(3, 3, 275, 253);
        source_view->box(FL_DOWN_FRAME);
        source_view->color(FL_WHITE);
        source_view->font_size(13);
        source_view->gutter(1);
        source_view->read_only(1);
        source_win->end();
        source_win->resizable(source_view);
    }
    source_view->set_text(guest_src);
    if (src_exec_count) source_view->highlight_line(src_line_for(cur.icount));
    source_win->show();
}

static void cb_stack_win(Fl_Widget *, void *)
{
    if (!stack_win) {
        stack_win = new Fl_Double_Window(161, 280, "stack");
        stack_win->color(mue_face());
        StackList *sl = new StackList(2, 2, 141, 276);
        sl->set(cur.sr[SS], cur.r[SP]);
        Fl_Scrollbar *sb = new Fl_Scrollbar(145, 2, 14, 276);
        sb->type(FL_VERTICAL);
        stack_win->end();
        stack_win->resizable(sl);
    }
    stack_win->show();
}

static void cb_vars_win(Fl_Widget *, void *)
{
    if (!vars_win) {
        vars_win = new Fl_Double_Window(239, 213, "variables");
        vars_win->color(mue_face());
        Fl_Box *sz = new Fl_Box(6, 6, 30, 20, "size:");
        sz->box(FL_NO_BOX); sz->labelsize(11); sz->align(FL_ALIGN_RIGHT);
        MueField *szf = new MueField(40, 6, 70, 20, 0);
        szf->value("byte");
        Fl_Box *el = new Fl_Box(118, 6, 56, 20, "elements:");
        el->box(FL_NO_BOX); el->labelsize(11); el->align(FL_ALIGN_RIGHT);
        MueField *elf = new MueField(178, 6, 55, 20, 1);
        elf->value("1");
        MueButton *edit = new MueButton(6, 30, 48, 22, "edit");
        Fl_Box *sa = new Fl_Box(60, 30, 60, 22, "show as:");
        sa->box(FL_NO_BOX); sa->labelsize(11); sa->align(FL_ALIGN_RIGHT);
        MueField *shf = new MueField(122, 30, 70, 22, 0);
        shf->value("hex");
        vars_win->end();
        vars_win->resizable(vars_win);
    }
    vars_win->show();
}

static void cb_debug_win(Fl_Widget *, void *)
{
    if (!debug_win) {
        debug_win = new Fl_Double_Window(677, 221,
                                         "debug log - debug.exe emulation");
        debug_win->color(mue_face());
        debug_win->end();
    }
    debug_win->show();
    dlg_refresh();
}

static void cb_flags_win(Fl_Widget *, void *)
{
    dlg_flags();
}

static void cb_screen_win(Fl_Widget *, void *)
{
    dlg_screen();
}

static void cb_show_emulator(Fl_Widget *, void *)
{
    if (emu_window) { emu_window->show(); emu_window->take_focus(); }
}

/* ====================================================================== */
/* main                                                                    */

static void editor_tick(void *)
{
    if (ed_status && editor_text)
        ed_status->caret(editor_text->caret_line() + 1,
                         editor_text->caret_col() + 1);
    Fl::repeat_timeout(0.15, editor_tick);
}

/* MUE_TRACE=1: log where FLTK thinks a click landed, so DPI scaling can be
 * compared against the physical pixel the operating system delivered. */
static int trace_dispatch(int e, Fl_Window *w)
{
    if (e == FL_PUSH)
        fprintf(stderr, "push at %d,%d window=%s\n", Fl::event_x(),
                Fl::event_y(), w && w->label() ? w->label() : "?");
    return Fl::handle_(e, w);
}

int main(int argc, char **argv)
{
    Fl::set_color(FL_SELECTION_COLOR, 0, 0, 255);
    Fl::set_color(FL_FOREGROUND_COLOR, 0, 0, 0);

    memset(&cur, 0, sizeof cur);
    memset(&prev, 0, sizeof prev);

    /* The install tree has to exist before the first paint: the editor's
     * initial text and the examples menu both read from it. */
    mue_tree_ensure();
#ifdef _WIN32
    _putenv_s("MUE_HOME", mue_home());   /* core code (INT 13h) follows suit */
#else
    setenv("MUE_HOME", mue_home(), 1);
#endif

    devices_boot();                        /* IN/OUT hook + device timer */
    editor_window = build_editor();
    emu_window = build_emulator();
    Fl_Window::default_icon(icon_app());   /* every window: title bar, taskbar */
    dlg_status_on_run([]() { cb_emulate(0, 0); });
    if (getenv("MUE_TRACE"))
        fprintf(stderr, "home: %s\n", mue_home());
    if (getenv("MUE_TRACE"))
        fprintf(stderr, "dpi: screen_scale=%g editor %dx%d emu %dx%d\n",
                (double)Fl::screen_scale(0),
                editor_window->w(), editor_window->h(),
                emu_window->w(), emu_window->h());

    if (argc > 1) {
        load_file(argv[1]);
    } else if (!getenv("MUE_BLANK")) {
        char start[MUE_MAXP];
        load_file(mue_home_path(start, sizeof start, "examples\\hello.asm"));
    }

    editor_window->show(argc, argv);
    if (getenv("MUE_TRACE")) {
        int sn = editor_window->screen_num();
        HWND hw = (HWND)editor_window->os_id();
        RECT r = {0, 0, 0, 0};
        if (hw) GetClientRect(hw, &r);
        fprintf(stderr,
                "screens=%d screen_num=%d scale(sn)=%g scale(0)=%g win32 client=%ldx%ld\n",
                Fl::screen_count(), sn,
                (double)Fl::screen_scale(sn),
                (double)Fl::screen_scale(0),
                (long)r.right, (long)r.bottom);
        Fl::event_dispatch(trace_dispatch);
    }
    if (getenv("MUE_GEOM")) {
        FILE *gf = fopen(getenv("MUE_GEOM"), "w");
        if (gf) {
            fprintf(gf, "editor win %d,%d %dx%d\n", editor_window->x(),
                    editor_window->y(), editor_window->w(), editor_window->h());
            fprintf(gf, "text %d,%d %dx%d\n", editor_text->x(), editor_text->y(),
                    editor_text->w(), editor_text->h());
            fprintf(gf, "hscroll %d,%d %dx%d\n", ed_hscroll->x(),
                    ed_hscroll->y(), ed_hscroll->w(), ed_hscroll->h());
            fprintf(gf, "status %d,%d %dx%d\n", ed_status->x(), ed_status->y(),
                    ed_status->w(), ed_status->h());
            fprintf(gf, "emu %d,%d %dx%d\n", emu_window->x(), emu_window->y(),
                    emu_window->w(), emu_window->h());
            fclose(gf);
        }
    }
    Fl::add_timeout(0.15, editor_tick);

    {
        const char *tk = getenv("MUE_OPEN_TOOLS");
        if (tk) {
            if (strstr(tk, "emulate"))    cb_emulate(0, 0);
            if (strstr(tk, "options"))    tools_open_options();
            if (strstr(tk, "convertor"))  tools_open_convertor();
            if (strstr(tk, "calculator")) tools_open_calculator();
            if (strstr(tk, "stack"))      cb_stack_win(0, 0);
            if (strstr(tk, "source"))     cb_source_win(0, 0);
            if (strstr(tk, "vars"))       cb_vars_win(0, 0);
            if (strstr(tk, "debug"))      cb_debug_win(0, 0);
            if (strstr(tk, "flags"))      cb_flags_win(0, 0);
            if (strstr(tk, "screen"))     cb_screen_win(0, 0);
            if (strstr(tk, "ascii"))      cb_ascii_codes(0, 0);
            if (strstr(tk, "about"))      cb_about(0, 0);
            if (strstr(tk, "alu"))        cb_alu(0, 0);
            if (strstr(tk, "ram"))        cb_ram(0, 0);
            if (strstr(tk, "status"))     dlg_assembler_status("Assembled successfully.");
            if (strstr(tk, "led"))        devices_open(DEV_LED);
            if (strstr(tk, "traffic"))    devices_open(DEV_TRAFFIC);
            if (strstr(tk, "stepper"))    devices_open(DEV_STEPPER);
            if (strstr(tk, "thermo"))     devices_open(DEV_THERMO);
            if (strstr(tk, "robot"))      devices_open(DEV_ROBOT);
            if (strstr(tk, "printer"))    devices_open(DEV_PRINTER);
            if (strstr(tk, "simple"))     devices_open(DEV_SIMPLE);
            if (strstr(tk, "vga"))        devices_open(DEV_VGA);
        }
    }
    Fl::flush();
    return Fl::run();
}
