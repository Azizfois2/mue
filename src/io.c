/* mue — I/O port space and device stubs
 * io.c
 * The original routes ports through c:\emu8086.io ("EMUPORT") and exposes
 * READ_IO_BYTE / WRITE_IO_BYTE.  Here ports are a flat 64 KiB byte space,
 * which is enough for the emulator's own device model.
 */
#include "cpu.h"
#include "io.h"

uint8_t ioport[0x10000];

uint8_t io_in8(uint16_t port)
{
    return ioport[port];
}

void io_out8(uint16_t port, uint8_t v)
{
    ioport[port] = v;
}

void io_out16(uint16_t port, uint16_t v)
{
    io_out8(port, (uint8_t)(v & 0xFF));
    io_out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
}
