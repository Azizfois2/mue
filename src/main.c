/* mue — clean-room 8086 assembler + emulator
 * usage:  mue <file.asm>          assemble and run as .COM
 *         mue -a <file.asm> <out>  assemble only
 *         mue -r <file.com>        run a flat binary at 100h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "alu.h"
#include "io.h"
#include "asm.h"
#include "exe.h"
#include "dos.h"

static void dump_state(void)
{
    printf("\n------------- REGISTERS -------------\n");
    printf("AX=%04X  BX=%04X  CX=%04X  DX=%04X\n",
           cpu.r[AX], cpu.r[BX], cpu.r[CX], cpu.r[DX]);
    printf("SP=%04X  BP=%04X  SI=%04X  DI=%04X\n",
           cpu.r[SP], cpu.r[BP], cpu.r[SI], cpu.r[DI]);
    printf("CS=%04X  DS=%04X  ES=%04X  SS=%04X  IP=%04X\n",
           cpu.sr[CS], cpu.sr[DS], cpu.sr[ES], cpu.sr[SS], cpu.ip);
    printf("FLAGS=%04X %s%s%s%s%s%s%s%s%s\n", cpu.flags,
           (cpu.flags & F_CF) ? "CF " : "", (cpu.flags & F_PF) ? "PF " : "",
           (cpu.flags & F_AF) ? "AF " : "", (cpu.flags & F_ZF) ? "ZF " : "",
           (cpu.flags & F_SF) ? "SF " : "", (cpu.flags & F_TF) ? "TF " : "",
           (cpu.flags & F_IF) ? "IF " : "", (cpu.flags & F_DF) ? "DF " : "",
           (cpu.flags & F_OF) ? "OF " : "");
    printf("instructions executed: %ld\n", cpu.icount);
}

static void load_com(const uint8_t *p, int n)
{
    memset(MEM, 0, sizeof(MEM));
    if (n > 0xFF00) n = 0xFF00;
    /* .COM loads at CS:0100 with all segment registers equal */
    memcpy(MEM + 0x01100, p, (size_t)n);
    cpu_reset();
    cpu.sr[CS] = cpu.sr[DS] = cpu.sr[ES] = cpu.sr[SS] = 0x0100;
    cpu.r[SP]  = 0xFFFE;
    poke16(seg_off_to_phys(cpu.sr[SS], 0xFFFE), 0x0000);
    cpu.ip     = 0x0100;
    cpu.phys   = seg_off_to_phys(cpu.sr[CS], cpu.ip);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "mue - clean-room 8086 assembler + emulator\n"
            "  mue <file.asm>           assemble and run (.COM)\n"
            "  mue -a <file.asm> <out>  assemble to a flat binary\n"
            "  mue -r <file.bin>        run a flat binary loaded at 100h\n");
        return 1;
    }

    if (!strcmp(argv[1], "-r") && argc >= 3) {
        FILE *f = fopen(argv[2], "rb");
        uint8_t *b; long n;
        if (!f) { perror(argv[2]); return 1; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = (uint8_t *)malloc((size_t)n);
        fread(b, 1, (size_t)n, f);
        fclose(f);
        load_com(b, (int)n);
        free(b);
        cpu_run(2000000);
        dos_flush(stdout);
        dump_state();
        return cpu.exit_code;
    }

    if (!strcmp(argv[1], "-a") && argc >= 4) {
        uint8_t *out = NULL; int outlen = 0;
        uint8_t *img = NULL; int imglen = 0;
        char err[512] = {0};
        if (!asm_assemble_file(argv[2], 1, &out, &outlen, err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", argv[2], err);
            return 1;
        }
        /* A `.model` source targets an MZ EXE: wrap the code in a header the
         * way emu8086 does (re/FINDINGS.md sec. 11). Flat sources stay flat. */
        if (asm_last_was_exe()) {
            img = exe_wrap_ex(out, outlen, asm_last_stack_size(), asm_last_code_start(), &imglen);
            if (!img) { free(out); fprintf(stderr, "out of memory\n"); return 1; }
            free(out);
            out = img; outlen = imglen;
        }
        {
            FILE *f = fopen(argv[3], "wb");
            if (!f) { perror(argv[3]); free(out); return 1; }
            fwrite(out, 1, (size_t)outlen, f);
            fclose(f);
        }
        printf("assembled %d bytes -> %s\n", outlen, argv[3]);
        free(out);
        return 0;
    }

    /* default: assemble and run */
    {
        uint8_t *out = NULL; int outlen = 0;
        char err[512] = {0};
        if (!asm_assemble_file(argv[1], 1, &out, &outlen, err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", argv[1], err);
            return 1;
        }
        printf("assembled %d bytes\n", outlen);
        load_com(out, outlen);
        free(out);
        cpu_run(2000000);
        dos_flush(stdout);
        dump_state();
        return cpu.exit_code;
    }
}
