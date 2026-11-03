bits 64
section .text
global start
start:
top:
    nop
    jo top
    jo fwd1
    jno top
    jno fwd1
    jb top
    jb fwd1
    jc top
    jc fwd1
    jnae top
    jnae fwd1
    jae top
    jae fwd1
    jnc top
    jnc fwd1
    jnb top
    jnb fwd1
    je top
    je fwd1
    jz top
    jz fwd1
    jne top
    jne fwd1
    jnz top
    jnz fwd1
    jbe top
    jbe fwd1
    jna top
    jna fwd1
    ja top
    ja fwd1
    jnbe top
    jnbe fwd1
    js top
    js fwd1
    jns top
    jns fwd1
    jp top
    jp fwd1
    jpe top
    jpe fwd1
    jnp top
    jnp fwd1
    jpo top
    jpo fwd1
    jl top
    jl fwd1
    jnge top
    jnge fwd1
    jge top
    jge fwd1
    jnl top
    jnl fwd1
    jle top
    jle fwd1
    jng top
    jng fwd1
    jg top
    jg fwd1
    jnle top
    jnle fwd1
    jmp top
    jmp fwd1
    jmp short fwd1
    jmp near fwd1
    call top
    call fwd1
    call extern_fn
    jmp extern_fn
    je extern_fn
    call rax
    call r12
    call [rax]
    call [rel top]
    jmp rax
    jmp r15
    jmp [rsp + 8]
fwd1:
    nop
    times 130 nop
    jmp top
    jne top
    jmp fwd2
    jnz fwd2
    times 126 nop
    jmp fwd3
    times 129 nop
fwd2:
    nop
fwd3:
    jmp fwd3
    jmp near fwd3
extern extern_fn
section .text2 progbits alloc exec nowrite align=16
other: nop
    call start
    jmp start
    jmp other
    jz other
