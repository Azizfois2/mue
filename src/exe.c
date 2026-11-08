/* mue — MZ (EXE) container writer
 * exe.c
 *
 * See exe.h for the recovered header layout. The payload is copied verbatim at
 * offset 0x200 (32 paragraphs), which is an exact match for emu8086's output on
 * every flat-`.code` example it ships.
 *
 * Details recovered by differential comparison against the 18 shipped
 * `*.exe_` images:
 *   1. checksum = (0xFFFF - word_sum_with_checksum_zeroed) & 0xFFFF
 *   2. word at 0x1C is a constant 1
 *   3. a source with `.stack N` puts the stack segment first (filled with
 *      0x54, 'T'), so CS begins after it and one relocation entry is emitted:
 *      `0001 : <cs>` pointing at the `mov ax,@data` immediate.
 *      div.exe_ is the only shipped example in this shape.
 */
#include "exe.h"

#include <stdlib.h>
#include <string.h>

#define EXE_HDR_PARAS 0x20      /* 32 paragraphs = 512 bytes */
#define EXE_HDR_BYTES (EXE_HDR_PARAS * 16)

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

uint8_t *exe_wrap(const uint8_t *code, int len, int *out_len)
{
    return exe_wrap_ex(code, len, 0, -1, out_len);
}

uint8_t *exe_wrap_ex(const uint8_t *code, int len, int stack_size,
                     int reloc_off, int *out_len)
{
    int nreloc = (stack_size > 0) ? 1 : 0;
    int total = EXE_HDR_BYTES + len;
    uint8_t *img;

    if (len < 0)
        return NULL;

    img = (uint8_t *)calloc(1, (size_t)total);
    if (!img)
        return NULL;

    /* --- MZ header --------------------------------------------------- */
    img[0] = 'M';
    img[1] = 'Z';
    put16(img + 0x02, (unsigned)(total % 512));         /* bytes last page */
    put16(img + 0x04, (unsigned)((total + 511) / 512)); /* page count      */
    put16(img + 0x06, (unsigned)nreloc);                /* reloc count     */
    put16(img + 0x08, EXE_HDR_PARAS);                   /* hdr paragraphs  */
    put16(img + 0x0A, 0);               /* minalloc                           */
    put16(img + 0x0C, 0xFFFF);          /* maxalloc                           */
    put16(img + 0x0E, 0);               /* ss                                 */
    put16(img + 0x10, (unsigned)stack_size); /* sp: top of stack segment      */
    put16(img + 0x12, 0);               /* checksum: filled in below          */
    put16(img + 0x14, 0);               /* ip                                 */
    put16(img + 0x16, 0);               /* cs                                 */
    put16(img + 0x18, 0x1E);            /* reloc table offset                 */
    put16(img + 0x1A, 0);               /* overlay                            */

    /* 0x1C carries a constant 1 in every shipped .exe_ image (verified across
     * all 18). Omitting it changes the word sum by exactly one and breaks the
     * checksum. */
    put16(img + 0x1C, 1);

    if (nreloc) {
        /* One entry at 0x1E: offset : segment of the @data immediate.
         * `cs` is the code segment in paragraphs (its offset within the
         * image divided by 16); div.exe_ resolves to 0x13. */
        int seg = (reloc_off >= 0) ? (reloc_off / 16) : 0;
        put16(img + 0x1E, 1);           /* reloc offset within segment        */
        put16(img + 0x20, (unsigned)seg); /* reloc segment                    */
        put16(img + 0x16, (unsigned)seg); /* cs                               */
    }

    if (len)
        memcpy(img + EXE_HDR_BYTES, code, (size_t)len);

    /* Checksum: computed as (0xFFFF - word_sum) with this field zeroed, which
     * reproduces every shipped emu8086 .exe_ image exactly.  An odd trailing
     * byte is padded with zero. */
    {
        unsigned long sum = 0;
        int i;
        put16(img + 0x12, 0);           /* zero the field before summing */
        for (i = 0; i < total; i += 2) {
            unsigned lo = img[i];
            unsigned hi = (i + 1 < total) ? img[i + 1] : 0;
            sum += lo | (hi << 8);
        }
        sum &= 0xFFFF;
        put16(img + 0x12, (unsigned)((0xFFFFu - sum) & 0xFFFFu));
    }

    *out_len = total;
    return img;
}
