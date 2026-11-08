#ifndef MUE_IO_H
#define MUE_IO_H
#include <stdint.h>
extern uint8_t ioport[0x10000];
uint8_t io_in8(uint16_t port);
void    io_out8(uint16_t port, uint8_t v);
void    io_out16(uint16_t port, uint16_t v);
#endif
