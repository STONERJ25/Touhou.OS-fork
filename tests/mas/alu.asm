bits 64
default rel
section .text
; the two-operand arithmetic group in all sizes and operand shapes
    add rax, r10
    add r10, rax
    add rbx, r13
    add rdi, [rax]
    add [rsp + 8], rcx
    add eax, r10d
    add r10d, eax
    add ebx, r13d
    add edi, [rax]
    add [rsp + 8], ecx
    add ax, r10w
    add r10w, ax
    add bx, r13w
    add di, [rax]
    add [rsp + 8], cx
    add al, r10b
    add r10b, al
    add bl, r13b
    add dil, [rax]
    add [rsp + 8], cl
    add rax, 1
    add rax, -1
    add rax, 127
    add rax, 128
    add rax, 0x12345678
    add rbx, 5
    add rbx, 0x1000
    add r9, -129
    add eax, 5
    add eax, 0x12345678
    add ebx, 0x12345678
    add ecx, -200
    add ax, 5
    add ax, 0x1234
    add bx, 0x1234
    add al, 5
    add bl, 5
    add sil, 5
    add r8b, 255
    add qword [rdi], 5
    add dword [rdi + 4], 0x1000
    add word [rsi], 7
    add byte [rdx], 9
    add qword [rbp - 8], 1000
    add byte [rsp], 255
    or rax, r10
    or r10, rax
    or rbx, r13
    or rdi, [rax]
    or [rsp + 8], rcx
    or eax, r10d
    or r10d, eax
    or ebx, r13d
    or edi, [rax]
    or [rsp + 8], ecx
    or ax, r10w
    or r10w, ax
    or bx, r13w
    or di, [rax]
    or [rsp + 8], cx
    or al, r10b
    or r10b, al
    or bl, r13b
    or dil, [rax]
    or [rsp + 8], cl
    or rax, 1
    or rax, -1
    or rax, 127
    or rax, 128
    or rax, 0x12345678
    or rbx, 5
    or rbx, 0x1000
    or r9, -129
    or eax, 5
    or eax, 0x12345678
    or ebx, 0x12345678
    or ecx, -200
    or ax, 5
    or ax, 0x1234
    or bx, 0x1234
    or al, 5
    or bl, 5
    or sil, 5
    or r8b, 255
    or qword [rdi], 5
    or dword [rdi + 4], 0x1000
    or word [rsi], 7
    or byte [rdx], 9
    or qword [rbp - 8], 1000
    or byte [rsp], 255
    adc rax, r10
    adc r10, rax
    adc rbx, r13
    adc rdi, [rax]
    adc [rsp + 8], rcx
    adc eax, r10d
    adc r10d, eax
    adc ebx, r13d
    adc edi, [rax]
    adc [rsp + 8], ecx
    adc ax, r10w
    adc r10w, ax
    adc bx, r13w
    adc di, [rax]
    adc [rsp + 8], cx
    adc al, r10b
    adc r10b, al
    adc bl, r13b
    adc dil, [rax]
    adc [rsp + 8], cl
    adc rax, 1
    adc rax, -1
    adc rax, 127
    adc rax, 128
    adc rax, 0x12345678
    adc rbx, 5
    adc rbx, 0x1000
    adc r9, -129
    adc eax, 5
    adc eax, 0x12345678
    adc ebx, 0x12345678
    adc ecx, -200
    adc ax, 5
    adc ax, 0x1234
    adc bx, 0x1234
    adc al, 5
    adc bl, 5
    adc sil, 5
    adc r8b, 255
    adc qword [rdi], 5
    adc dword [rdi + 4], 0x1000
    adc word [rsi], 7
    adc byte [rdx], 9
    adc qword [rbp - 8], 1000
    adc byte [rsp], 255
    sbb rax, r10
    sbb r10, rax
    sbb rbx, r13
    sbb rdi, [rax]
    sbb [rsp + 8], rcx
    sbb eax, r10d
    sbb r10d, eax
    sbb ebx, r13d
    sbb edi, [rax]
    sbb [rsp + 8], ecx
    sbb ax, r10w
    sbb r10w, ax
    sbb bx, r13w
    sbb di, [rax]
    sbb [rsp + 8], cx
    sbb al, r10b
    sbb r10b, al
    sbb bl, r13b
    sbb dil, [rax]
    sbb [rsp + 8], cl
    sbb rax, 1
    sbb rax, -1
    sbb rax, 127
    sbb rax, 128
    sbb rax, 0x12345678
    sbb rbx, 5
    sbb rbx, 0x1000
    sbb r9, -129
    sbb eax, 5
    sbb eax, 0x12345678
    sbb ebx, 0x12345678
    sbb ecx, -200
    sbb ax, 5
    sbb ax, 0x1234
    sbb bx, 0x1234
    sbb al, 5
    sbb bl, 5
    sbb sil, 5
    sbb r8b, 255
    sbb qword [rdi], 5
    sbb dword [rdi + 4], 0x1000
    sbb word [rsi], 7
    sbb byte [rdx], 9
    sbb qword [rbp - 8], 1000
    sbb byte [rsp], 255
    and rax, r10
    and r10, rax
    and rbx, r13
    and rdi, [rax]
    and [rsp + 8], rcx
    and eax, r10d
    and r10d, eax
    and ebx, r13d
    and edi, [rax]
    and [rsp + 8], ecx
    and ax, r10w
    and r10w, ax
    and bx, r13w
    and di, [rax]
    and [rsp + 8], cx
    and al, r10b
    and r10b, al
    and bl, r13b
    and dil, [rax]
    and [rsp + 8], cl
    and rax, 1
    and rax, -1
    and rax, 127
    and rax, 128
    and rax, 0x12345678
    and rbx, 5
    and rbx, 0x1000
    and r9, -129
    and eax, 5
    and eax, 0x12345678
    and ebx, 0x12345678
    and ecx, -200
    and ax, 5
    and ax, 0x1234
    and bx, 0x1234
    and al, 5
    and bl, 5
    and sil, 5
    and r8b, 255
    and qword [rdi], 5
    and dword [rdi + 4], 0x1000
    and word [rsi], 7
    and byte [rdx], 9
    and qword [rbp - 8], 1000
    and byte [rsp], 255
    sub rax, r10
    sub r10, rax
    sub rbx, r13
    sub rdi, [rax]
    sub [rsp + 8], rcx
    sub eax, r10d
    sub r10d, eax
    sub ebx, r13d
    sub edi, [rax]
    sub [rsp + 8], ecx
    sub ax, r10w
    sub r10w, ax
    sub bx, r13w
    sub di, [rax]
    sub [rsp + 8], cx
    sub al, r10b
    sub r10b, al
    sub bl, r13b
    sub dil, [rax]
    sub [rsp + 8], cl
    sub rax, 1
    sub rax, -1
    sub rax, 127
    sub rax, 128
    sub rax, 0x12345678
    sub rbx, 5
    sub rbx, 0x1000
    sub r9, -129
    sub eax, 5
    sub eax, 0x12345678
    sub ebx, 0x12345678
    sub ecx, -200
    sub ax, 5
    sub ax, 0x1234
    sub bx, 0x1234
    sub al, 5
    sub bl, 5
    sub sil, 5
    sub r8b, 255
    sub qword [rdi], 5
    sub dword [rdi + 4], 0x1000
    sub word [rsi], 7
    sub byte [rdx], 9
    sub qword [rbp - 8], 1000
    sub byte [rsp], 255
    xor rax, r10
    xor r10, rax
    xor rbx, r13
    xor rdi, [rax]
    xor [rsp + 8], rcx
    xor eax, r10d
    xor r10d, eax
    xor ebx, r13d
    xor edi, [rax]
    xor [rsp + 8], ecx
    xor ax, r10w
    xor r10w, ax
    xor bx, r13w
    xor di, [rax]
    xor [rsp + 8], cx
    xor al, r10b
    xor r10b, al
    xor bl, r13b
    xor dil, [rax]
    xor [rsp + 8], cl
    xor rax, 1
    xor rax, -1
    xor rax, 127
    xor rax, 128
    xor rax, 0x12345678
    xor rbx, 5
    xor rbx, 0x1000
    xor r9, -129
    xor eax, 5
    xor eax, 0x12345678
    xor ebx, 0x12345678
    xor ecx, -200
    xor ax, 5
    xor ax, 0x1234
    xor bx, 0x1234
    xor al, 5
    xor bl, 5
    xor sil, 5
    xor r8b, 255
    xor qword [rdi], 5
    xor dword [rdi + 4], 0x1000
    xor word [rsi], 7
    xor byte [rdx], 9
    xor qword [rbp - 8], 1000
    xor byte [rsp], 255
    cmp rax, r10
    cmp r10, rax
    cmp rbx, r13
    cmp rdi, [rax]
    cmp [rsp + 8], rcx
    cmp eax, r10d
    cmp r10d, eax
    cmp ebx, r13d
    cmp edi, [rax]
    cmp [rsp + 8], ecx
    cmp ax, r10w
    cmp r10w, ax
    cmp bx, r13w
    cmp di, [rax]
    cmp [rsp + 8], cx
    cmp al, r10b
    cmp r10b, al
    cmp bl, r13b
    cmp dil, [rax]
    cmp [rsp + 8], cl
    cmp rax, 1
    cmp rax, -1
    cmp rax, 127
    cmp rax, 128
    cmp rax, 0x12345678
    cmp rbx, 5
    cmp rbx, 0x1000
    cmp r9, -129
    cmp eax, 5
    cmp eax, 0x12345678
    cmp ebx, 0x12345678
    cmp ecx, -200
    cmp ax, 5
    cmp ax, 0x1234
    cmp bx, 0x1234
    cmp al, 5
    cmp bl, 5
    cmp sil, 5
    cmp r8b, 255
    cmp qword [rdi], 5
    cmp dword [rdi + 4], 0x1000
    cmp word [rsi], 7
    cmp byte [rdx], 9
    cmp qword [rbp - 8], 1000
    cmp byte [rsp], 255
