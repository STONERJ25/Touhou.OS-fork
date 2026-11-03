bits 64
default rel
section .text
    inc rax
    inc r9
    inc rbp
    inc eax
    inc r9d
    inc ebp
    inc ax
    inc r9w
    inc bp
    inc al
    inc r9b
    inc bpl
    inc qword [rax]
    inc dword [rbx + 4]
    inc word [rcx]
    inc byte [rdx + rsi*2]
    dec rax
    dec r9
    dec rbp
    dec eax
    dec r9d
    dec ebp
    dec ax
    dec r9w
    dec bp
    dec al
    dec r9b
    dec bpl
    dec qword [rax]
    dec dword [rbx + 4]
    dec word [rcx]
    dec byte [rdx + rsi*2]
    not rax
    not r9
    not rbp
    not eax
    not r9d
    not ebp
    not ax
    not r9w
    not bp
    not al
    not r9b
    not bpl
    not qword [rax]
    not dword [rbx + 4]
    not word [rcx]
    not byte [rdx + rsi*2]
    neg rax
    neg r9
    neg rbp
    neg eax
    neg r9d
    neg ebp
    neg ax
    neg r9w
    neg bp
    neg al
    neg r9b
    neg bpl
    neg qword [rax]
    neg dword [rbx + 4]
    neg word [rcx]
    neg byte [rdx + rsi*2]
    mul rax
    mul r9
    mul rbp
    mul eax
    mul r9d
    mul ebp
    mul ax
    mul r9w
    mul bp
    mul al
    mul r9b
    mul bpl
    mul qword [rax]
    mul dword [rbx + 4]
    mul word [rcx]
    mul byte [rdx + rsi*2]
    div rax
    div r9
    div rbp
    div eax
    div r9d
    div ebp
    div ax
    div r9w
    div bp
    div al
    div r9b
    div bpl
    div qword [rax]
    div dword [rbx + 4]
    div word [rcx]
    div byte [rdx + rsi*2]
    idiv rax
    idiv r9
    idiv rbp
    idiv eax
    idiv r9d
    idiv ebp
    idiv ax
    idiv r9w
    idiv bp
    idiv al
    idiv r9b
    idiv bpl
    idiv qword [rax]
    idiv dword [rbx + 4]
    idiv word [rcx]
    idiv byte [rdx + rsi*2]
    imul rax
    imul r9
    imul rbp
    imul eax
    imul r9d
    imul ebp
    imul ax
    imul r9w
    imul bp
    imul al
    imul r9b
    imul bpl
    imul qword [rax]
    imul dword [rbx + 4]
    imul word [rcx]
    imul byte [rdx + rsi*2]
    imul rax, rbx
    imul eax, [rcx]
    imul r9, [rsp + 8]
    imul rax, rbx, 10
    imul rax, rbx, 1000
    imul eax, ecx, -3
    imul r8, [rdi], 100000
    shl rax, 1
    shl rbx, 5
    shl r9, cl
    shl eax, 1
    shl ebx, 5
    shl r9d, cl
    shl ax, 1
    shl bx, 5
    shl r9w, cl
    shl al, 1
    shl bl, 5
    shl r9b, cl
    shl qword [rax], 3
    shl dword [rax], 1
    shl byte [rax], cl
    shr rax, 1
    shr rbx, 5
    shr r9, cl
    shr eax, 1
    shr ebx, 5
    shr r9d, cl
    shr ax, 1
    shr bx, 5
    shr r9w, cl
    shr al, 1
    shr bl, 5
    shr r9b, cl
    shr qword [rax], 3
    shr dword [rax], 1
    shr byte [rax], cl
    sar rax, 1
    sar rbx, 5
    sar r9, cl
    sar eax, 1
    sar ebx, 5
    sar r9d, cl
    sar ax, 1
    sar bx, 5
    sar r9w, cl
    sar al, 1
    sar bl, 5
    sar r9b, cl
    sar qword [rax], 3
    sar dword [rax], 1
    sar byte [rax], cl
    sal rax, 1
    sal rbx, 5
    sal r9, cl
    sal eax, 1
    sal ebx, 5
    sal r9d, cl
    sal ax, 1
    sal bx, 5
    sal r9w, cl
    sal al, 1
    sal bl, 5
    sal r9b, cl
    sal qword [rax], 3
    sal dword [rax], 1
    sal byte [rax], cl
    rol rax, 1
    rol rbx, 5
    rol r9, cl
    rol eax, 1
    rol ebx, 5
    rol r9d, cl
    rol ax, 1
    rol bx, 5
    rol r9w, cl
    rol al, 1
    rol bl, 5
    rol r9b, cl
    rol qword [rax], 3
    rol dword [rax], 1
    rol byte [rax], cl
    ror rax, 1
    ror rbx, 5
    ror r9, cl
    ror eax, 1
    ror ebx, 5
    ror r9d, cl
    ror ax, 1
    ror bx, 5
    ror r9w, cl
    ror al, 1
    ror bl, 5
    ror r9b, cl
    ror qword [rax], 3
    ror dword [rax], 1
    ror byte [rax], cl
    rcl rax, 1
    rcl rbx, 5
    rcl r9, cl
    rcl eax, 1
    rcl ebx, 5
    rcl r9d, cl
    rcl ax, 1
    rcl bx, 5
    rcl r9w, cl
    rcl al, 1
    rcl bl, 5
    rcl r9b, cl
    rcl qword [rax], 3
    rcl dword [rax], 1
    rcl byte [rax], cl
    rcr rax, 1
    rcr rbx, 5
    rcr r9, cl
    rcr eax, 1
    rcr ebx, 5
    rcr r9d, cl
    rcr ax, 1
    rcr bx, 5
    rcr r9w, cl
    rcr al, 1
    rcr bl, 5
    rcr r9b, cl
    rcr qword [rax], 3
    rcr dword [rax], 1
    rcr byte [rax], cl
    test rax, rax
    test eax, eax
    test al, al
    test ax, ax
    test rbx, rcx
    test [rax], rcx
    test rcx, [rax]
    test eax, 1
    test eax, 0x100
    test rbx, 0x100
    test al, 3
    test bl, 3
    test dword [rax], 5
    test byte [rax], 5
    test sil, sil
    test r9, 0x7fffffff
    movzx eax, bl
    movzx rax, bx
    movzx eax, byte [rcx]
    movzx eax, word [rcx]
    movsx rax, bl
    movsx eax, bx
    movsx r9, byte [rdi]
    movsxd rax, ecx
    movsxd r9, dword [rdi]
    movzx r9d, sil
    movzx eax, ah
    cmove eax, ebx
    cmovne rax, [rcx]
    cmovl r9, r10
    cmova eax, ecx
    cmovbe rax, rdx
    cmovz rax, rbx
    cmovnle rcx, rdx
    sete al
    setne bl
    setl sil
    seta byte [rax]
    setg r9b
    setnz dl
    xchg rax, rbx
    xchg rbx, rax
    xchg eax, ecx
    xchg rcx, rdx
    xchg [rax], rbx
    xchg rbx, [rax]
    xchg al, bl
    xchg ax, bx
    xchg r8, rax
    xadd [rax], rbx
    xadd [rax], ecx
    cmpxchg [rdi], rsi
    cmpxchg [rdi], esi
    lock xadd [rax], rbx
    lock cmpxchg [rdi], rdx
    lock inc qword [rax]
    lock add dword [rax], 1
    bswap eax
    bswap rax
    bswap r9
    bsf rax, rbx
    bsr eax, [rcx]
    popcnt rax, rbx
    popcnt eax, [rcx]
    tzcnt rax, rcx
    lzcnt eax, ecx
    bt rax, rbx
    bt rax, 5
    bts [rax], rbx
    btr qword [rax], 7
    btc ecx, 3
    int 0x80
    int 3
    ret 8
    in al, dx
    in eax, dx
    in al, 0x60
    out dx, al
    out dx, eax
    out 0x21, al
    out 0x80, eax
    rep movsb
    rep movsq
    rep stosb
    rep stosq
    repe cmpsb
    repne scasb
    lodsb
    lodsq
    movsd
    stosw
    nop
