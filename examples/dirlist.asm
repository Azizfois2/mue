; dirlist.asm - list the current directory through INT 21h AH=4Eh/4Fh.
;
; Prints every entry that matches PAT as "<NAME>  <size in hex>", one per
; line, then leaves with INT 21h AH=4Ch.  The directory search reads the
; result from the default DTA (PSP:0080), whose name field is at +1Eh and
; size at +1Ah - the layout the original fills (FINDINGS 19).
;
; Note: 4Eh/4Fh are NOT in emu8086's documented supported-interrupt list, and
; its dispatcher arms never write AX or CF back to the guest (FINDINGS 23),
; so on the real emulator the "no more files" test below cannot fire.
        org 100h

        mov     ah, 4Eh             ; find first
        mov     cx, 0               ; normal files only (no directories)
        lea     dx, pat
        int     21h

next:
        jc      done                ; DOS: CF + AX = 12h when there are none
        or      ax, ax
        jnz     done                ; also stop on a plain error code

        mov     si, 009Eh           ; DTA + 1Eh: ASCIZ name of the match
        call    print_string

        mov     dl, ' '
        mov     ah, 02h
        int     21h
        mov     dl, ' '
        int     21h

        mov     ax, [009Ah]         ; DTA + 1Ah: size, low word
        call    print_hex4

        mov     dl, 13
        mov     ah, 02h
        int     21h
        mov     dl, 10
        int     21h

        mov     ah, 4Fh             ; find next
        int     21h
        jmp     next

done:
        mov     ax, 4C00h
        int     21h

; ---- print the ASCIZ string at DS:SI ---------------------------------
print_string:
        push    ax
        push    si
        push    dx
ps_loop:
        lodsb
        or      al, al
        jz      ps_done
        mov     dl, al
        mov     ah, 02h
        int     21h
        jmp     ps_loop
ps_done:
        pop     dx
        pop     si
        pop     ax
        ret

; ---- print AX as 4 hex digits ----------------------------------------
print_hex4:
        push    ax
        push    cx
        push    dx
        mov     cx, 4
ph_loop:
        rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     ph_out
        add     al, 7               ; 'A'..'F'
ph_out:
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     ax
        dec     cx
        jnz     ph_loop
        pop     dx
        pop     cx
        pop     ax
        ret

pat     db '*.*', 0
