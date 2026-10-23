; Kernel context switch and the first-run trampoline for new tasks.
bits 64
default rel

extern task_first_run
extern task_exit_current

section .text

; void context_switch(u64 *old_rsp, u64 new_rsp)
global context_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; New tasks "return" here from context_switch with r12 = entry function, r13 = argument.
global task_entry_trampoline
task_entry_trampoline:
    call task_first_run
    sti
    mov rdi, r13
    call r12
    xor edi, edi
    call task_exit_current
    ud2
