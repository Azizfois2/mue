"""Read emu8086's runtime-built vtable out of a live process.

VB6 creates the project object's vtable in memory at startup, so the slot a
`call [obj+0x7AC]` reaches exists only at run time. This starts emu8086, reads
the global object pointer at 0x6214D0, follows it to the vtable and dumps the
slots the INT code uses.

Usage: python live_vtable.py [slot ...]
"""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys
import time

EXE = r'C:\emu8086\emu8086.exe'
BASE = 0x400000
OBJ_GLOBAL = 0x6214D0          # the project object, FINDINGS 25.3
SLOTS = [0x7AC, 0x7BC, 0x77C, 0x760, 0x714, 0x70C, 0x64]

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD),
                ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_char * 260)]


def find_pid(name='emu8086.exe'):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == -1:
        return None
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32First(snap, ctypes.byref(pe))
    pid = None
    while ok:
        if pe.szExeFile.decode('latin1').lower() == name:
            pid = pe.th32ProcessID
            break
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return pid


def reader(pid):
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit('OpenProcess failed: %d' % ctypes.get_last_error())

    def rd(addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n,
                                     ctypes.byref(got)):
            raise OSError('ReadProcessMemory @%08x: %d'
                          % (addr, ctypes.get_last_error()))
        return buf.raw[:got.value]
    return rd


def main():
    p = subprocess.Popen([EXE], cwd=r'C:\emu8086')
    print('started pid', p.pid)
    deadline = time.time() + 20
    rd = None
    while time.time() < deadline:
        time.sleep(1.0)
        pid = find_pid() or p.pid
        try:
            rd = reader(pid)
        except SystemExit as e:
            print(e)
            continue
        try:
            obj = struct.unpack('<I', rd(OBJ_GLOBAL, 4))[0]
        except OSError:
            continue
        print('obj  =', hex(obj))
        if BASE <= obj < 0x640000:
            break
    if rd is None:
        raise SystemExit('no reader')
    obj = struct.unpack('<I', rd(OBJ_GLOBAL, 4))[0]
    vt = struct.unpack('<I', rd(obj, 4))[0]
    print('vtable =', hex(vt))
    for off in (SLOTS if len(sys.argv) < 2 else [int(a, 16) for a in sys.argv[1:]]):
        try:
            tgt = struct.unpack('<I', rd(vt + off, 4))[0]
        except OSError as e:
            print('  %#6x -> <unreadable %s>' % (off, e))
            continue
        inmod = BASE <= tgt < 0x640000
        print('  %#6x -> %08x %s' % (off, tgt, 'in module' if inmod else 'outside'))
    p.terminate()


if __name__ == '__main__':
    main()
