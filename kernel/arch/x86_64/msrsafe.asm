; Guarded rdmsr (see kernel/include/msrsafe.h).
;
; The table at msrsafe_table lists (address of an instruction that may fault, address to continue at). msrsafe_gp_stub is what vector 13
; of the IDT points at once msrsafe_init has run: it looks at the faulting address, and for one in the table it rewrites the saved
; return address and returns from the interrupt (the faulting function then sees its fixup path); for any other #GP it jumps to the
; handler that was there before (msrsafe_chain), with the stack exactly as the CPU left it.
bits 64
default rel

section .text

global msrsafe_rdmsr
global msrsafe_poison
global msrsafe_gp_stub

; int msrsafe_rdmsr(u32 msr /* edi */, u64 *out /* rsi */)   ->  0, or -1 when the CPU raised #GP
msrsafe_rdmsr:
    mov ecx, edi
msrsafe_rdmsr_insn:
    rdmsr
    shl rdx, 32
    or  rax, rdx
    mov [rsi], rax
    xor eax, eax
    ret
msrsafe_rdmsr_fixup:
    mov eax, -1
    ret

; int msrsafe_poison(void)  ->  1 (a #GP that was caught). Loads a segment register with a selector far beyond the end of the GDT, which
; raises #GP on every x86 CPU and every emulator; the same trick as the kernel's crash=gp. The test of the handler itself.
msrsafe_poison:
    mov eax, 0xFFF8
msrsafe_poison_insn:
    mov es, ax
    xor eax, eax                ; (not reached)
    ret
msrsafe_poison_fixup:
    mov eax, 1
    ret

; The stub. On entry the CPU has pushed (from the top) the error code, RIP, CS, RFLAGS, RSP, SS.
msrsafe_gp_stub:
    push rax
    push rcx
    push rdx
    mov  rax, [rsp + 32]        ; the faulting RIP (three saved registers and the error code are above it)
    lea  rcx, [rel msrsafe_table]
.scan:
    mov  rdx, [rcx]
    test rdx, rdx
    jz   .not_ours
    cmp  rdx, rax
    je   .ours
    add  rcx, 16
    jmp  .scan
.ours:
    mov  rdx, [rcx + 8]         ; where to carry on
    mov  [rsp + 32], rdx
    pop  rdx
    pop  rcx
    pop  rax
    add  rsp, 8                 ; the error code
    iretq
.not_ours:
    pop  rdx
    pop  rcx
    pop  rax
    jmp  [rel msrsafe_chain]    ; the kernel's own #GP stub, with the error code still on the stack

section .rodata
align 8
msrsafe_table:
    dq msrsafe_rdmsr_insn, msrsafe_rdmsr_fixup
    dq msrsafe_poison_insn, msrsafe_poison_fixup
    dq 0, 0

section .data
align 8
global msrsafe_chain
msrsafe_chain: dq 0
