/* mue - I/O port space and device hooks
 * io.c
 * The original routes ports through c:\emu8086.io ("EMUPORT") and exposes
 * READ_IO_BYTE / WRITE_IO_BYTE.  Here ports are a flat 64 KiB byte space,
 * which is enough for the emulator's own device model.  The UI installs a
 * hook so virtual devices (LED display, traffic lights, ...) can answer IN
 * and react to OUT without the core knowing anything about windows.
 */
#include "cpu.h"
#include "ioport.h"

uint8_t ioport[0x10000];

static io_hook_fn hook;

void io_set_hook(io_hook_fn fn)
{
    hook = fn;
}

uint8_t io_in8(uint16_t port)
{
    uint8_t v = ioport[port];
    if (hook) hook(0, port, &v);
    return v;
}

void io_out8(uint16_t port, uint8_t v)
{
    if (hook) hook(1, port, &v);
    ioport[port] = v;
}

void io_out16(uint16_t port, uint16_t v)
{
    io_out8(port, (uint8_t)(v & 0xFF));
    io_out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
}
