#ifndef MUE_IO_H
#define MUE_IO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t ioport[0x10000];
/* A host device model (the UI's virtual devices) gets every access first.
 * Returning 1 means the model supplied/consumed the byte; either way the flat
 * port array is kept coherent for programs that read back an OUT. */
typedef int (*io_hook_fn)(int is_out, unsigned port, unsigned char *value);
void    io_set_hook(io_hook_fn fn);
uint8_t io_in8(uint16_t port);
void    io_out8(uint16_t port, uint8_t v);
void    io_out16(uint16_t port, uint16_t v);
#ifdef __cplusplus
}
#endif
#endif
