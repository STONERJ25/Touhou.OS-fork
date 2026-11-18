; Interrupt/exception entry stubs: one per vector, all funnelled through isr_common into trap_handler(TrapFrame*).
bits 64
default rel

extern trap_handler
extern g_smap_enabled

section .text

%macro ISR_NOERR 1
isr_stub_%+%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%+%1:
    push qword %1
    jmp isr_common
%endmacro

align 16
isr_common:
    ; SMAP: clear EFLAGS.AC before any handler runs (Intel SDM vol. 3A, 4.6.1). The CPU does not clear it for an interrupt or an exception, so without this a
    ; program that sets AC in its own flags (any ring may) would have the kernel run with SMAP switched off, and an interrupt that arrives inside the copy
    ; window of uaccess.asm would run its handler with the window open. The interrupted code gets its own AC back from the saved flags at iretq. CLAC is #UD
    ; on a CPU without SMAP (or with "smap=off"): hence the flag. Only the flags register is touched, and the CPU has saved the interrupted one.
    cmp byte [g_smap_enabled], 0
    je .smap_done
    clac
.smap_done:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    cld
    call trap_handler
    global isr_return
isr_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16             ; vector + error code
    iretq

%assign i 0
%rep 256
%if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
%else
    ISR_NOERR i
%endif
%assign i i+1
%endrep

section .rodata
align 8
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
