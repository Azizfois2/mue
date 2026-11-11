#!/usr/bin/env python3
"""INT 21h file services regression: 3Ch/3Dh/3Eh/3Fh/40h/41h/42h/43h/4Eh/4Fh/
56h, and 1Ah (DTA).

Each guest runs in a scratch directory, so the files it creates are the
oracle: the host reads them back.  Derived from FINDINGS 5.1 (3C/3D ->
0x5DDF80, 3E -> 0x5DE530, 42 -> 0x5DED70) plus the DOS contract the original
implements.

Exit 0 pass, 1 fail.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MUE = [os.path.join(HERE, "mue.exe"), os.path.join(HERE, "mue"),
       os.path.join(HERE, "..", "mue.exe"), os.path.join(HERE, "..", "mue")]

PROLOG = "org 100h\n"


def find_mue():
    for c in MUE:
        if os.path.exists(c):
            return os.path.abspath(c)
    return None


def run(mue, src, cwd, stdin_data=None, env=None):
    """Assemble + run in cwd; return (exit code, combined output)."""
    a = os.path.join(cwd, "t.asm")
    with open(a, "w") as f:
        f.write(src)
    e = None
    if env:
        e = dict(os.environ)
        e.update(env)
    # stdin is closed unless the check supplies one: a guest that waits for a
    # keystroke must hit end-of-input, never block the suite.
    if stdin_data is None:
        p = subprocess.run([mue, a], stdin=subprocess.DEVNULL, env=e,
                           capture_output=True, timeout=60, cwd=cwd)
    else:
        p = subprocess.run([mue, a], input=stdin_data, env=e,
                           capture_output=True, timeout=60, cwd=cwd)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def reg(out, name):
    for line in out.splitlines():
        for tok in line.strip().split():
            if tok.upper().startswith(name.upper() + "="):
                try:
                    return int(tok.split("=", 1)[1], 16)
                except ValueError:
                    pass
    return None


def guest_text(out):
    body = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("assembled"):
            continue
        if s.startswith("-----"):
            break
        body.append(line.rstrip("\r"))
    return "\n".join(body)


# 3Ch create, 40h write, 3Eh close: handle in BX, flags in DI.
T_CREATE = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fna
int 21h
mov bx,ax
mov ah,40h
mov cx,5
lea dx,msg
int 21h
pushf
pop di
mov ah,3eh
int 21h
ret
fna db 'a.txt',0
msg db 'hello'
"""

# 3Dh open, 3Fh read, then 3Ch/40h the buffer into b.txt: count in SI.
T_READ = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,3fh
mov cx,5
lea dx,buf
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,si
lea dx,buf
int 21h
mov ah,3eh
int 21h
mov ax,0
ret
fna db 'a.txt',0
fnb db 'b.txt',0
buf db 0,0,0,0,0
"""

# 42h: seek to 1 from the start, read 'ell' into b.txt, then ask for the size
# with AL=2, which comes back in DX:AX.
T_SEEK = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,0
mov cx,0
mov dx,1
int 21h
mov ah,3fh
mov cx,3
lea dx,buf
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,si
lea dx,buf
int 21h
mov ah,3eh
int 21h
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,2
mov cx,0
mov dx,0
int 21h
ret
fna db 'a.txt',0
fnb db 'b.txt',0
buf db 0,0,0
"""

# Errors: missing file is AX=2, bad handle is AX=6, both with CF set.
T_ERR = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fnx
int 21h
mov bp,ax
pushf
pop di
mov ax,0
mov ah,3eh
mov bx,30h
int 21h
pushf
pop si
ret
fnx db 'nope.txt',0
"""

# Reading at end of file is not an error: AX=0, CF clear.
T_EOF = PROLOG + """
mov ah,3dh
mov al,0
lea dx,fna
int 21h
mov bx,ax
mov ah,42h
mov al,2
mov cx,0
mov dx,0
int 21h
mov ah,3fh
mov cx,5
lea dx,buf
int 21h
pushf
pop di
ret
fna db 'a.txt',0
buf db 0,0,0,0,0
"""

# b.txt rewritten as 'xyz' (3 bytes) so the find tests have a known size.
T_TWO = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fnb
int 21h
mov bx,ax
mov ah,40h
mov cx,3
lea dx,msg
int 21h
mov ah,3eh
int 21h
ret
fnb db 'b.txt',0
msg db 'xyz'
"""

# 4Eh/4Fh over *.txt: DTA attribute at +0x15, name at +0x1E, and the third
# call must report no more files (AX=12h, CF set).
T_FIND = PROLOG + """
mov ah,4eh
mov cx,0
lea dx,pat
int 21h
pushf
pop di
mov bx,0
mov bl,[0095h]
mov bh,[009eh]
mov ah,4fh
int 21h
pushf
pop si
mov cl,[009eh]
mov ah,4fh
int 21h
mov bp,ax
pushf
pop dx
ret
pat db '*.txt',0
"""

# A directory is only reported when CX asks for it (bit 10h).
T_FINDDIR = PROLOG + """
mov ah,4eh
mov cx,10h
lea dx,dpat
int 21h
mov bl,[0095h]
pushf
pop di
mov ah,4eh
mov cx,0
lea dx,dpat
int 21h
mov bp,ax
pushf
pop si
ret
dpat db 'sub',0
"""

# 1Ah moves the DTA: the result must land at DS:0200, size at +0x1A.
T_FINDDTA = PROLOG + """
mov ah,1ah
mov dx,200h
int 21h
mov ah,4eh
mov cx,0
lea dx,bpat
int 21h
mov bh,0
mov bl,[021eh]
mov bp,[021ah]
ret
bpat db 'b.txt',0
"""

# 41h deletes a.txt; deleting it again reports file not found.
T_DEL = PROLOG + """
mov ah,41h
lea dx,fna
int 21h
pushf
pop di
mov ah,41h
lea dx,fna
int 21h
pushf
pop si
ret
fna db 'a.txt',0
"""

# Handle 0 is stdin through the keyboard queue: the read stops at CR, keeps
# it, and the second read picks up after it.
T_STDIN = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,8
mov dx,200h
int 21h
mov si,ax
mov ah,3fh
mov bx,0
mov cx,8
mov dx,210h
int 21h
mov bp,ax
mov ah,3ch
mov cx,0
lea dx,f1
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,200h
int 21h
mov ah,3eh
int 21h
mov ah,3ch
mov cx,0
lea dx,f2
int 21h
mov bx,ax
mov ah,40h
mov cx,bp
mov dx,210h
int 21h
mov ah,3eh
int 21h
ret
f1 db 's1.txt',0
f2 db 's2.txt',0
"""

# End of input is not an error: AX=0, CF clear.
# A scripted stdin that spans three lines and an exec: the parent reads the
# first line, the child reads the second and the third.  Each 3Fh read stops
# after CR, so one call is one line and the stream must carry on across 4Bh.
SL_ASM = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,5
mov dx,210h
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,f1
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,210h
int 21h
mov ah,3eh
int 21h
mov ah,3fh
mov bx,0
mov cx,5
mov dx,220h
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,f2
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,220h
int 21h
mov ah,3eh
int 21h
mov ah,4ch
mov al,0
int 21h
f1 db 'c1.txt',0
f2 db 'c2.txt',0
"""

T_SCRIPT_EXEC = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,5
mov dx,200h
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,f1
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,200h
int 21h
mov ah,3eh
int 21h
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'sl.com',0
f1 db 'p1.txt',0
"""

# Reads 4 bytes of stdin and writes whatever it got to sc.txt, so the
# scripted-input checks can look at the text itself and at the count.
T_STDIN_SCRIPT = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,4
mov dx,200h
int 21h
mov si,ax
mov ah,3ch
mov cx,0
lea dx,f1
int 21h
mov bx,ax
mov ah,40h
mov cx,si
mov dx,200h
int 21h
mov ah,3eh
int 21h
ret
f1 db 'sc.txt',0
"""

# Standard output redirected to a file: 09h, 40h on handle 1 and 02h all go
# there, and none of it reaches the screen.
T_STDOUT_REDIR = PROLOG + """
mov ah,9
lea dx,m1
int 21h
mov ah,40h
mov bx,1
mov cx,1
mov dx,offset m2
int 21h
mov ah,2
mov dl,'C'
int 21h
ret
m1 db 'A$'
m2 db 'B'
"""

# A child started by 4Bh writes into the same file the parent redirected to.
SO_ASM = PROLOG + """
mov ah,9
lea dx,m1
int 21h
mov ah,40h
mov bx,1
mov cx,1
mov dx,offset m2
int 21h
mov ah,4ch
mov al,0
int 21h
m1 db '2$'
m2 db '3'
"""

T_EXEC_STDOUT = PROLOG + """
mov ah,40h
mov bx,1
mov cx,1
mov dx,offset p1
int 21h
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov ah,40h
mov bx,1
mov cx,1
mov dx,offset p4
int 21h
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'so.com',0
p1 db '1'
p4 db '4'
"""

T_STDIN_EOF = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,4
mov dx,200h
int 21h
pushf
pop di
ret
"""

# 56h rename: the old name goes away and the new one keeps the contents.
T_RENAME = PROLOG + """
mov ah,3ch
mov cx,0
lea dx,fold
int 21h
mov bx,ax
mov ah,40h
mov cx,2
lea dx,msg
int 21h
mov ah,3eh
int 21h
mov ax,cs
mov es,ax
mov ah,56h
lea dx,fold
lea di,fnew
int 21h
pushf
pop bp
ret
fold db 'old.txt',0
fnew db 'new.txt',0
msg db 'hi'
"""

T_RENAME_MISS = PROLOG + """
mov ax,cs
mov es,ax
mov ah,56h
lea dx,fnope
lea di,fx
int 21h
pushf
pop si
ret
fnope db 'nope.txt',0
fx db 'x.txt',0
"""

# 43h: get (AL=0) then set read-only (AL=1) then get again.
T_ATTR = PROLOG + """
mov ah,43h
mov al,0
lea dx,fnew
int 21h
mov bx,cx
pushf
pop di
mov ah,43h
mov al,1
mov cx,1
lea dx,fnew
int 21h
pushf
pop si
mov ah,43h
mov al,0
lea dx,fnew
int 21h
mov bp,cx
ret
fnew db 'new.txt',0
"""

# 0Eh/19h/47h.  47h writes an ASCIIZ at DS:SI; the guest reads it back byte by
# byte into BX so the dump can be checked, and also prints it with 09h.
T_DRIVE = PROLOG + """
; The handlers write AL only (like the original, which stores +0xBA and
; leaves AH alone), so read AL into AX explicitly before the dump.
mov ah,19h
int 21h
mov ah,0
mov si,ax                ; AL = current drive (2 = C:) before 0Eh
mov ah,0eh
mov dl,3                 ; select D:
int 21h
mov ah,0
mov bx,ax                ; AL = 0x19 = 25 logical drives
mov ah,19h
int 21h
mov ah,0
mov di,ax                ; AL = 3
mov ah,0eh
mov dl,2                 ; back to C:
int 21h
ret
"""

T_GETDIR = PROLOG + """
mov ah,47h
xor dl,dl                ; default drive
lea si,buf
int 21h
pushf
pop bp                   ; CF
mov bx,ax                ; AX (should be 0001h)
mov al,[si]              ; first byte of the path
mov ah,0
mov dx,ax
mov ah,47h
mov dl,27                ; > 26 -> invalid drive
lea si,buf
int 21h
mov cx,ax
pushf
pop di                   ; CF
ret
buf db 80 dup(0)
"""

# 31h: terminate and stay resident.  Residency is impossible here, so the
# clone ends the program like 4Ch; the `mov bx,0DEADh` after the call must
# never run.
T_TSR = PROLOG + """
mov ah,31h
mov al,7
mov dx,20h
int 21h
mov bx,0deadh
ret
"""

# 4Bh: a missing program, an overlay request (AL=1, refused), then a real
# child that exits with 5 and prints before the parent does.  BL collects a
# bit for every answer that came back wrong.
# 4Bh AL=1: an overlay, loaded where the parameter block says (+0x10) and
# not run.  The guest prints 'L' when the call returns, far-calls the overlay
# at 8001h:0000 (header CS 1, since .code starts at 0x10), then 'P'.  0x9A is
# the 8086 far call; a `push imm16` + retf would need 0x68, which is 80186.
OVL_ASM = """
.model small
.data
msg db 'O$'
.code
mov ax,@data
mov ds,ax
mov ah,9
mov dx,offset msg
int 21h
retf
end
"""

T_OVERLAY = PROLOG + """
mov ax,4b01h
lea dx,oname
mov bx,offset pblk
push ds
pop es
int 21h
jnc ovok
mov bp,ax
or bp,0100h
jmp ov2
ovok:
mov bp,0
ov2:
mov ah,2
mov dl,'L'
int 21h
push ds
call 8001h:0
pop ds
mov ah,2
mov dl,'P'
int 21h
ret
pblk dw 0
dd 0
dd 0
dd 0
dw 0
dw 8000h
oname db 'ovl.exe',0
"""

# AL=1 with no load address in the block: the clone refuses it rather than
# guessing where to put the image.
T_OVERLAY_NOADDR = PROLOG + """
mov ax,4b01h
lea dx,oname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
jnc on1
or bp,0100h
on1:
ret
pblk dw 0
dd 0
dd 0
dd 0
dw 0
dw 0
oname db 'ovl.exe',0
"""

T_EXEC = PROLOG + """
mov bx,0
mov ah,4bh
mov al,0
lea dx,missing
int 21h
jnc e1
cmp ax,2
je e1
or bl,1
e1:
mov ah,4bh
mov al,1
lea dx,cname
int 21h
jnc e2
cmp ax,9
je e2
or bl,2
e2:
mov ah,4bh
mov al,0
lea dx,cname
int 21h
jnc e3
or bl,4
e3:
mov bp,ax
mov ah,9
lea dx,pmsg
int 21h
ret
missing db 'nosuch.com',0
cname db 'child.com',0
pmsg db 'parent$'
"""

CHILD_ASM = PROLOG + """
mov ah,9
lea dx,msg
int 21h
mov ah,4ch
mov al,5
int 21h
msg db 'child$'
"""

# The same child, leaving through 31h instead: its AL must become the exit
# code the 4Bh caller sees, which is how a TSR exit is observable at all.
CHILD_TSR_ASM = PROLOG + """
mov ah,9
lea dx,msg
int 21h
mov ah,31h
mov al,5
mov dx,100h
int 21h
msg db 'child$'
"""

# Nested exec: c1 prints, executes c2 (which prints and exits 9), then leaves
# with whatever c2's exit code was - so AX=9 proves it travelled two levels.
C1_ASM = PROLOG + """
mov ah,9
lea dx,m1
int 21h
mov ax,4b00h
lea dx,cname
mov bx,offset param
push ds
pop es
int 21h
mov ah,4ch
int 21h
m1 db 'c1$'
cname db 'c2.com',0
param dw 0
dd 0
dd 0
dd 0
"""

C2_ASM = PROLOG + """
mov ah,9
lea dx,m2
int 21h
mov ah,4ch
mov al,9
int 21h
m2 db 'c2$'
"""

T_EXEC_C1 = PROLOG + """
mov ax,4b00h
lea dx,c1name
mov bx,offset param
push ds
pop es
int 21h
mov bp,ax
mov ah,9
lea dx,pmsg
int 21h
ret
c1name db 'c1.com',0
param dw 0
dd 0
dd 0
dd 0
pmsg db 'parent$'
"""

# c3 prints its command tail, then the first string of the environment it was
# handed.  Both come from its PSP, so they can only work if 4Bh built one.
C3_ASM = PROLOG + """
mov al,[80h]
mov ah,0
mov bx,81h
add bx,ax
mov byte ptr [bx],'$'
mov ah,9
mov dx,81h
int 21h
mov ah,2
mov dl,'|'
int 21h
mov ax,[2ch]
test ax,ax
jz done
mov ds,ax
mov bx,0
scan:
cmp byte ptr [bx],0
je repl
inc bx
jmp scan
repl:
mov byte ptr [bx],'$'
mov dx,0
mov ah,9
int 21h
done:
mov ah,4ch
mov al,0
int 21h
"""

# 4Bh with a full parameter block: an environment block taken from 48h and
# filled with FOO=BAR, and the counted command tail "HI".
T_EXEC_ARGS = PROLOG + """
mov ah,48h
mov bx,1
int 21h
mov [envseg],ax
mov es,ax
mov si,offset envstr
xor di,di
mov cx,10
rep movsb
push ds
pop es
mov bx,offset paramblk
mov ax,[envseg]
mov [bx],ax
mov ax,offset cmdstr
mov [bx+2],ax
mov ax,ds
mov [bx+4],ax
mov ax,4b00h
lea dx,c3name
int 21h
mov bp,ax
ret
envseg dw 0
paramblk dw 0
dd 0
dd 0
dd 0
envstr db 'FOO=BAR',0,0,0
cmdstr db 2,'HI',13
c3name db 'c3.com',0
"""

# The boot program's own PSP: 0x16 (no parent, so it points at itself) and the
# job file table at 0x18, which 3Ch has to keep current.
T_PSP_BOOT = PROLOG + """
mov ah,3ch
xor cx,cx
lea dx,fname
int 21h
mov al,1
mov bx,[18h]
cmp bx,8180h
jne out
mov al,2
mov bx,[1ah]
cmp bx,8382h
jne out
mov al,3
mov bx,[1dh]
cmp bx,0ff00h
jne out
mov al,4
mov bx,[32h]
cmp bx,14h
jne out
mov al,5
mov bx,[34h]
cmp bx,18h
jne out
mov al,6
mov bx,[36h]
push ds
pop cx
cmp bx,cx
jne out
mov al,7
mov bx,[16h]
cmp bx,cx
jne out
mov al,0
out:
mov ah,4ch
int 21h
fname db 'psp.txt',0
"""

# A child's PSP as 4Bh builds it: the inherited handle table, its size and
# pointer fields, its own segment, the parent segment, and handle 5 usable
# because the parent had it open.
CPSP_ASM = PROLOG + """
mov bp,ds
mov al,1
mov bx,[18h]
cmp bx,8180h
jne out
mov al,2
mov bx,[1dh]
cmp bx,0ff00h
jne out
mov al,3
mov bx,[32h]
cmp bx,14h
jne out
mov al,4
mov bx,[34h]
cmp bx,18h
jne out
mov al,5
mov bx,[36h]
cmp bx,bp
jne out
mov al,6
mov bx,[16h]
cmp bx,0100h
jne out
mov al,7
mov ah,3fh
mov bx,5
mov cx,5
lea dx,buf
int 21h
cmp ax,5
jne out
mov bx,offset buf
mov byte ptr [bx+5],'$'
mov ah,9
lea dx,buf
int 21h
mov al,0
out:
mov ah,4ch
int 21h
buf db '-----','$'
"""

# Opens data.txt (handle 5) and execs cpsp.com, which must find that handle in
# its own table and read from it.
T_PSP_INHERIT = PROLOG + """
mov ax,3d00h
lea dx,dname
int 21h
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
mov ah,9
lea dx,pmsg
int 21h
ret
pblk dw 0
dd 0
dd 0
dd 0
dname db 'data.txt',0
cname db 'cpsp.com',0
pmsg db 'p$'
"""

# Reports the high byte of its own PSP:0x16 through its exit code, so the
# parent can see which PSP segment 4Bh recorded as its parent.
CPSP_PARENT_ASM = PROLOG + """
mov al,[17h]
mov ah,4ch
int 21h
"""

# Execs pchild.com and passes its exit code on, so a two-level chain can report
# the segment a grandchild believes its parent has.
C1P_ASM = PROLOG + """
mov ax,4b00h
lea dx,pname
mov bx,offset pblk
push ds
pop es
int 21h
mov ah,4ch
int 21h
pblk dw 0
dd 0
dd 0
dd 0
pname db 'pchild.com',0
"""

T_EXEC_PARENT = PROLOG + """
mov ax,4b00h
lea dx,pname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
pname db 'pchild.com',0
"""

# A child's PSP:0x02 is the first paragraph past its block.  The topmost
# window ends at the megabyte, where the segment does not fit in a word: it
# must report 0xFFFF (0xFF in the high byte) and not wrap to 0x0000.
TOP_ASM = PROLOG + """
mov bx,0
mov ax,[bx+2]
mov al,ah
mov ah,4ch
int 21h
"""

# Runs `top.com` as a child, for the MZ maxalloc check below.
T_EXEC_TOP = PROLOG + """
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'top.com',0
"""

# One level down, so the child of this child sits in the 0xE000 window and its
# top of memory is still an exact 0xF000.
MID_ASM = PROLOG + """
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov ah,4ch
int 21h
pblk dw 0
dd 0
dd 0
dd 0
cname db 'top.com',0
"""

# A module that reports its own child PSP:0x02 through the exit code, so a
# header's maxalloc can be read back: top = 0xF000 + 0x10 + maxalloc.
MAXA_ASM = """
mov bx,0
mov ax,[bx+2]
mov al,ah
mov ah,4ch
int 21h
"""

# Runs `maxa.exe`, an MZ image whose header caps maxalloc at 0x110 paragraphs:
# the block ends early, so the child sees 0xF120 rather than the whole window.
T_EXEC_MAXA = PROLOG + """
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'maxa.exe',0
"""

T_EXEC_TOP_MID = PROLOG + """
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'mid.com',0
"""

# The boot program's own PSP:0x02, and the free-memory sum a program makes
# from it (top - PSP).
T_BOOT_TOP = PROLOG + """
mov bx,0
mov ax,[bx+2]
mov bp,ax
mov dx,ds
sub ax,dx
mov si,ax
ret
"""

# Execs itself until the window limit refuses the exec, so every window level
# runs the same test: its own (PSP:0x02 - PSP) must be positive.  Each level
# returns the number of levels it saw, or 0xEE if its sum was zero or wrapped.
CHAIN_ASM = PROLOG + """
mov bx,0
mov ax,[bx+2]
mov dx,ds
sub ax,dx
jbe bad
mov ax,4b00h
lea dx,nm
mov bx,offset pblk
push ds
pop es
int 21h
jnc again
mov al,1
jmp out2
again:
cmp al,0EEh
je out2
inc al
out2:
mov ah,4ch
int 21h
bad:
mov ax,4cEEh
int 21h
pblk dw 0
dd 0
dd 0
dd 0
nm db 'chain.com',0
"""

T_CHAIN = PROLOG + """
mov ax,4b00h
lea dx,nm
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
nm db 'chain.com',0
"""

T_EXEC_PARENT_NEST = PROLOG + """
mov ax,4b00h
lea dx,pname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
pname db 'c1p.com',0
"""

# A child that opens a file and never closes it: the handle must not survive
# into the parent, which is why the next open has to get slot 1 again.
OPENER_ASM = PROLOG + """
mov ax,3d00h
lea dx,bname
int 21h
mov ah,4ch
mov al,0
int 21h
bname db 'b.txt',0
"""

T_EXEC_LEAK = PROLOG + """
mov ax,3d00h
lea dx,aname
int 21h
mov ax,4b00h
lea dx,oname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
mov ax,3d00h
lea dx,cname
int 21h
mov bx,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
aname db 'a.txt',0
cname db 'c.txt',0
oname db 'opener.com',0
"""

# Redirected stdin must survive an exec: the parent takes the first line, the
# child the next one, and neither is echoed (stdin here is a file, not a
# console).
T_EXEC_STDIN = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,6
mov dx,200h
int 21h
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
mov ah,9
lea dx,pmsg
int 21h
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'sread.com',0
pmsg db 'P$'
"""

# A `.model` source with no `.stack`: .data starts the image and .code follows
# it, so CS must be 1 (0x10/16) and the @data word must start as 0 - the load
# segment itself - for `mov ds,ax` to find the data.
MZMODEL_ASM = """
.model small
.data
msg db 'MZDATA$'
.code
mov ax,@data
mov ds,ax
mov ah,9
mov dx,offset msg
int 21h
mov ax,4c00h
int 21h
end
"""

T_EXEC_MZ_MODEL = PROLOG + """
mov ax,4b00h
lea dx,cname
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
cname db 'mzm.exe',0
"""

# Reads the next line from the inherited stdin and prints it.
SREAD_ASM = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,6
mov dx,200h
int 21h
mov bx,200h
add bx,ax
mov byte ptr [bx],'$'
mov ah,9
mov dx,200h
int 21h
mov ah,4ch
mov al,0
int 21h
"""

# The module of the MZ child below, assembled flat (base 0) and then wrapped in
# a header written by mz_image(): 'MZ$' sits at module offset 0, the code at
# 0x10, and the first instruction's immediate is what the relocation fixes up.
MZCHILD_ASM = """
db 'MZ$'
db 0,0,0,0,0,0,0,0,0,0,0,0,0
mov ax,0
mov ds,ax
mov dx,0
mov ah,9
int 21h
mov ax,sp
and al,0fh
add al,'0'
mov dl,al
mov ah,2
int 21h
mov ax,4c07h
int 21h
"""


def mz_image(module, minalloc=0, ss=3, sp=0x0127, cs=1, ip=0, maxalloc=0xFFFF,
             reloc=True):
    """Wrap a load module in an MZ header.

    By default there is one relocation entry, (segment cs, offset 1): the
    immediate of `mov ax,0`, which must come back as the load segment after
    loading.  reloc=False is for modules that have no such word.
    """
    hdr = bytearray(0x200)
    total = 0x200 + len(module)          # the header is part of the file size
    hdr[0:2] = b"MZ"
    hdr[2:4] = (total % 512).to_bytes(2, "little")        # bytes last page
    hdr[4:6] = ((total + 511) // 512).to_bytes(2, "little")  # page count
    hdr[6:8] = (1 if reloc else 0).to_bytes(2, "little")  # relocation count
    hdr[8:10] = (0x20).to_bytes(2, "little")             # header paragraphs
    hdr[0x0A:0x0C] = minalloc.to_bytes(2, "little")
    hdr[0x0C:0x0E] = maxalloc.to_bytes(2, "little")
    hdr[0x0E:0x10] = ss.to_bytes(2, "little")
    hdr[0x10:0x12] = sp.to_bytes(2, "little")
    hdr[0x14:0x16] = ip.to_bytes(2, "little")
    hdr[0x16:0x18] = cs.to_bytes(2, "little")
    hdr[0x18:0x1A] = (0x1E).to_bytes(2, "little")        # relocation table
    if reloc:
        hdr[0x1E:0x20] = (1).to_bytes(2, "little")       # ... offset
        hdr[0x20:0x22] = cs.to_bytes(2, "little")        # ... segment
    return bytes(hdr) + module


# 4Bh of an MZ child: ES:BX is the parameter block, and AX comes back as the
# child's exit code (7).
T_EXEC_MZ = PROLOG + """
mov ax,4b00h
lea dx,mzc
mov bx,offset pblk
push ds
pop es
int 21h
mov si,ax
mov ah,9
lea dx,pmsg
int 21h
ret
pblk dw 0
dd 0
dd 0
dd 0
mzc db 'mzch.exe',0
pmsg db 'P$'
"""

# The same call with an image that asks for more memory than a child window
# has: AX must come back 8.
T_EXEC_MZ_MEM = PROLOG + """
mov ax,4b00h
lea dx,bigc
mov bx,offset pblk
push ds
pop es
int 21h
mov bp,ax
ret
pblk dw 0
dd 0
dd 0
dd 0
bigc db 'big.exe',0
"""

# 3Fh on a redirected handle 0: the bytes are read, not echoed.
T_STDIN_SILENT = PROLOG + """
mov ah,3fh
mov bx,0
mov cx,3
mov dx,200h
int 21h
mov ah,2
mov dl,'K'
int 21h
ret
"""

# 0Ah on a redirected stdin: same rule for the buffered line reader.
T_STDIN_LINE_SILENT = PROLOG + """
mov ah,0ah
lea dx,buf
int 21h
mov ah,2
mov dl,'K'
int 21h
ret
buf db 8,0,0,0,0,0,0,0,0
"""

# 0Bh must say a redirected stream has input waiting, even though the queue is
# filled lazily.
T_STDIN_STATUS = PROLOG + """
mov ah,0bh
int 21h
mov bp,ax
ret
"""

T_EXEC_TSR = PROLOG + """
mov ah,4bh
mov al,0
lea dx,tname
int 21h
mov bp,ax
ret
tname db 'tsr.com',0
"""

# 48h/49h/4Ah with the original's no-op behaviour switched on: nothing at all
# may change - AX keeps AH=48h, BX keeps its value, and CF stays as set.
T_MEM_NOOP = PROLOG + """
mov ax,4800h
mov bx,5678h
stc
int 21h
pushf
pop di
ret
"""

# 36h: every drive reports the same fixed picture; DL > 26 is AX=0xFFFF.
# The guest does the invalid drive first, so DX still holds the valid answer at
# the end; CX is a sentinel 36h must not touch.
T_FREESPACE = PROLOG + """
mov cx,1234h
mov ah,36h
mov dl,27
int 21h
mov si,ax
pushf
pop di
mov ah,36h
mov dl,3
int 21h
mov bp,ax
ret
"""

# 48h/49h/4Ah: two blocks, a grow that cannot fit, then a double free.  CL
# collects a bit for every CF that came back wrong (set where it should be
# clear, clear where it should be set), so CL=0 at the end means every call
# answered the way DOS says.  The grow asks for 0xFFF0 paragraphs, more than
# the whole arena, so it can never succeed.
T_MEMORY = PROLOG + """
mov cl,0
mov ah,48h
mov bx,10h
int 21h
jnc a1
or cl,1
a1:
mov bp,ax
mov ah,48h
mov bx,20h
int 21h
jnc a2
or cl,2
a2:
mov si,ax
mov ah,4ah
mov es,si
mov bx,0fff0h
int 21h
jc g1
or cl,4
g1:
mov dx,ax
mov ah,49h
mov es,bp
int 21h
jnc f1
or cl,8
f1:
mov ah,49h
mov es,bp
int 21h
jc f2
or cl,16
f2:
mov di,ax
ret
"""

# Round trip: mkdir, chdir, get the directory, climb back out, rmdir.  The
# guest prints each 47h answer (converted to '$' in place) so the host can see
# the path itself, and leaves the last chdir's CF/AX in BP/BX.
T_CHDIR = PROLOG + """
mov ah,39h
lea dx,dname
int 21h
mov ah,3bh
lea dx,dname
int 21h
mov [ok_in],0
jnc in_ok
mov [ok_in],1
in_ok:
mov ah,47h
xor dl,dl
lea si,buf1
int 21h
lea si,buf1
call show
mov ah,3bh
lea dx,updir
int 21h
mov [ok_up],0
jnc up_ok
mov [ok_up],1
up_ok:
mov ah,47h
xor dl,dl
lea si,buf2
int 21h
lea si,buf2
call show
mov ah,3bh
lea dx,baddir
int 21h
pushf
pop bp
mov bx,ax
mov ah,3ah
lea dx,dname
int 21h
ret

show:
mov bx,0
scan:
cmp byte ptr [si+bx],0
je repl
inc bx
jmp scan
repl:
mov byte ptr [si+bx],'$'
mov ah,9
mov dx,si
int 21h
mov ah,2
mov dl,10
int 21h
ret

dname db 'rt1',0
updir db '..',0
baddir db 'nosuchdir',0
ok_in db 0
ok_up db 0
buf1 db 80 dup(0)
buf2 db 80 dup(0)
"""

# 39h/3Ah.  The guests leave the directory for the host to inspect, so the
# checks can look at both the registers and the filesystem.
T_MKDIR = PROLOG + """
mov ah,39h
lea dx,dname
int 21h
pushf
pop si
mov ah,39h
lea dx,dname
int 21h
mov di,ax
pushf
pop bp
ret
dname db 'mk_a',0
"""

T_RMDIR = PROLOG + """
mov ah,3ah
lea dx,dname
int 21h
pushf
pop si
mov ah,3ah
lea dx,dname
int 21h
mov di,ax
pushf
pop bp
ret
dname db 'mk_a',0
"""

T_ATTR_ERR = PROLOG + """
mov ah,43h
mov al,0
lea dx,fnope
int 21h
mov bp,ax
pushf
pop di
mov ah,43h
mov al,2
lea dx,fnope
int 21h
mov bx,ax
pushf
pop si
ret
fnope db 'nope.txt',0
"""

# INT 16h AH=01 waits for a key and leaves it for the next AH=00.
T_INT16_PEEK = PROLOG + """
mov ah,1
int 16h
pushf
pop di
mov bx,ax
mov ah,0
int 16h
mov cx,ax
ret
"""

# With no input at all it must report "no key" rather than wait forever.
T_INT16_NONE = PROLOG + """
mov ah,1
int 16h
pushf
pop di
mov bx,ax
ret
"""

# INT 21h AH=01 echoes what it reads, in order.
T_INT21_01 = PROLOG + """
mov ah,1
int 21h
mov ah,1
int 21h
ret
"""

# Handle 1 is stdout: the bytes must reach the console, not a file.
T_STDOUT = PROLOG + """
mov ah,40h
mov bx,1
mov cx,2
lea dx,hi
int 21h
ret
hi db 'ok'
"""


def main():
    mue = find_mue()
    if mue is None:
        print("SKIP: no mue binary")
        return 77
    fails = []

    def check(name, cond, extra=""):
        print(("PASS " if cond else "FAIL ") + name +
              (" " + str(extra) if extra and not cond else ""))
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as d:
        def host(name):
            try:
                with open(os.path.join(d, name), "rb") as f:
                    return f.read()
            except OSError:
                return None

        rc, out = run(mue, T_CREATE, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        check("create-returns-handle-5", bx == 5, bx if bx is not None else "AX?")
        check("create-write-cf-clear", di is not None and not (di & 1), di)
        check("create-write-hits-host", host("a.txt") == b"hello", host("a.txt"))

        rc, out = run(mue, T_READ, d)
        si = reg(out, "SI")
        check("read-count", si == 5, si)
        check("read-roundtrip", host("b.txt") == b"hello", host("b.txt"))

        rc, out = run(mue, T_SEEK, d)
        dx = reg(out, "DX")
        check("seek-read-from-offset", host("b.txt") == b"ell", host("b.txt"))
        check("seek-end-reports-size", dx == 5, dx)

        rc, out = run(mue, T_ERR, d)
        bp = reg(out, "BP")
        di = reg(out, "DI")
        si = reg(out, "SI")
        ax = reg(out, "AX")
        check("open-missing-file-not-found", bp == 2, bp)
        check("open-missing-sets-cf", di is not None and (di & 1), di)
        check("close-bad-handle-invalid", ax == 6, ax)
        check("close-bad-handle-sets-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_EOF, d)
        ax = reg(out, "AX")
        di = reg(out, "DI")
        check("read-at-eof-returns-zero", ax == 0, ax)
        check("read-at-eof-cf-clear", di is not None and not (di & 1), di)

        rc, out = run(mue, T_TWO, d)
        rc, out = run(mue, T_FIND, d)
        di = reg(out, "DI")
        si = reg(out, "SI")
        bx = reg(out, "BX")
        cx = reg(out, "CX")
        bp = reg(out, "BP")
        dx = reg(out, "DX")
        check("find-first-succeeds", di is not None and not (di & 1), di)
        # The guest puts the DTA attribute in BL and the name's first byte in
        # BH, so BX reads as name<<8 | attribute.
        check("find-first-file-attribute",
              bx is not None and (bx & 0xFF) == 0x20,
              hex(bx) if bx is not None else None)
        check("find-first-name-in-dta",
              bx is not None and (bx >> 8) in (ord("A"), ord("B")),
              hex(bx) if bx is not None else None)
        check("find-next-succeeds", si is not None and not (si & 1), si)
        check("find-next-advances",
              cx is not None and (cx & 0xFF) != ((bx or 0) >> 8),
              hex(cx) if cx is not None else None)
        check("find-next-ends-with-12h", bp == 0x12, bp)
        check("find-next-ends-with-cf", dx is not None and (dx & 1), dx)

        os.mkdir(os.path.join(d, "sub"))
        rc, out = run(mue, T_FINDDIR, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        bp = reg(out, "BP")
        si = reg(out, "SI")
        check("find-directory-needs-10h", bx is not None and (bx & 0xFF) == 0x10,
              hex(bx) if bx is not None else None)
        check("find-directory-first-cf-clear",
              di is not None and not (di & 1), di)
        check("find-directory-without-10h-fails", bp == 0x12, bp)
        check("find-directory-without-10h-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_FINDDTA, d)
        bx = reg(out, "BX")
        bp = reg(out, "BP")
        check("set-dta-redirects", bx is not None and (bx & 0xFF) == ord("B"),
              hex(bx) if bx is not None else None)
        check("find-reports-size", bp == 3, bp)

        rc, out = run(mue, T_DEL, d)
        di = reg(out, "DI")
        si = reg(out, "SI")
        ax = reg(out, "AX")
        check("delete-removes-file", host("a.txt") is None, host("a.txt"))
        check("delete-first-cf-clear", di is not None and not (di & 1), di)
        check("delete-again-not-found", ax == 2, ax)
        check("delete-again-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_STDIN, d, b"hi\r\nrest")
        si = reg(out, "SI")
        bp = reg(out, "BP")
        check("stdin-read-stops-at-cr", si == 3, si)
        check("stdin-keeps-cr", host("s1.txt") == b"hi\r", host("s1.txt"))
        check("stdin-continues-after-cr", bp == 5, bp)
        check("stdin-rest-unconsumed", host("s2.txt") == b"\nrest",
              host("s2.txt"))

        rc, out = run(mue, T_STDIN_EOF, d, b"")
        ax = reg(out, "AX")
        di = reg(out, "DI")
        check("stdin-eof-returns-zero", ax == 0, ax)
        check("stdin-eof-cf-clear", di is not None and not (di & 1), di)

        # dos_feed_stdin: MUE_STDIN_TEXT scripts stdin from a string, with no
        # host stream at all, and it wins over whatever the host offers.
        rc, out = run(mue, T_STDIN_SCRIPT, d,
                      env={"MUE_STDIN_TEXT": "abc"})
        check("stdin-script-reads-without-host", reg(out, "SI") == 3,
              reg(out, "SI"))
        check("stdin-script-text", host("sc.txt") == b"abc", host("sc.txt"))

        rc, out = run(mue, T_STDIN_SCRIPT, d, b"zzzz",
                      env={"MUE_STDIN_TEXT": "ab"})
        check("stdin-script-ends-at-script", reg(out, "SI") == 2,
              reg(out, "SI"))
        check("stdin-script-beats-host-stream", host("sc.txt") == b"ab",
              host("sc.txt"))

        # One script, several lines, across an exec: the parent takes the first
        # line and the child reads on from exactly where the parent stopped.
        with open(os.path.join(d, "sl.asm"), "w") as f:
            f.write(SL_ASM)
        subprocess.run([mue, "-a", "sl.asm", "sl.com"], cwd=d,
                       capture_output=True, timeout=60)

        rc, out = run(mue, T_SCRIPT_EXEC, d,
                      env={"MUE_STDIN_TEXT": "one\rtwo\rthree"})
        check("stdin-script-spans-exec", (reg(out, "BP") or 0) == 0,
              reg(out, "BP"))
        check("stdin-script-parent-first-line",
              host("p1.txt") == b"one\r", host("p1.txt"))
        check("stdin-script-child-second-line",
              host("c1.txt") == b"two\r", host("c1.txt"))
        check("stdin-script-child-third-line",
              host("c2.txt") == b"three", host("c2.txt"))

        # Standard output redirected to a file: 09h, 02h and 40h on handle 1
        # all land there, and an exec'd child writes to the same file.
        with open(os.path.join(d, "so.asm"), "w") as f:
            f.write(SO_ASM)
        subprocess.run([mue, "-a", "so.asm", "so.com"], cwd=d,
                       capture_output=True, timeout=60)

        rc, out = run(mue, T_STDOUT_REDIR, d,
                      env={"MUE_STDOUT_FILE": "redir.txt"})
        check("stdout-redirect-file", host("redir.txt") == b"ABC",
              host("redir.txt"))
        check("stdout-redirect-off-screen", guest_text(out).strip() == "",
              repr(guest_text(out)[:40]))

        rc, out = run(mue, T_EXEC_STDOUT, d,
                      env={"MUE_STDOUT_FILE": "redir2.txt"})
        check("exec-child-inherits-stdout", host("redir2.txt") == b"1234",
              host("redir2.txt"))
        check("exec-child-stdout-off-screen", guest_text(out).strip() == "",
              repr(guest_text(out)[:40]))

        rc, out = run(mue, T_RENAME, d)
        bp = reg(out, "BP")
        check("rename-succeeds", bp is not None and not (bp & 1), bp)
        check("rename-old-name-gone", host("old.txt") is None, host("old.txt"))
        check("rename-keeps-contents", host("new.txt") == b"hi",
              host("new.txt"))

        rc, out = run(mue, T_RENAME_MISS, d)
        ax = reg(out, "AX")
        si = reg(out, "SI")
        check("rename-missing-not-found", ax == 2, ax)
        check("rename-missing-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_ATTR, d)
        bx = reg(out, "BX")
        di = reg(out, "DI")
        si = reg(out, "SI")
        bp = reg(out, "BP")
        check("attrs-get-file-archive", bx == 0x20, hex(bx) if bx is not None else None)
        check("attrs-get-cf-clear", di is not None and not (di & 1), di)
        check("attrs-set-succeeds", si is not None and not (si & 1), si)
        check("attrs-readonly-readable", bp is not None and (bp & 1), bp)
        try:                                        # let the cleanup work
            os.chmod(os.path.join(d, "new.txt"), 0o666)
        except OSError:
            pass                                    # the checks already failed

        rc, out = run(mue, T_ATTR_ERR, d)
        bp = reg(out, "BP")
        di = reg(out, "DI")
        bx = reg(out, "BX")
        si = reg(out, "SI")
        check("attrs-missing-not-found", bp == 2, bp)
        check("attrs-missing-cf", di is not None and (di & 1), di)
        check("attrs-bad-subfunction", bx == 1, bx)
        check("attrs-bad-subfunction-cf", si is not None and (si & 1), si)

        rc, out = run(mue, T_DRIVE, d)
        si = reg(out, "SI")
        bx = reg(out, "BX")
        di = reg(out, "DI")
        check("current-drive-is-c", si == 2, si)
        check("select-disk-drive-count", bx == 0x19, hex(bx) if bx is not None else None)
        check("select-disk-takes-effect", di == 3, di)

        rc, out = run(mue, T_GETDIR, d)
        bp = reg(out, "BP")
        bx = reg(out, "BX")
        dx = reg(out, "DX")
        cx = reg(out, "CX")
        di = reg(out, "DI")
        check("getdir-cf-clear", bp is not None and not (bp & 1), bp)
        check("getdir-returns-1", bx == 1, hex(bx) if bx is not None else None)
        # The path has to start with a real directory name: the check runs in a
        # temp dir, so "temp" on POSIX and a drive-less Windows path otherwise.
        check("getdir-first-char-not-separator",
              dx is not None and dx not in (0, ord("/"), ord("\\")), dx)
        check("getdir-invalid-drive", cx == 0x0F, cx)
        check("getdir-invalid-drive-cf", di is not None and (di & 1), di)

        # 31h: the handler must end the program, not fall through.
        rc, out = run(mue, T_TSR, d)
        check("tsr-terminates", reg(out, "BX") == 0, reg(out, "BX"))

        # 4Bh: child program, loaded and run, its output before the parent's.
        with open(os.path.join(d, "child.asm"), "w") as f:
            f.write(CHILD_ASM)
        subprocess.run([mue, "-a", "child.asm", "child.com"], cwd=d,
                       capture_output=True, timeout=60)
        rc, out = run(mue, T_EXEC, d)
        txt = guest_text(out)
        check("exec-runs-child-first",
              "child" in txt and "parent" in txt and
              txt.index("child") < txt.index("parent"), repr(txt))
        check("exec-reports-child-exit-code",
              reg(out, "BP") == 5, reg(out, "BP"))
        check("exec-error-answers", reg(out, "BX") == 0, reg(out, "BX"))

        # Nested exec: the grandchild's exit code has to come back two levels.
        for nm, src in (("c1", C1_ASM), ("c2", C2_ASM), ("c3", C3_ASM)):
            with open(os.path.join(d, nm + ".asm"), "w") as f:
                f.write(src)
            subprocess.run([mue, "-a", nm + ".asm", nm + ".com"], cwd=d,
                           capture_output=True, timeout=60)
        rc, out = run(mue, T_EXEC_C1, d)
        txt = guest_text(out)
        check("exec-nested",
              reg(out, "BP") == 9 and "c1" in txt and "c2" in txt and
              txt.index("c1") < txt.index("c2") < txt.index("parent"),
              (reg(out, "BP"), repr(txt)))

        # Command tail and environment, both from the child's own PSP.
        rc, out = run(mue, T_EXEC_ARGS, d)
        txt = guest_text(out)
        check("exec-command-tail", "HI|" in txt.replace("\r", ""), repr(txt))
        check("exec-environment", "FOO=BAR" in txt, repr(txt))
        check("exec-args-returns-child-code", reg(out, "BP") == 0, reg(out, "BP"))

        # ---- the PSP 4Bh builds ------------------------------------------
        # Its own PSP: parent segment 0x16 and the job file table 0x18, which
        # 3Ch must update for the process that is running.
        rc, out = run(mue, T_PSP_BOOT, d)
        ax = (reg(out, "AX") or 0) & 0xFF
        check("psp-boot-jft", ax == 0, ax)

        for nm, src in (("cpsp", CPSP_ASM), ("pchild", CPSP_PARENT_ASM),
                        ("c1p", C1P_ASM), ("opener", OPENER_ASM),
                        ("top", TOP_ASM), ("mid", MID_ASM),
                        ("chain", CHAIN_ASM)):
            with open(os.path.join(d, nm + ".asm"), "w") as f:
                f.write(src)
            subprocess.run([mue, "-a", nm + ".asm", nm + ".com"], cwd=d,
                           capture_output=True, timeout=60)

        # A child's table: devices in 0-2, the inherited file in 5, its size,
        # offset and segment, the parent PSP, and the handle itself readable.
        with open(os.path.join(d, "data.txt"), "wb") as f:
            f.write(b"HELLO")
        rc, out = run(mue, T_PSP_INHERIT, d)
        txt = guest_text(out)
        check("psp-child-layout", reg(out, "BP") == 0, reg(out, "BP"))
        check("exec-inherits-handles", "HELLO" in txt, repr(txt))

        # 0x16 names the PSP of the process that called 4Bh: the program mue
        # loaded at 0x0100, or the window of the child doing the nested exec.
        rc, out = run(mue, T_EXEC_PARENT, d)
        check("psp-parent-segment", reg(out, "BP") == 1, reg(out, "BP"))
        rc, out = run(mue, T_EXEC_PARENT_NEST, d)
        check("psp-parent-nested", reg(out, "BP") == 0xF0, reg(out, "BP"))

        # Top of memory: 0xFFFF in the topmost window (it ends at the
        # megabyte), the exact 0xF000 one level down.
        rc, out = run(mue, T_EXEC_TOP, d)
        check("psp-child-top-of-memory", (reg(out, "BP") or 0) & 0xFF == 0xFF,
              reg(out, "BP"))
        rc, out = run(mue, T_EXEC_TOP_MID, d)
        check("psp-grandchild-top-of-memory",
              (reg(out, "BP") or 0) & 0xFF == 0xF0, reg(out, "BP"))

        # The boot program has a PSP too, and its block ends at the top of
        # conventional memory (0xA000 paragraphs), not at zero.
        rc, out = run(mue, T_BOOT_TOP, d)
        check("boot-psp-top-of-memory", reg(out, "BP") == 0xA000, reg(out, "BP"))
        check("boot-free-memory-positive", reg(out, "SI") == 0x9F00,
              reg(out, "SI"))

        # Every window level: (PSP:0x02 - PSP) must stay positive, so the
        # six levels of the chain all report in.
        rc, out = run(mue, T_CHAIN, d)
        check("child-free-memory-all-levels", reg(out, "BP") == 6,
              reg(out, "BP"))

        # A child's unclosed handles die with it; the parent's slot 1 is free
        # again, so the next open gets handle 6 and not 7.
        for nm in ("a.txt", "b.txt", "c.txt"):
            with open(os.path.join(d, nm), "wb") as f:
                f.write(b"x")
        rc, out = run(mue, T_EXEC_LEAK, d)
        check("exec-child-handles-do-not-leak",
              reg(out, "BX") == 6 and reg(out, "BP") == 0,
              (reg(out, "BX"), reg(out, "BP")))

        # Redirected stdin across an exec: the parent consumes the first line,
        # the child the second, and a file is never echoed.
        with open(os.path.join(d, "sread.asm"), "w") as f:
            f.write(SREAD_ASM)
        subprocess.run([mue, "-a", "sread.asm", "sread.com"], cwd=d,
                       capture_output=True, timeout=60)
        rc, out = run(mue, T_EXEC_STDIN, d, stdin_data=b"first\rsecond\r")
        txt = guest_text(out).replace("\r", "")
        check("exec-child-reads-stdin", txt.strip() == "secondP", repr(txt))
        check("exec-child-stdin-no-echo", "first" not in txt, repr(txt))

        rc, out = run(mue, T_STDIN_SILENT, d, stdin_data=b"abc")
        txt = guest_text(out).replace("\r", "")
        check("stdin-redirect-not-echoed", txt.strip() == "K", repr(txt))

        rc, out = run(mue, T_STDIN_LINE_SILENT, d, stdin_data=b"abc\r")
        txt = guest_text(out).replace("\r", "")
        check("stdin-line-not-echoed", txt.strip() == "K", repr(txt))

        rc, out = run(mue, T_STDIN_STATUS, d, stdin_data=b"x")
        check("stdin-status-redirected-ready",
              (reg(out, "BP") or 0) & 0xFF == 0xFF, reg(out, "BP"))

        # 4Bh of an MZ image: the header's CS:IP (code at module offset 0x10)
        # and SS:SP (SP low nibble) come through, the relocation makes the
        # first immediate the load segment so DS:0 prints 'MZ', and AX is the
        # child's exit code 7.
        with open(os.path.join(d, "mzc.asm"), "w") as f:
            f.write(MZCHILD_ASM)
        subprocess.run([mue, "-a", "mzc.asm", "mzc.bin"], cwd=d,
                       capture_output=True, timeout=60)
        module = open(os.path.join(d, "mzc.bin"), "rb").read()
        with open(os.path.join(d, "mzch.exe"), "wb") as f:
            f.write(mz_image(module))
        with open(os.path.join(d, "big.exe"), "wb") as f:
            f.write(mz_image(module, minalloc=0x1000))
        # maxalloc caps the block: 0x110 paragraphs past the load segment, so
        # the child's top of memory is 0xF120 instead of the whole window.
        with open(os.path.join(d, "maxa.asm"), "w") as f:
            f.write(MAXA_ASM)
        subprocess.run([mue, "-a", "maxa.asm", "maxa.bin"], cwd=d,
                       capture_output=True, timeout=60)
        maxa = open(os.path.join(d, "maxa.bin"), "rb").read()
        with open(os.path.join(d, "maxa.exe"), "wb") as f:
            f.write(mz_image(maxa, maxalloc=0x110, cs=0, reloc=False))

        rc, out = run(mue, T_EXEC_MZ, d)
        check("exec-mz-loads",
              guest_text(out).replace("\r", "").strip() == "MZ7P",
              repr(guest_text(out)[:60]))
        check("exec-mz-returns-child-code", (reg(out, "SI") or 0) & 0xFF == 7,
              reg(out, "SI"))

        rc, out = run(mue, T_EXEC_MZ_MEM, d)
        check("exec-mz-insufficient-memory", (reg(out, "BP") or 0) & 0xFF == 8,
              reg(out, "BP"))

        # Writer: a `.model` source without `.stack` must still get a CS that
        # points at the code and an @data word that starts at 0.
        with open(os.path.join(d, "mzm.asm"), "w") as f:
            f.write(MZMODEL_ASM)
        subprocess.run([mue, "-a", "mzm.asm", "mzm.exe"], cwd=d,
                       capture_output=True, timeout=60)
        raw = open(os.path.join(d, "mzm.exe"), "rb").read()
        hdr = int.from_bytes(raw[0x08:0x0A], "little") * 16
        cs = int.from_bytes(raw[0x16:0x18], "little")
        nrel = int.from_bytes(raw[0x06:0x08], "little")
        rel = int.from_bytes(raw[0x18:0x1A], "little")
        roff = int.from_bytes(raw[rel:rel + 2], "little")
        rseg = int.from_bytes(raw[rel + 2:rel + 4], "little")
        at = hdr + (rseg << 4) + roff
        check("mz-writer-no-stack-cs",
              cs != 0 and raw[hdr + (cs << 4)] == 0xB8, (cs, raw[hdr:][:2]))
        check("mz-writer-no-stack-data",
              nrel == 1 and raw[at - 1] == 0xB8 and
              int.from_bytes(raw[at:at + 2], "little") == 0,
              (nrel, roff, rseg, raw[at - 1:at + 2]))

        rc, out = run(mue, T_EXEC_MAXA, d)
        check("exec-mz-maxalloc", (reg(out, "BP") or 0) == 0xF1, reg(out, "BP"))

        rc, out = run(mue, T_EXEC_MZ_MODEL, d)
        check("exec-mz-model-child", "MZDATA" in guest_text(out),
              repr(guest_text(out)[:40]))

        # 4Bh AL=1: an overlay lands where the block asks and is not run; the
        # caller enters it itself, and its @data relocation resolves to the
        # load segment (else it would print from segment 0).
        with open(os.path.join(d, "ovl.asm"), "w") as f:
            f.write(OVL_ASM)
        subprocess.run([mue, "-a", "ovl.asm", "ovl.exe"], cwd=d,
                       capture_output=True, timeout=60)
        rc, out = run(mue, T_OVERLAY, d)
        txt = guest_text(out).replace("\r", "").strip()
        check("exec-overlay-loads", reg(out, "BP") == 0, reg(out, "BP"))
        check("exec-overlay-does-not-run", txt.startswith("LO"), repr(txt[:20]))
        check("exec-overlay-runs-where-asked", txt == "LOP", repr(txt[:20]))

        rc, out = run(mue, T_OVERLAY_NOADDR, d)
        check("exec-overlay-needs-address",
              (reg(out, "BP") or 0) == 0x109, reg(out, "BP"))

        # A child that leaves through 31h: its AL must reach the 4Bh caller.
        with open(os.path.join(d, "tsr.asm"), "w") as f:
            f.write(CHILD_TSR_ASM)
        subprocess.run([mue, "-a", "tsr.asm", "tsr.com"], cwd=d,
                       capture_output=True, timeout=60)
        rc, out = run(mue, T_EXEC_TSR, d)
        check("tsr-code-reaches-exec-caller",
              reg(out, "BP") == 5 and "child" in guest_text(out),
              (reg(out, "BP"), repr(guest_text(out))))

        # The same 48h guest under the original's no-op switch and without it:
        # the flag must change the answer, so both models are testable.
        rc, out = run(mue, T_MEM_NOOP, d, env={"MUE_INT21_MEM_NOOP": "1"})
        check("mem-noop-compat-flag",
              reg(out, "AX") == 0x4800 and reg(out, "BX") == 0x5678 and
              (reg(out, "DI") or 0) & 1,
              (reg(out, "AX"), reg(out, "BX"), reg(out, "DI")))
        rc, out = run(mue, T_MEM_NOOP, d)
        check("mem-noop-off-by-default",
              reg(out, "AX") == 0x2000 and (reg(out, "DI") or 0) & 1 == 0,
              (reg(out, "AX"), reg(out, "DI")))

        # 36h free space: constants from the original's inline arm, and the
        # invalid drive answer.
        rc, out = run(mue, T_FREESPACE, d)
        check("freespace-valid-drive",
              reg(out, "BP") == 0x0012 and reg(out, "BX") == 0x0200 and
              reg(out, "DX") == 0xA000,
              (reg(out, "BP"), reg(out, "BX"), reg(out, "DX")))
        check("freespace-leaves-cx", reg(out, "CX") == 0x1234, reg(out, "CX"))
        check("freespace-invalid-drive",
              reg(out, "SI") == 0xFFFF and (reg(out, "DI") or 0) & 1,
              (reg(out, "SI"), reg(out, "DI")))

        # 48h/49h/4Ah: the arena.
        rc, out = run(mue, T_MEMORY, d)
        seg1, seg2 = reg(out, "BP"), reg(out, "SI")
        errs = (reg(out, "CX") or 0) & 0xFF      # the guest's CF error bits
        check("alloc-returns-segment",
              (errs & 3) == 0 and seg1 and seg2, (errs, seg1, seg2))
        check("alloc-blocks-do-not-overlap",
              seg1 == 0x2000 and seg2 == 0x2010, (seg1, seg2))
        check("resize-too-big-fails-with-max",
              reg(out, "DX") == 8 and 0 < (reg(out, "BX") or 0) < 0xFFF0,
              (reg(out, "DX"), reg(out, "BX")))
        check("free-block-and-double-free",
              reg(out, "DI") == 9 and (errs & 0x1C) == 0,
              (reg(out, "DI"), errs))

        # 3Bh + per-drive 47h, driven by a full mkdir/chdir/getdir round trip.
        rc, out = run(mue, T_CHDIR, d)
        lines = [l.strip() for l in guest_text(out).splitlines() if l.strip()]
        inside = lines[0] if lines else ""
        back = lines[1] if len(lines) > 1 else ""
        check("chdir-relative-enters",
              inside.split("\\")[-1] == "rt1", repr(inside))
        check("chdir-parent-leaves",
              back.split("\\")[-1] != "rt1" and back == inside.rsplit("\\", 1)[0],
              repr((inside, back)))
        check("chdir-missing-dir-fails",
              (reg(out, "BX") or 0) & 0xFFFF == 3 and (reg(out, "BP") or 0) & 1,
              (reg(out, "BX"), reg(out, "BP")))
        # The guest rmdir'd from the parent, so the directory must be gone.
        check("chdir-round-trip-cleans-up", not os.path.isdir(os.path.join(d, "rt1")), None)

        # 39h/3Ah: the two functions the shipped examples call that were
        # missing (examples/file-operations.asm).  DOS answers 5 for "already
        # exists" / "not empty" and 3 for "no such path".
        rc, out = run(mue, T_MKDIR, d)
        si = reg(out, "SI")
        di = reg(out, "DI")
        bp = reg(out, "BP")
        check("mkdir-creates-directory", os.path.isdir(os.path.join(d, "mk_a")),
              os.listdir(d))
        check("mkdir-cf-clear", si is not None and not (si & 1), si)
        check("mkdir-exists-is-denied", di == 5, di)
        check("mkdir-exists-cf", bp is not None and (bp & 1), bp)

        rc, out = run(mue, T_RMDIR, d)
        si = reg(out, "SI")
        di = reg(out, "DI")
        bp = reg(out, "BP")
        check("rmdir-removes-directory", not os.path.exists(os.path.join(d, "mk_a")),
              os.listdir(d))
        check("rmdir-cf-clear", si is not None and not (si & 1), si)
        check("rmdir-missing-not-found", di == 3, di)
        check("rmdir-missing-cf", bp is not None and (bp & 1), bp)

        rc, out = run(mue, T_INT16_PEEK, d, b"K")
        bx = reg(out, "BX")
        cx = reg(out, "CX")
        di = reg(out, "DI")
        check("int16-peek-returns-key", bx == 0x4B00,
              hex(bx) if bx is not None else None)
        check("int16-peek-zf-clear", di is not None and not (di & 0x40), di)
        check("int16-peek-does-not-consume", cx == 0x4B00,
              hex(cx) if cx is not None else None)

        rc, out = run(mue, T_INT16_NONE, d, b"")
        bx = reg(out, "BX")
        di = reg(out, "DI")
        check("int16-no-input-returns-zero", bx == 0, bx)
        check("int16-no-input-sets-zf", di is not None and (di & 0x40), di)

        rc, out = run(mue, T_INT21_01, d, b"QZ")
        txt = guest_text(out)
        check("int21-01-reads-in-order", txt.strip() == "QZ", repr(txt))

        rc, out = run(mue, T_STDOUT, d)
        txt = guest_text(out)
        check("stdout-handle-writes", "ok" in txt, repr(txt))

    print("INT21 %s (%d fail)" % ("PASS" if not fails else "FAIL", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
