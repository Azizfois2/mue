/* mue — MZ (EXE) container writer
 * exe.h
 *
 * emu8086 emits a minimal MZ image for `.exe_` targets; the header layout was
 * recovered from its own output (re/FINDINGS.md sec. 11):
 *
 *   offset  size  value
 *   0x00    2     'MZ'
 *   0x02    2     lastb   = code length (NOT size%512, as a DOS linker would)
 *   0x04    2     pages   = 2
 *   0x06    2     nreloc  = 0
 *   0x08    2     hdrsz   = 0x20 paragraphs (512 bytes)
 *   0x0A    2     minalloc= 0
 *   0x0C    2     maxalloc= 0xFFFF
 *   0x0E    2     ss      = 0
 *   0x10    2     sp      = 0
 *   0x12    2     csum    = 0
 *   0x14    2     ip      = 0
 *   0x16    2     cs      = 0
 *   0x18    2     reloff  = 0x1E
 *   0x1A    2     ovl     = 0
 *   0x1C..0x1FF   zero padding
 *   0x200..       code, verbatim
 */
#ifndef MUE_EXE_H
#define MUE_EXE_H

#include <stdint.h>

/* Build an MZ image around `code` (length `len`).
 * Returns a malloc'd buffer and sets *out_len; caller frees.
 * Returns NULL on allocation failure. */
uint8_t *exe_wrap(const uint8_t *code, int len, int *out_len);

/* As exe_wrap(), but also describing a `.stack N` segment laid out ahead of the
 * code (stack_size > 0) and where the relocatable `mov ax,@data` immediate sits
 * inside the code image (reloc_off, or -1 for none). */
uint8_t *exe_wrap_ex(const uint8_t *code, int len, int stack_size,
                     int reloc_off, int *out_len);

#endif /* MUE_EXE_H */
