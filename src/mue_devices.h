/* mue_devices.h - emu8086's virtual devices, rebuilt inside mue.
 *
 * emu8086 ships eight device programs in DEVICES\ and talks to them through a
 * shared 64 KiB port file (C:\emu8086.io): a device reads/writes the bytes for
 * its ports, which is why an OUT instruction animates its window.  mue keeps
 * the same port contract but drives the windows in-process, so no external
 * .exe is needed.  re/DEVICES.md records each protocol and geometry.
 */
#ifndef MUE_DEVICES_H
#define MUE_DEVICES_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DEV_LED = 0,
    DEV_TRAFFIC,
    DEV_STEPPER,
    DEV_THERMO,
    DEV_ROBOT,
    DEV_PRINTER,
    DEV_SIMPLE,
    DEV_VGA,
    DEV_COUNT
};

/* Install the IN/OUT hook and the device animation timer.  Call once, after
 * mue_tree_ensure(), before the first window is shown. */
void devices_boot(void);

/* Put every device back to the state its VB form starts in.  Called on every
 * assemble-and-load, matching the original's reset_DEVICE. */
void devices_reset(void);

/* Open the windows named by #start=<device># or by a device filename anywhere
 * in the source (emu8086's documented activation rule). */
void devices_scan_source(const char *src);

/* Show one device.  Unknown names are handed to Windows, exactly as emu8086
 * launches a device .exe it has no internal model for. */
void devices_open(int id);
void devices_open_name(const char *filename);
int  devices_find(const char *filename);

/* Menu inventory, read from DEVICES\ at menu-build time. */
int         devices_menu_count(void);
const char *devices_menu_name(int i);   /* file name, e.g. "LED_Display.exe" */

/* The port hook installed into io.c.  `is_out` is 1 for OUT.  A handler may
 * change *value (the printer clears its data register after printing). */
int devices_io(int is_out, unsigned port, unsigned char *value);

/* True while at least one device window is on screen. */
int devices_any_open(void);

#ifdef __cplusplus
}
#endif
#endif
