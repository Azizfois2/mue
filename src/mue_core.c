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

#include "cpu.h"
#include "dos.h"
#include "asm.h"
#include "mue_core.h"

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

int core_load(const char *src, char *err, int errsz)
{
    char dir[512], path[600];
    FILE *f;
    uint8_t *p = NULL;
    int n = 0;
    const char *tmp = getenv("TEMP");
    if (err && errsz) err[0] = 0;
    if (!tmp || !tmp[0]) tmp = ".";

    snprintf(dir, sizeof dir, "%s\\mue_ui_live", tmp);
#ifdef _WIN32
    { char mk[700]; snprintf(mk, sizeof mk, "cmd /c mkdir \"%s\" 2>nul", dir); system(mk); }
#endif
    snprintf(path, sizeof path, "%s\\guest.asm", dir);

    f = fopen(path, "wb");
    if (!f) { snprintf(err, errsz, "cannot write %s", path); return 0; }
    fputs(src, f);
    fclose(f);

    if (!asm_assemble_file(path, 1, &p, &n, err, errsz ? errsz : 0)) return 0;

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
    return 1;
}

/* ---- stepping --------------------------------------------------------- */

int core_step(void)
{
    if (cpu.halted) return 0;
    return cpu_step();
}

int core_run(int max)
{
    int n = 0;
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

/* Disassemble one instruction at CS:IP without running it. */
int core_disasm_at(char *out, int *len)
{
    dis_t d;
    uint8_t op;
    char a[64], b[64];

    d.seg = cpu.sr[CS];
    d.off = cpu.ip;
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
            if (form == 0) snprintf(out, 200, "%s %s,%s%s", ALU[alu],
                                    seg_prefix(&d), a, rn[reg]);
            else if (form == 1) snprintf(out, 200, "%s %s,%s%s", ALU[alu],
                                         rn[reg], seg_prefix(&d), a);
            else if (form == 2) snprintf(out, 200, "%s %s,%s%s", ALU[alu],
                                         seg_prefix(&d), a, rn[reg]);
            else snprintf(out, 200, "%s %s%s,%s", ALU[alu], seg_prefix(&d),
                          a, rn[reg]);
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
        int width = (op == 0x80 || op == 0x82 || op == 0x83) ? 1 : 2;
        int mod, rm, reg;
        uint8_t m = d8(&d);
        mod = m >> 6; rm = m & 7; reg = (m >> 3) & 7;
        rm_str(&d, mod, rm, width, a, sizeof a);
        unsigned v = (width == 1) ? d8(&d) : d16(&d);
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

void core_disasm_list(char *out, int outsz, int rows)
{
    int o = 0, i;
    uint16_t cs;
    int ip;
    if (outsz <= 0) return;
    out[0] = 0;
    cs = cpu.sr[CS];
    ip = cpu.ip;

    for (i = 0; i < rows; i++) {
        char txt[200];
        int len = 0;
        cpu.sr[CS] = cs;
        cpu.ip = (uint16_t)ip;
        core_disasm_at(txt, &len);
        if (len <= 0) break;
        o += snprintf(out + o, (size_t)(outsz - o), "%04X:%04X  %-16s\n",
                      cs, (unsigned)(ip & 0xFFFF), txt);
        if (o >= outsz - 100) break;
        ip = (ip + len) & 0xFFFF;
    }
}
